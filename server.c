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

static int server_mark_socket_exec(bool exec, bool usr) {
	struct stat sb;
	if (stat(sockaddr.sun_path, &sb) == -1)
		return -1;
	mode_t mode = sb.st_mode;
	mode_t flag = usr ? S_IXUSR : S_IXGRP;
	if (exec)
		mode |= flag;
	else
		mode &= ~flag;
	return chmod(sockaddr.sun_path, mode);
}

static int server_create_socket(const char *name) {
	int socket = create_socket(name);
	if (socket == -1)
		return -1;
	socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
	mode_t mode = S_IRUSR|S_IWUSR;
	fchmod(socket, mode);
	if (bind(socket, (struct sockaddr*)&sockaddr, socklen) == -1)
		return -1;
	if (fchmod(socket, mode) == -1 && chmod(sockaddr.sun_path, mode) == -1)
		goto error;
	if (listen(socket, 5) == -1)
		goto error;
	return socket;
error:
	unlink(sockaddr.sun_path);
	return -1;
}

static int server_set_socket_non_blocking(int sock) {
	int flags;
	if ((flags = fcntl(sock, F_GETFL, 0)) == -1)
		flags = 0;
    	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

static Client *server_accept_client(void) {
	int newfd = accept(server.socket, NULL, NULL);
	if (newfd == -1)
		return NULL;
	Client *c = client_malloc(newfd);
	if (!c)
		return NULL;
	if (!server.clients)
		server_mark_socket_exec(true, true);
	server_set_socket_non_blocking(newfd);
	c->socket = newfd;
	c->state = STATE_CONNECTED;
	c->next = server.clients;
	server.clients = c;
	server.read_pty = true;
	return c;
}

static bool server_read_pty(Packet *pkt) {
	pkt->type = MSG_CONTENT;
	ssize_t len = read(server.pty, pkt->u.msg, sizeof(pkt->u.msg));
	if (len != -1)
		pkt->len = len;
	else if (errno != EAGAIN && errno != EINTR)
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

static void server_mainloop(void) {
	atexit(server_atexit_handler);
	fd_set new_readfds, new_writefds;
	FD_ZERO(&new_readfds);
	FD_ZERO(&new_writefds);
	FD_SET(server.socket, &new_readfds);
	int new_fdmax = server.socket;
	bool exit_packet_delivered = false;

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

		if (FD_ISSET(server.pty, &readfds))
			pty_data = server_read_pty(&server_packet);

		for (Client **prev_next = &server.clients, *c = server.clients; c;) {
			if (FD_ISSET(c->socket, &readfds) && server_recv_packet(c, &client_packet)) {
				switch (client_packet.type) {
				case MSG_CONTENT:
					server_write_pty(&client_packet);
					break;
				case MSG_ATTACH:
					c->readonly = client_packet.u.b;
					break;
				case MSG_RESIZE:
					c->state = STATE_ATTACHED;
				case MSG_REDRAW:
					if (!c->readonly && (client_packet.type == MSG_REDRAW || c == server.clients)) {
						debug("server-ioct: TIOCSWINSZ\n");
						ioctl(server.pty, TIOCSWINSZ, &client_packet.u.ws);
					}
					kill(-server.pid, SIGWINCH);
					break;
				case MSG_DETACH:
				case MSG_EXIT:
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
					if (server_send_packet(c, &pkt))
						exit_packet_delivered = true;
					else
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

	exit(EXIT_SUCCESS);
}
