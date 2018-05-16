/*
 * Copyright (c) 2013-2018 Marc André Tanner <mat at brain-dump.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <libgen.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#if defined(__linux__) || defined(__CYGWIN__)
# include <pty.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
# include <libutil.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
# include <util.h>
#endif

#if defined CTRL && defined _AIX
  #undef CTRL
#endif
#ifndef CTRL
  #define CTRL(k)   ((k) & 0x1F)
#endif

#include "config.h"

#if defined(_AIX)
# include "forkpty-aix.c"
#elif defined(__sun)
# include "forkpty-sunos.c"
#endif

#define countof(arr) (sizeof(arr) / sizeof((arr)[0]))

enum PacketType {
	MSG_CONTENT = 0,
	MSG_ATTACH  = 1,
	MSG_DETACH  = 2,
	MSG_RESIZE  = 3,
	MSG_EXIT    = 4,
	MSG_PID     = 5,
};

typedef struct {
	uint32_t type;
	uint32_t len;
	union {
		char msg[4096 - 2*sizeof(uint32_t)];
		struct {
			uint16_t rows;
			uint16_t cols;
		} ws;
		uint32_t i;
		uint64_t l;
	} u;
} Packet;

typedef struct Client Client;
struct Client {
	int socket;
	enum {
		STATE_CONNECTED,
		STATE_ATTACHED,
		STATE_DETACHED,
		STATE_DISCONNECTED,
	} state;
	bool need_resize;
	enum {
		CLIENT_READONLY = 1 << 0,
		CLIENT_LOWPRIORITY = 1 << 1,
	} flags;
	Client *next;
};

typedef struct {
	Client *clients;
	int socket;
	Packet pty_output;
	int pty;
	int exit_status;
	struct termios term;
	struct winsize winsize;
	pid_t pid;
	volatile sig_atomic_t running;
	const char *name;
	const char *session_name;
	char host[255];
	bool read_pty;
} Server;

static Server server = { .running = true, .exit_status = -1, .host = "@localhost" };
static Client client;
static struct termios orig_term, cur_term;
static bool has_term, alternate_buffer, quiet, passthrough;

static struct sockaddr_un sockaddr = {
	.sun_family = AF_UNIX,
};

static bool set_socket_name(struct sockaddr_un *sockaddr, const char *name);
static void die(const char *s);
static void info(const char *str, ...);

#include "debug.c"

static inline size_t packet_header_size() {
	return offsetof(Packet, u);
}

static size_t packet_size(Packet *pkt) {
	return packet_header_size() + pkt->len;
}

static ssize_t write_all(int fd, const char *buf, size_t len) {
	debug("write_all(%d)\n", len);
	ssize_t ret = len;
	while (len > 0) {
		ssize_t res = write(fd, buf, len);
		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				continue;
			return -1;
		}
		if (res == 0)
			return ret - len;
		buf += res;
		len -= res;
	}
	return ret;
}

static ssize_t read_all(int fd, char *buf, size_t len) {
	debug("read_all(%d)\n", len);
	ssize_t ret = len;
	while (len > 0) {
		ssize_t res = read(fd, buf, len);
		if (res < 0) {
			if (errno == EWOULDBLOCK)
				return ret - len;
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return -1;
		}
		if (res == 0)
			return ret - len;
		buf += res;
		len -= res;
	}
	return ret;
}

static bool send_packet(int socket, Packet *pkt) {
	size_t size = packet_size(pkt);
	if (size > sizeof(*pkt))
		return false;
	return write_all(socket, (char *)pkt, size) == size;
}

static bool recv_packet(int socket, Packet *pkt) {
	ssize_t len = read_all(socket, (char*)pkt, packet_header_size());
	if (len <= 0 || len != packet_header_size())
		return false;
	if (pkt->len > sizeof(pkt->u.msg)) {
		pkt->len = 0;
		return false;
	}
	if (pkt->len > 0) {
		len = read_all(socket, pkt->u.msg, pkt->len);
		if (len <= 0 || len != pkt->len)
			return false;
	}
	return true;
}

#include "client.c"
#include "server.c"

static void info(const char *str, ...) {
	va_list ap;
	va_start(ap, str);
	if (str && !quiet) {
		fprintf(stderr, "%s: %s: ", server.name, server.session_name);
		vfprintf(stderr, str, ap);
		fprintf(stderr, "\r\n");
		fflush(stderr);
	}
	va_end(ap);
}

static void die(const char *s) {
	perror(s);
	exit(EXIT_FAILURE);
}

static void usage(void) {
	fprintf(stderr, "usage: abduco [-a|-A|-c|-n] [-p] [-r] [-q] [-l] [-f] [-e detachkey] name command\n");
	exit(EXIT_FAILURE);
}

static bool xsnprintf(char *buf, size_t size, const char *fmt, ...) {
	va_list ap;
	if (size > INT_MAX)
		return false;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (n == -1)
		return false;
	if (n >= size) {
		errno = ENAMETOOLONG;
		return false;
	}
	return true;
}

static int session_connect(const char *name) {
	int fd;
	struct stat sb;
	if (!set_socket_name(&sockaddr, name) || (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;
	socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
	if (connect(fd, (struct sockaddr*)&sockaddr, socklen) == -1) {
		if (errno == ECONNREFUSED && stat(sockaddr.sun_path, &sb) == 0 && S_ISSOCK(sb.st_mode))
			unlink(sockaddr.sun_path);
		close(fd);
		return -1;
	}
	return fd;
}

static pid_t session_exists(const char *name) {
	Packet pkt;
	pid_t pid = 0;
	if ((server.socket = session_connect(name)) == -1)
		return pid;
	if (client_recv_packet(&pkt) && pkt.type == MSG_PID)
		pid = pkt.u.l;
	close(server.socket);
	return pid;
}

static bool session_alive(const char *name) {
	struct stat sb;
	return session_exists(name) &&
	       stat(sockaddr.sun_path, &sb) == 0 &&
	       S_ISSOCK(sb.st_mode) && (sb.st_mode & S_IXGRP) == 0;
}

static bool create_socket_dir(struct sockaddr_un *sockaddr) {
	sockaddr->sun_path[0] = '\0';
	int socketfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socketfd == -1)
		return false;

	const size_t maxlen = sizeof(sockaddr->sun_path);
	uid_t uid = getuid();
	struct passwd *pw = getpwuid(uid);

	for (unsigned int i = 0; i < countof(socket_dirs); i++) {
		struct stat sb;
		struct Dir *dir = &socket_dirs[i];
		bool ishome = false;
		if (dir->env) {
			dir->path = getenv(dir->env);
			ishome = !strcmp(dir->env, "HOME");
			if (ishome && (!dir->path || !dir->path[0]) && pw)
				dir->path = pw->pw_dir;
		}
		if (!dir->path || !dir->path[0])
			continue;
		if (!xsnprintf(sockaddr->sun_path, maxlen, "%s/%s%s/", dir->path, ishome ? "." : "", server.name))
			continue;
		mode_t mask = umask(0);
		int r = mkdir(sockaddr->sun_path, dir->personal ? S_IRWXU : S_IRWXU|S_IRWXG|S_IRWXO|S_ISVTX);
		umask(mask);
		if (r != 0 && errno != EEXIST)
			continue;
		if (lstat(sockaddr->sun_path, &sb) != 0)
			continue;
		if (!S_ISDIR(sb.st_mode)) {
			errno = ENOTDIR;
			continue;
		}

		size_t dirlen = strlen(sockaddr->sun_path);
		if (!dir->personal) {
			/* create subdirectory only accessible to user */
			if (pw && !xsnprintf(sockaddr->sun_path+dirlen, maxlen-dirlen, "%s/", pw->pw_name))
				continue;
			if (!pw && !xsnprintf(sockaddr->sun_path+dirlen, maxlen-dirlen, "%d/", uid))
				continue;
			if (mkdir(sockaddr->sun_path, S_IRWXU) != 0 && errno != EEXIST)
				continue;
			if (lstat(sockaddr->sun_path, &sb) != 0)
				continue;
			if (!S_ISDIR(sb.st_mode)) {
				errno = ENOTDIR;
				continue;
			}
			dirlen = strlen(sockaddr->sun_path);
		}

		if (sb.st_uid != uid || sb.st_mode & (S_IRWXG|S_IRWXO)) {
			errno = EACCES;
			continue;
		}

		if (!xsnprintf(sockaddr->sun_path+dirlen, maxlen-dirlen, ".abduco-%d", getpid()))
			continue;

		socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr->sun_path) + 1;
		if (bind(socketfd, (struct sockaddr*)sockaddr, socklen) == -1)
			continue;
		unlink(sockaddr->sun_path);
		close(socketfd);
		sockaddr->sun_path[dirlen] = '\0';
		return true;
	}

	close(socketfd);
	return false;
}

