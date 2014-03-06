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
#define _GNU_SOURCE
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

#ifdef _AIX
# include "forkpty-aix.c"
#endif
#define CLIENT_TIMEOUT 100
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
	} u;
} Packet;

typedef struct {
	Packet pkt;
	size_t off;
} ClientPacketState;

typedef struct {
	Packet *pkt;
	size_t off;
} ServerPacketState;

typedef struct Client Client;
struct Client {
	ServerPacketState output; /* display output as received from server */
	ClientPacketState input; /* input as sent to the server */
	int socket;
	enum {
		STATE_CONNECTED,
		STATE_ATTACHED,
		STATE_DETACHED,
		STATE_DISCONNECTED,
	} state;
	time_t last_activity;
	bool need_resize;
	Client *next;
};

typedef struct { 
	Client *clients;
	int client_count;
	int socket;
	Packet pty_output;
	ClientPacketState pty_input;
	Packet queue[10];
	unsigned int queue_count;
	unsigned int queue_insert;
	unsigned int queue_remove;
	int pty;
	int exit_status;
	struct termios term;
	pid_t pid;
	volatile sig_atomic_t running;
	const char *name;
	const char *session_name;
} Server;

static Server server = { .running = true, .exit_status = -1 };
static struct termios orig_term, cur_term;
bool has_term;

static struct sockaddr_un sockaddr = {
	.sun_family = AF_UNIX,
};

static int create_socket(const char *name);
static void die(const char *s);
static void info(const char *str, ...);

static inline size_t packet_header_size() {
	return offsetof(Packet, u);
}

static size_t packet_size(Packet *pkt) {
	return packet_header_size() + pkt->len;
}

static bool is_client_packet_complete(ClientPacketState *pkt) {
	return pkt->off >= packet_header_size() && pkt->off == packet_size(&pkt->pkt);
}

static bool is_server_packet_complete(ServerPacketState *pkt) {
	return pkt->pkt && pkt->off == packet_size(pkt->pkt);
}

static bool is_server_packet_nonempty(ServerPacketState *pkt) {
	return pkt->pkt && pkt->pkt->len > 0;
}

#include "debug.c"
#include "client.c"
#include "server.c"

static void info(const char *str, ...) {
	va_list ap;
	va_start(ap, str);
	fprintf(stdout, "\e[999H\r\n");
	if (str) {
		fprintf(stdout, "%s: %s: ", server.name, server.session_name);
		vfprintf(stdout, str, ap);
		fprintf(stdout, "\r\n");
	}
	fflush(stdout);
	va_end(ap);
}

static void die(const char *s) {
	perror(s);
	exit(EXIT_FAILURE);
}

static void usage() {
	fprintf(stderr, "usage: abduco [-a|-A|-c|-n] [-e detachkey] name command\n");
	exit(EXIT_FAILURE);
}

static int create_socket_dir() {
	size_t maxlen = sizeof(sockaddr.sun_path);
	char *dir = getenv("HOME");
	if (!dir)
		dir = "/tmp";
	int len = snprintf(sockaddr.sun_path, maxlen, "%s/.%s/", dir, server.name);
	if (len < 0 || (size_t)len >= maxlen)
		return -1;
	if (mkdir(sockaddr.sun_path, 0750) == -1 && errno != EEXIST)
		return -1;
	return len;
}

static int create_socket(const char *name) {
	size_t maxlen = sizeof(sockaddr.sun_path);
	if (name[0] == '.' || name[0] == '/') {
		strncpy(sockaddr.sun_path, name, maxlen);
		if (sockaddr.sun_path[maxlen-1])
			return -1;
	} else {
		int len = create_socket_dir(), rem = strlen(name);
		if (len == -1 || maxlen - len - rem <= 0)
			return -1;
		strncat(sockaddr.sun_path, name, maxlen - len - 1);
	}
	return socket(AF_LOCAL, SOCK_STREAM, 0);
}

static bool create_session(const char *name, char * const argv[]) {
	int pipefds[2];
	pid_t pid;
	char errormsg[255];
	struct sigaction sa;

	if (pipe(pipefds) == -1)
		return false;
	if ((server.socket = server_create_socket(name)) == -1)
		return false;

	switch ((pid = fork())) {
	case 0: /* child process */
		setsid();
		close(pipefds[0]);
		switch ((pid = fork())) {
		case 0: /* child process */
			sa.sa_flags = 0;
			sigemptyset(&sa.sa_mask);
			sa.sa_handler = server_pty_died_handler;
			sigaction(SIGCHLD, &sa, NULL);
			switch (server.pid = forkpty(&server.pty, NULL, has_term ? &server.term : NULL, NULL)) {
			case 0: /* child process */
				fcntl(pipefds[1], F_SETFD, FD_CLOEXEC);
				close(server.socket);
				execvp(argv[0], argv);
				snprintf(errormsg, sizeof(errormsg), "server-execvp: %s\n", strerror(errno));
				write_all(pipefds[1], errormsg, strlen(errormsg));
				close(pipefds[1]);
				exit(EXIT_FAILURE);
				break;
			case -1:
				die("server-forkpty");
				break;
			default:
				/* SIGTTIN, SIGTTU */
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
				close(pipefds[1]);
				server_mainloop();
				break;
			}
			break;
		default:
			close(pipefds[1]);
			exit(EXIT_SUCCESS);
			break;
		}
		break;
	case -1: /* fork failed */
		return false;
	default: /* parent */
		close(pipefds[1]);
		int status;
		wait(&status); /* wait for first fork */
		if ((status = read_all(pipefds[0], errormsg, sizeof(errormsg))) > 0) {
			write_all(STDERR_FILENO, errormsg, status);
			unlink(sockaddr.sun_path);
			exit(EXIT_FAILURE);
		}
		close(pipefds[0]);
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
	tcsetattr(0, TCSADRAIN, &cur_term);

	client_clear_screen();
	int status = client_mainloop();
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
	return d->d_name[0] != '.';
}

static int session_comparator(const struct dirent **a, const struct dirent **b) {
	struct stat sa, sb;
	if (stat((*a)->d_name, &sa) != 0)
		return -1;
	if (stat((*b)->d_name, &sb) != 0)
		return 1;
	return sa.st_atime < sb.st_atime ? -1 : 1;
}

static int list_session() {
	if (create_socket_dir() == -1)
		return 1;
	chdir(sockaddr.sun_path);
	struct dirent **namelist;
	int n = scandir(sockaddr.sun_path, &namelist, session_filter, session_comparator);
	if (n < 0)
		return 1;
	puts("Active sessions");
	while (n--) {
		struct stat sb; char buf[255];
		if (stat(namelist[n]->d_name, &sb) == 0) {
			strftime(buf, sizeof(buf), "%a%t %d.%m.%Y %T", localtime(&sb.st_atime));
			printf(" %s\t%s\n", buf, namelist[n]->d_name);
		}
		free(namelist[n]);
	}
	free(namelist);
	return 0;
}

int main(int argc, char *argv[]) {
	char **cmd = NULL, action = '\0';
	server.name = basename(argv[0]);
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
		case 'v':
			puts("abduco-"VERSION" © 2013-2014 Marc André Tanner");
			exit(EXIT_SUCCESS);
		default:
			usage();
		}
	}

	if (!action || !server.session_name || (action != 'a' && !cmd))
		usage();

	if (tcgetattr(STDIN_FILENO, &orig_term) != -1) {
		server.term = orig_term;
		has_term = true;
	}

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
