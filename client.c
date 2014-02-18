static Client client;

static void client_sigwinch_handler(int sig) {
	client.need_resize = true;
}

static ssize_t write_all(int fd, const char *buf, size_t len) {
	ssize_t ret = len;
	while (len > 0) {
		int res = write(fd, buf, len);
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
		int res = read(fd, buf, len);
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

static bool client_send_packet(ClientPacket *pkt) {
	print_client_packet("client-send:", pkt);
	if (write_all(server.socket, (char *)pkt, sizeof(ClientPacket)) != sizeof(ClientPacket)) {
		debug("FAILED\n");
		server.running = false;
		return false;
	}
	return true;
}

static bool client_recv_packet(ServerPacket *pkt) {
	pkt->len = read_all(server.socket, pkt->buf, sizeof(pkt->buf));
	print_server_packet("client-recv:", pkt);
	if (pkt->len <= 0) {
		debug("FAILED\n");
		server.running = false;
		return false;
	}
	return true;
}

static void client_clear_screen() {
	printf("\e[H\e[J");
	fflush(stdout);
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
				ClientPacket pkt = {
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
			ServerPacket pkt;
			if (client_recv_packet(&pkt))
				write_all(STDOUT_FILENO, pkt.buf, pkt.len);
		}

		if (FD_ISSET(STDIN_FILENO, &fds)) {
			ClientPacket pkt = { .type = MSG_CONTENT };
			ssize_t len = read(STDIN_FILENO, pkt.u.msg, sizeof(pkt.u.msg));
			if (len == -1 && errno != EAGAIN && errno != EINTR)
				die("client-stdin");
			if (len > 0) {
				pkt.len = len;
				if (pkt.u.msg[0] == KEY_REDRAW) {
					client.need_resize = true;
				} else if (pkt.u.msg[0] == KEY_DETACH) {
					pkt.type = MSG_DETACH;
					client_send_packet(&pkt);
					return -1;
				} else if (pkt.u.msg[0] == cur_term.c_cc[VSUSP]) {
					pkt.type = MSG_DETACH;
					client_send_packet(&pkt);
					tcsetattr(0, TCSADRAIN, &orig_term);
					client_show_cursor();
					info(NULL);
					kill(getpid(), SIGTSTP); 
					tcsetattr(0, TCSADRAIN, &cur_term);
					client.need_resize = true;
				} else {
					client_send_packet(&pkt);
				}
			}
		}
	}

	return 0;
}