static bool set_socket_name(struct sockaddr_un *sockaddr, const char *name) {
	const size_t maxlen = sizeof(sockaddr->sun_path);
	const char *session_name = NULL;
	char buf[maxlen];

	if (name[0] == '/') {
		if (strlen(name) >= maxlen) {
			errno = ENAMETOOLONG;
			return false;
		}
		strncpy(sockaddr->sun_path, name, maxlen);
	} else if (name[0] == '.' && (name[1] == '.' || name[1] == '/')) {
		char *cwd = getcwd(buf, sizeof buf);
		if (!cwd)
			return false;
		if (!xsnprintf(sockaddr->sun_path, maxlen, "%s/%s", cwd, name))
			return false;
	} else {
		if (!create_socket_dir(sockaddr))
			return false;
		if (strlen(sockaddr->sun_path) + strlen(name) + strlen(server.host) >= maxlen) {
			errno = ENAMETOOLONG;
			return false;
		}
		session_name = name;
		strncat(sockaddr->sun_path, name, maxlen - strlen(sockaddr->sun_path) - 1);
		strncat(sockaddr->sun_path, server.host, maxlen - strlen(sockaddr->sun_path) - 1);
	}

	if (!session_name) {
		strncpy(buf, sockaddr->sun_path, sizeof buf);
		session_name = basename(buf);
	}
	setenv("ABDUCO_SESSION", session_name, 1);
	setenv("ABDUCO_SOCKET", sockaddr->sun_path, 1);

	return true;
}

