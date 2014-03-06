static Client client;

static void client_sigwinch_handler(int sig) {
	client.need_resize = true;
}

static ssize_t write_all(int fd, const char *buf, size_t len) {
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

static bool client_send_packet(Packet *pkt) {
	print_packet("client-send:", pkt);
	size_t size = packet_size(pkt);
	if (write_all(server.socket, (char *)pkt, size) != size) {
		debug("FAILED\n");
		server.running = false;
		return false;
	}
	return true;
}

static bool client_recv_packet(Packet *pkt) {
	ssize_t len = read_all(server.socket, (char*)pkt, packet_header_size());
	if (len <= 0 || len != packet_header_size() || pkt->len == 0)
		goto error;
	len = read_all(server.socket, pkt->u.msg, pkt->len);
	print_packet("client-recv:", pkt);
	if (len <= 0 || len != pkt->len)
		goto error;
	return true;
error:
	debug("FAILED here\n");
	server.running = false;
	return false;
}

static void client_show_cursor() {
	printf("\e[?25h");
	fflush(stdout);
}

static void client_restore_terminal() {
	if (has_term)
		tcsetattr(0, TCSADRAIN, &orig_term);
	client_show_cursor();
}

static int client_mainloop() {
	client.need_resize = true;
	while (server.running) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(server.socket, &fds);

		if (client.need_resize) {
			struct winsize ws;
			if (ioctl(0, TIOCGWINSZ, &ws) != -1) {
				Packet pkt = {
					.type = MSG_RESIZE,
					.u = { .ws = ws },
					.len = sizeof(ws),
				};
				if (client_send_packet(&pkt))
					client.need_resize = false;
			}
		}

		if (select(server.socket + 1, &fds, NULL, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			die("client-mainloop");
		}

		if (FD_ISSET(server.socket, &fds)) {
			Packet pkt;
			if (client_recv_packet(&pkt)) {
				switch (pkt.type) {
				case MSG_CONTENT:
					write_all(STDOUT_FILENO, pkt.u.msg, pkt.len);
					break;
				case MSG_EXIT:
					return pkt.u.i;
				}
			}
		}

		if (FD_ISSET(STDIN_FILENO, &fds)) {
			Packet pkt = { .type = MSG_CONTENT };
			ssize_t len = read(STDIN_FILENO, pkt.u.msg, sizeof(pkt.u.msg));
			if (len == -1 && errno != EAGAIN && errno != EINTR)
				die("client-stdin");
			if (len > 0) {
				debug("client-stdin: %c", pkt.u.msg[0]);
				pkt.len = len;
				if (pkt.u.msg[0] == KEY_REDRAW) {
					client.need_resize = true;
				} else if (pkt.u.msg[0] == KEY_DETACH) {
					pkt.type = MSG_DETACH;
					pkt.len = 0;
					client_send_packet(&pkt);
					return -1;
				} else {
					client_send_packet(&pkt);
				}
			}
		}
	}

	return -EIO;
}
