#define FD_SET_MAX(fd, set, maxfd) do { \
		FD_SET(fd, set);        \
		if (fd > maxfd)         \
			maxfd = fd;     \
	} while (0)

static Client *client_malloc(int socket) {
	Client *c = calloc(1, sizeof(Client));
	if (!c)
		return NULL;
	c->socket = socket;
	return c;
}

static void client_free(Client *c) {
	if (c && c->socket > 0)
		close(c->socket);
	free(c);
}

static void server_sink_client() {
	if (!server.clients || !server.clients->next)
		return;
	Client *target = server.clients;
	server.clients = target->next;
	Client *dst = server.clients;
	while (dst->next)
		dst = dst->next;
	target->next = NULL;
	dst->next = target;
}

static void server_mark_socket_exec(bool exec, bool usr) {
	struct stat sb;
	if (stat(sockaddr.sun_path, &sb) == -1)
		return;
	mode_t mode = sb.st_mode;
	mode_t flag = usr ? S_IXUSR : S_IXGRP;
	if (exec)
		mode |= flag;
	else
		mode &= ~flag;
	chmod(sockaddr.sun_path, mode);
}

static int server_create_socket(const char *name) {
	if (!set_socket_name(&sockaddr, name))
		return -1;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
		return -1;
	socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
	mode_t mask = umask(S_IXUSR|S_IRWXG|S_IRWXO);
	int r = bind(fd, (struct sockaddr*)&sockaddr, socklen);
	umask(mask);

	if (r == -1) {
		close(fd);
		return -1;
	}

	if (listen(fd, 5) == -1) {
		unlink(sockaddr.sun_path);
		close(fd);
		return -1;
	}

	return fd;
}