static bool create_session(const char *name, char * const argv[]) {
	/* this uses the well known double fork strategy as described in section 1.7 of
	 *
	 *  http://www.faqs.org/faqs/unix-faq/programmer/faq/
	 *
	 * pipes are used for synchronization and error reporting i.e. the child sets
	 * the close on exec flag before calling execvp(3) the parent blocks on a read(2)
	 * in case of failure the error message is written to the pipe, success is
	 * indicated by EOF on the pipe.
	 */
	int client_pipe[2], server_pipe[2];
	pid_t pid;
	char errormsg[255];
	struct sigaction sa;

	if (session_exists(name)) {
		errno = EADDRINUSE;
		return false;
	}

	if (pipe(client_pipe) == -1)
		return false;
	if ((server.socket = server_create_socket(name)) == -1)
		return false;

	switch ((pid = fork())) {
	case 0: /* child process */
		setsid();
		close(client_pipe[0]);
		switch ((pid = fork())) {
		case 0: /* child process */
			if (pipe(server_pipe) == -1) {
				snprintf(errormsg, sizeof(errormsg), "server-pipe: %s\n", strerror(errno));
				write_all(client_pipe[1], errormsg, strlen(errormsg));
				close(client_pipe[1]);
				_exit(EXIT_FAILURE);
			}
			sa.sa_flags = 0;
			sigemptyset(&sa.sa_mask);
			sa.sa_handler = server_pty_died_handler;
			sigaction(SIGCHLD, &sa, NULL);
			switch (server.pid = forkpty(&server.pty, NULL, has_term ? &server.term : NULL, &server.winsize)) {
			case 0: /* child = user application process */
				close(server.socket);
				close(server_pipe[0]);
				if (fcntl(client_pipe[1], F_SETFD, FD_CLOEXEC) == 0 &&
				    fcntl(server_pipe[1], F_SETFD, FD_CLOEXEC) == 0)
					execvp(argv[0], argv);
				snprintf(errormsg, sizeof(errormsg), "server-execvp: %s: %s\n",
						 argv[0], strerror(errno));
				write_all(client_pipe[1], errormsg, strlen(errormsg));
				write_all(server_pipe[1], errormsg, strlen(errormsg));
				close(client_pipe[1]);
				close(server_pipe[1]);
				_exit(EXIT_FAILURE);
				break;
			case -1: /* forkpty failed */
				snprintf(errormsg, sizeof(errormsg), "server-forkpty: %s\n", strerror(errno));
				write_all(client_pipe[1], errormsg, strlen(errormsg));
				close(client_pipe[1]);
				close(server_pipe[0]);
				close(server_pipe[1]);
				_exit(EXIT_FAILURE);
				break;
			default: /* parent = server process */
				sa.sa_handler = server_sigterm_handler;
				sigaction(SIGTERM, &sa, NULL);
				sigaction(SIGINT, &sa, NULL);
				sa.sa_handler = server_sigusr1_handler;
				sigaction(SIGUSR1, &sa, NULL);
				sa.sa_handler = SIG_IGN;
				sigaction(SIGPIPE, &sa, NULL);
				sigaction(SIGHUP, &sa, NULL);
				chdir("/");
			#ifdef NDEBUG
				int fd = open("/dev/null", O_RDWR);
				if (fd != -1) {
					dup2(fd, STDIN_FILENO);
					dup2(fd, STDOUT_FILENO);
					dup2(fd, STDERR_FILENO);
					close(fd);
				}
			#endif /* NDEBUG */
				close(client_pipe[1]);
				close(server_pipe[1]);
				if (read_all(server_pipe[0], errormsg, sizeof(errormsg)) > 0)
					_exit(EXIT_FAILURE);
				close(server_pipe[0]);
				server_mainloop();
				break;
			}
			break;
		case -1: /* fork failed */
			snprintf(errormsg, sizeof(errormsg), "server-fork: %s\n", strerror(errno));
			write_all(client_pipe[1], errormsg, strlen(errormsg));
			close(client_pipe[1]);
			_exit(EXIT_FAILURE);
			break;
		default: /* parent = intermediate process */
			close(client_pipe[1]);
			_exit(EXIT_SUCCESS);
			break;
		}
		break;
	case -1: /* fork failed */
		close(client_pipe[0]);
		close(client_pipe[1]);
		return false;
	default: /* parent = client process */
		close(client_pipe[1]);
		int status;
		wait(&status); /* wait for first fork */
		ssize_t len = read_all(client_pipe[0], errormsg, sizeof(errormsg));
		if (len > 0) {
			write_all(STDERR_FILENO, errormsg, len);
			unlink(sockaddr.sun_path);
			exit(EXIT_FAILURE);
		}
		close(client_pipe[0]);
	}
	return true;
}

