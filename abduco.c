/*
 * Copyright (c) 2013-2014 Marc André Tanner <mat at brain-dump.org>
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
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#if defined(__linux__) || defined(__CYGWIN__)
# include <pty.h>
#elif defined(__FreeBSD__)
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
	MSG_REDRAW  = 4,
	MSG_EXIT    = 5,
};

typedef struct {
	unsigned int type;
	size_t len;
	union {
		char msg[BUFSIZ];
		struct winsize ws;
		int i;
		bool b;
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
	bool readonly;
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
bool has_term;

static struct sockaddr_un sockaddr = {
	.sun_family = AF_UNIX,
};

static int create_socket(const char *name);
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
	return write_all(socket, (char *)pkt, size) == size;
}

static bool recv_packet(int socket, Packet *pkt) {
	ssize_t len = read_all(socket, (char*)pkt, packet_header_size());
	if (len <= 0 || len != packet_header_size())
		return false;
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
	fprintf(stderr, "\033[999H");
	if (str) {
		fprintf(stderr, "%s: %s: ", server.name, server.session_name);
		vfprintf(stderr, str, ap);
		fprintf(stderr, "\r\n");
	}
	fflush(stderr);
	va_end(ap);
}

static void die(const char *s) {
	perror(s);
	exit(EXIT_FAILURE);
}

static void usage(void) {
	fprintf(stderr, "usage: abduco [-a|-A|-c|-n] [-r] [-e detachkey] name command\n");
	exit(EXIT_FAILURE);
}

static int create_socket_dir(void) {
	size_t maxlen = sizeof(sockaddr.sun_path);
	char *dirs[] = { getenv("HOME"), getenv("TMPDIR"), "/tmp" };
	int socketfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socketfd == -1)
		return -1;
	for (unsigned int i = 0; i < countof(dirs); i++) {
		char *dir = dirs[i];
		if (!dir)
			continue;
		int len = snprintf(sockaddr.sun_path, maxlen, "%s/.%s/", dir, server.name);
		if (len < 0 || (size_t)len >= maxlen)
			continue;
		if (mkdir(sockaddr.sun_path, 0750) == -1 && errno != EEXIST)
			continue;
		int len2 = snprintf(sockaddr.sun_path, maxlen, "%s/.%s/.abduco-%d", dir, server.name, getpid());
		if (len2 < 0 || (size_t)len2 >= maxlen)
			continue;
		socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
		if (bind(socketfd, (struct sockaddr*)&sockaddr, socklen) == -1)
			continue;
		unlink(sockaddr.sun_path);
		close(socketfd);
		sockaddr.sun_path[len] = '\0';
		return len;
	}

	close(socketfd);
	return -1;
}

static int create_socket(const char *name) {
	size_t maxlen = sizeof(sockaddr.sun_path);
	if (name[0] == '/') {
		strncpy(sockaddr.sun_path, name, maxlen);
		if (sockaddr.sun_path[maxlen-1])
			return -1;
	} else if (name[0] == '.' && (name[1] == '.' || name[1] == '/')) {
		char buf[maxlen], *cwd = getcwd(buf, sizeof buf);
		if (!cwd)
			return -1;
		int len = snprintf(sockaddr.sun_path, maxlen, "%s/%s", cwd, name);
		if (len < 0 || (size_t)len >= maxlen)
			return -1;
	} else {
		int dir_len = create_socket_dir();
		if (dir_len == -1 || dir_len + strlen(name) + strlen(server.host) >= maxlen)
			return -1;
		strncat(sockaddr.sun_path, name, maxlen - strlen(sockaddr.sun_path) - 1);
		strncat(sockaddr.sun_path, server.host, maxlen - strlen(sockaddr.sun_path) - 1);
	}
	return socket(AF_UNIX, SOCK_STREAM, 0);
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
				fcntl(client_pipe[1], F_SETFD, FD_CLOEXEC);
				fcntl(server_pipe[1], F_SETFD, FD_CLOEXEC);
				execvp(argv[0], argv);
				snprintf(errormsg, sizeof(errormsg), "server-execvp: %s\n", strerror(errno));
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
				dup2(fd, 0);
				dup2(fd, 1);
				dup2(fd, 2);
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

static bool attach_session(const char *name) {
	if (server.socket > 0)
		close(server.socket);
	if ((server.socket = create_socket(name)) == -1)
		return false;
	socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
	if (connect(server.socket, (struct sockaddr*)&sockaddr, socklen) == -1)
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
	atexit(client_restore_terminal);

	cur_term = orig_term;
	cur_term.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON|IXOFF);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	cur_term.c_cflag &= ~(CSIZE|PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = _POSIX_VDISABLE;
	cur_term.c_cc[VMIN] = 1;
	cur_term.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSADRAIN, &cur_term);

	int status = client_mainloop();
	client_restore_terminal();
	if (status == -1) {
		info("detached");
	} else if (status == -EIO) {
		info("exited due to I/O errors");
	} else {
		info("session terminated with exit status %d", status);
		exit(status);
	}

	return true;
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
	if (create_socket_dir() == -1)
		return 1;
	chdir(sockaddr.sun_path);
	struct dirent **namelist;
	int n = scandir(sockaddr.sun_path, &namelist, session_filter, session_comparator);
	if (n < 0)
		return 1;
	printf("Active sessions (on host %s)\n", server.host+1);
	while (n--) {
		struct stat sb; char buf[255];
		if (stat(namelist[n]->d_name, &sb) == 0 && S_ISSOCK(sb.st_mode)) {
			strftime(buf, sizeof(buf), "%a%t %F %T", localtime(&sb.st_atime));
			char status = ' ';
			if (sb.st_mode & S_IXUSR)
				status = '*';
			else if (sb.st_mode & S_IXGRP)
				status = '+';
			char *name = strstr(namelist[n]->d_name, server.host);
			if (name)
				*name = '\0';
			printf("%c %s\t%s\n", status, buf, namelist[n]->d_name);
		}
		free(namelist[n]);
	}
	free(namelist);
	return 0;
}

int main(int argc, char *argv[]) {
	char **cmd = NULL, action = '\0';
	server.name = basename(argv[0]);
	gethostname(server.host+1, sizeof(server.host) - 1);
	if (argc == 1)
		exit(list_session());
	for (int arg = 1; arg < argc; arg++) {
		if (argv[arg][0] != '-') {
			if (!server.session_name) {
				server.session_name = argv[arg];
				continue;
			} else if (!cmd) {
				cmd = &argv[arg];
				break;
			}
		}
		if (server.session_name)
			usage();
		switch (argv[arg][1]) {
		case 'a':
		case 'A':
		case 'c':
		case 'n':
			action = argv[arg][1];
			break;
		case 'e':
			if (arg + 1 >= argc)
				usage();
			char *esc = argv[++arg];
			if (esc[0] == '^' && esc[1])
				*esc = CTRL(esc[1]);
			KEY_DETACH = *esc;
			break;
		case 'r':
			client.readonly = true;
			break;
		case 'v':
			puts("abduco-"VERSION" © 2013-2014 Marc André Tanner");
			exit(EXIT_SUCCESS);
		default:
			usage();
		}
	}

	if (!cmd) {
		cmd = (char*[]){ getenv("ABDUCO_CMD"), NULL };
		if (!cmd[0])
			cmd[0] = "dvtm";
	}

	if (!action || !server.session_name || ((action == 'c' || action == 'A') && client.readonly))
		usage();

	if (tcgetattr(STDIN_FILENO, &orig_term) != -1) {
		server.term = orig_term;
		has_term = true;
	}

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &server.winsize) == -1) {
		server.winsize.ws_col = 80;
		server.winsize.ws_row = 25;
	}

	server.read_pty = (action == 'n');

	switch (action) {
	redo:
	case 'n':
	case 'c':
		if (!create_session(server.session_name, cmd))
			die("create-session");
		if (action == 'n')
			break;
	case 'a':
	case 'A':
		if (!attach_session(server.session_name)) {
			if (action == 'A') {
				action = 'c';
				goto redo;
			}
			die("attach-session");
		}
	}

	return 0;
}