static int server_set_socket_non_blocking(int sock) {
	int flags;
	if ((flags = fcntl(sock, F_GETFL, 0)) == -1)
		flags = 0;
    	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

static bool server_read_pty(Packet *pkt) {
	pkt->type = MSG_CONTENT;
	ssize_t len = read(server.pty, pkt->u.msg, sizeof(pkt->u.msg));
	if (len > 0)
		pkt->len = len;
	else if (len == 0)
		server.running = false;
	else if (len == -1 && errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)
		server.running = false;
	print_packet("server-read-pty:", pkt);
	return len > 0;
}

static bool server_write_pty(Packet *pkt) {
	print_packet("server-write-pty:", pkt);
	size_t size = pkt->len;
	if (write_all(server.pty, pkt->u.msg, size) == size)
		return true;
	debug("FAILED\n");
	server.running = false;
	return false;
}

static bool server_recv_packet(Client *c, Packet *pkt) {
	if (recv_packet(c->socket, pkt)) {
		print_packet("server-recv:", pkt);
		return true;
	}
	debug("server-recv: FAILED\n");
	c->state = STATE_DISCONNECTED;
	return false;
}

static bool server_send_packet(Client *c, Packet *pkt) {
	print_packet("server-send:", pkt);
	if (send_packet(c->socket, pkt))
		return true;
	debug("FAILED\n");
	c->state = STATE_DISCONNECTED;
	return false;
}

static void server_pty_died_handler(int sig) {
	int errsv = errno;
	pid_t pid;

	while ((pid = waitpid(-1, &server.exit_status, WNOHANG)) != 0) {
		if (pid == -1)
			break;
		server.exit_status = WEXITSTATUS(server.exit_status);
		server_mark_socket_exec(true, false);
	}

	debug("server pty died: %d\n", server.exit_status);
	errno = errsv;
}

static void server_sigterm_handler(int sig) {
	exit(EXIT_FAILURE); /* invoke atexit handler */
}

static Client *server_accept_client(void) {
	int newfd = accept(server.socket, NULL, NULL);
	if (newfd == -1 || server_set_socket_non_blocking(newfd) == -1)
		goto error;
	Client *c = client_malloc(newfd);
	if (!c)
		goto error;
	if (!server.clients)
		server_mark_socket_exec(true, true);
	c->socket = newfd;
	c->state = STATE_CONNECTED;
	c->next = server.clients;
	server.clients = c;
	server.read_pty = true;

	Packet pkt = {
		.type = MSG_PID,
		.len = sizeof pkt.u.l,
		.u.l = getpid(),
	};
	server_send_packet(c, &pkt);

	return c;
error:
	if (newfd != -1)
		close(newfd);
	return NULL;
}

static void server_sigusr1_handler(int sig) {
	int socket = server_create_socket(server.session_name);
	if (socket != -1) {
		if (server.socket)
			close(server.socket);
		server.socket = socket;
	}
}

static void server_atexit_handler(void) {
	unlink(sockaddr.sun_path);
}

static void server_send_screen_buffer(Client *c) {
	struct entry *np;

	TAILQ_FOREACH_REVERSE(np, &server.screen, screenhead, entries) {
		Packet pkt = {
			.type = MSG_CONTENT,
			.len = np->len,
		};
		strncpy(pkt.u.msg, np->data, np->len);
		server_send_packet(c, &pkt);
	}
}

static void server_preserve_screen_data(Packet *pkt) {
	char *str, *end;
	uint32_t len;
	struct entry *scrline = NULL;

	if (screen_max_rows == 0 || pkt->len <= 0 || pkt->type != MSG_CONTENT)
		return;

	str = pkt->u.msg;
	len = pkt->len;
	end = str + len;

	while (str != end) {
		char *data;
		uint32_t i, dlen;

		bool newline = false;
		char *token = end;

		for (i = 0; i < len; i++) {
			if (str[i] == '\n') {
				token = str + i + 1;
				newline = true;
				break;
			}
		}

		if ((dlen = token - str) <= 0)
			break;

		scrline = TAILQ_FIRST(&server.screen);

		if (scrline && !scrline->complete) {
			data = realloc(scrline->data, scrline->len + dlen);
			if (!data)
				die("unable to extend string in the screen buffer");

			memcpy(data + scrline->len, str, dlen);

			scrline->complete = newline;
			scrline->data = data;
			scrline->len += dlen;
		} else {
			data = malloc(dlen);
			if (!data)
				die("unable to allocate memory for new line in the screen buffer");

			memcpy(data, str, dlen);

			scrline = malloc(sizeof(*scrline));
			if (!scrline)
				die("unable to allocate memory for screen buffer element");

			scrline->complete = newline;
			scrline->data = data;
			scrline->len = dlen;

			TAILQ_INSERT_HEAD(&server.screen, scrline, entries);
			server.screen_rows++;

			if (server.screen_rows > screen_max_rows) {
				scrline = TAILQ_LAST(&server.screen, screenhead);
				TAILQ_REMOVE(&server.screen, scrline, entries);
				free(scrline->data);
				free(scrline);
				server.screen_rows--;
			}
		}

		str = token;
		len -= dlen;
	}
}

static void server_mainloop(void) {
	atexit(server_atexit_handler);
	fd_set new_readfds, new_writefds;
	FD_ZERO(&new_readfds);
	FD_ZERO(&new_writefds);
	FD_SET(server.socket, &new_readfds);
	int new_fdmax = server.socket;
	bool exit_packet_delivered = false;

	TAILQ_INIT(&server.screen);

	if (server.read_pty)
		FD_SET_MAX(server.pty, &new_readfds, new_fdmax);

	while (server.clients || !exit_packet_delivered) {
		int fdmax = new_fdmax;
		fd_set readfds = new_readfds;
		fd_set writefds = new_writefds;
		FD_SET_MAX(server.socket, &readfds, fdmax);

		if (select(fdmax+1, &readfds, &writefds, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			die("server-mainloop");
		}

		FD_ZERO(&new_readfds);
		FD_ZERO(&new_writefds);
		new_fdmax = server.socket;

		bool pty_data = false;

		Packet server_packet, client_packet;

		if (FD_ISSET(server.socket, &readfds))
			server_accept_client();

		if (FD_ISSET(server.pty, &readfds)) {
			pty_data = server_read_pty(&server_packet);
			if (pty_data)
				server_preserve_screen_data(&server_packet);
		}

		for (Client **prev_next = &server.clients, *c = server.clients; c;) {
			if (FD_ISSET(c->socket, &readfds) && server_recv_packet(c, &client_packet)) {
				switch (client_packet.type) {
				case MSG_CONTENT:
					server_write_pty(&client_packet);
					break;
				case MSG_ATTACH:
					c->flags = client_packet.u.i;
					if (c->flags & CLIENT_LOWPRIORITY)
						server_sink_client();
					server_send_screen_buffer(c);
					break;
				case MSG_RESIZE:
					c->state = STATE_ATTACHED;
					if (!(c->flags & CLIENT_READONLY) && c == server.clients) {
						debug("server-ioct: TIOCSWINSZ\n");
						struct winsize ws = { 0 };
						ws.ws_row = client_packet.u.ws.rows;
						ws.ws_col = client_packet.u.ws.cols;
						ioctl(server.pty, TIOCSWINSZ, &ws);
					}
					kill(-server.pid, SIGWINCH);
					break;
				case MSG_EXIT:
					exit_packet_delivered = true;
					/* fall through */
				case MSG_DETACH:
					c->state = STATE_DISCONNECTED;
					break;
				default: /* ignore package */
					break;
				}
			}

			if (c->state == STATE_DISCONNECTED) {
				bool first = (c == server.clients);
				Client *t = c->next;
				client_free(c);
				*prev_next = c = t;
				if (first && server.clients) {
					Packet pkt = {
						.type = MSG_RESIZE,
						.len = 0,
					};
					server_send_packet(server.clients, &pkt);
				} else if (!server.clients) {
					server_mark_socket_exec(false, true);
				}
				continue;
			}

			FD_SET_MAX(c->socket, &new_readfds, new_fdmax);

			if (pty_data)
				server_send_packet(c, &server_packet);
			if (!server.running) {
				if (server.exit_status != -1) {
					Packet pkt = {
						.type = MSG_EXIT,
						.u.i = server.exit_status,
						.len = sizeof(pkt.u.i),
					};
					if (!server_send_packet(c, &pkt))
						FD_SET_MAX(c->socket, &new_writefds, new_fdmax);
				} else {
					FD_SET_MAX(c->socket, &new_writefds, new_fdmax);
				}
			}
			prev_next = &c->next;
			c = c->next;
		}

		if (server.running && server.read_pty)
			FD_SET_MAX(server.pty, &new_readfds, new_fdmax);
	}

	struct entry *n1, *n2;

	n1 = TAILQ_FIRST(&server.screen);
	while (n1 != NULL) {
		n2 = TAILQ_NEXT(n1, entries);
		free(n1->data);
		free(n1);
		n1 = n2;
	}

	exit(EXIT_SUCCESS);
}