static bool attach_session(const char *name, const bool terminate) {
	if (server.socket > 0)
		close(server.socket);
	if ((server.socket = session_connect(name)) == -1)
		return false;
	if (server_set_socket_non_blocking(server.socket) == -1)
		return false;

	struct sigaction sa;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = client_sigwinch_handler;
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	client_setup_terminal();
	int status = client_mainloop();
	client_restore_terminal();
	if (status == -1) {
		info("detached");
	} else if (status == -EIO) {
		info("exited due to I/O errors");
	} else {
		info("session terminated with exit status %d", status);
		if (terminate)
			exit(status);
	}

	return terminate;
}

static int session_filter(const struct dirent *d) {
	return strstr(d->d_name, server.host) != NULL;
}

static int session_comparator(const struct dirent **a, const struct dirent **b) {
	struct stat sa, sb;
	if (stat((*a)->d_name, &sa) != 0)
		return -1;
	if (stat((*b)->d_name, &sb) != 0)
		return 1;
	return sa.st_atime < sb.st_atime ? -1 : 1;
}

static int list_session(void) {
	if (!create_socket_dir(&sockaddr))
		return 1;
	if (chdir(sockaddr.sun_path) == -1)
		die("list-session");
	struct dirent **namelist;
	int n = scandir(sockaddr.sun_path, &namelist, session_filter, session_comparator);
	if (n < 0)
		return 1;
	printf("Active sessions (on host %s)\n", server.host+1);
	while (n--) {
		struct stat sb; char buf[255];
		if (stat(namelist[n]->d_name, &sb) == 0 && S_ISSOCK(sb.st_mode)) {
			pid_t pid = 0;
			strftime(buf, sizeof(buf), "%a%t %F %T", localtime(&sb.st_mtime));
			char status = ' ';
			char *local = strstr(namelist[n]->d_name, server.host);
			if (local) {
				*local = '\0'; /* truncate hostname if we are local */
				if (!(pid = session_exists(namelist[n]->d_name)))
					continue;
			}
			if (sb.st_mode & S_IXUSR)
				status = '*';
			else if (sb.st_mode & S_IXGRP)
				status = '+';
			printf("%c %s\t%jd\t%s\n", status, buf, (intmax_t)pid, namelist[n]->d_name);
		}
		free(namelist[n]);
	}
	free(namelist);
	return 0;
}

int main(int argc, char *argv[]) {
	int opt;
	bool force = false;
	char **cmd = NULL, action = '\0';

	char *default_cmd[4] = { "/bin/sh", "-c", getenv("ABDUCO_CMD"), NULL };
	if (!default_cmd[2]) {
		default_cmd[0] = ABDUCO_CMD;
		default_cmd[1] = NULL;
	}

	server.name = basename(argv[0]);
	gethostname(server.host+1, sizeof(server.host) - 1);

	while ((opt = getopt(argc, argv, "aAclne:fpqrv")) != -1) {
		switch (opt) {
		case 'a':
		case 'A':
		case 'c':
		case 'n':
			action = opt;
			break;
		case 'e':
			if (!optarg)
				usage();
			if (optarg[0] == '^' && optarg[1])
				optarg[0] = CTRL(optarg[1]);
			KEY_DETACH = optarg[0];
			break;
		case 'f':
			force = true;
			break;
		case 'p':
			passthrough = true;
			break;
		case 'q':
			quiet = true;
			break;
		case 'r':
			client.flags |= CLIENT_READONLY;
			break;
		case 'l':
			client.flags |= CLIENT_LOWPRIORITY;
			break;
		case 'v':
			puts("abduco-"VERSION" © 2013-2018 Marc André Tanner");
			exit(EXIT_SUCCESS);
		default:
			usage();
		}
	}

	/* collect the session name if trailing args */
	if (optind < argc)
		server.session_name = argv[optind];

	/* if yet more trailing arguments, they must be the command */
	if (optind + 1 < argc)
		cmd = &argv[optind + 1];
	else
		cmd = default_cmd;

	if (server.session_name && !isatty(STDIN_FILENO))
		passthrough = true;

	if (passthrough) {
		if (!action)
			action = 'a';
		quiet = true;
		client.flags |= CLIENT_LOWPRIORITY;
	}

	if (!action && !server.session_name)
		exit(list_session());
	if (!action || !server.session_name)
		usage();

	if (!passthrough && tcgetattr(STDIN_FILENO, &orig_term) != -1) {
		server.term = orig_term;
		has_term = true;
	}

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &server.winsize) == -1) {
		server.winsize.ws_col = 80;
		server.winsize.ws_row = 25;
	}

	server.read_pty = (action == 'n');

	redo:
	switch (action) {
	case 'n':
	case 'c':
		if (force) {
			if (session_alive(server.session_name)) {
				info("session exists and has not yet terminated");
				return 1;
			}
			if (session_exists(server.session_name))
				attach_session(server.session_name, false);
		}
		if (!create_session(server.session_name, cmd))
			die("create-session");
		if (action == 'n')
			break;
	case 'a':
		if (!attach_session(server.session_name, true))
			die("attach-session");
		break;
	case 'A':
		if (session_alive(server.session_name)) {
			if (!attach_session(server.session_name, true))
				die("attach-session");
		} else if (!attach_session(server.session_name, !force)) {
			force = false;
			action = 'c';
			goto redo;
		}
		break;
	}

	return 0;
}
