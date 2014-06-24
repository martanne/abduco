static void client_sigwinch_handler(int sig) {
	client.need_resize = true;
}

static bool client_send_packet(Packet *pkt) {
	print_packet("client-send:", pkt);
	if (send_packet(server.socket, pkt))
		return true;
	debug("FAILED\n");
	server.running = false;
	return false;
}

static bool client_recv_packet(Packet *pkt) {
	if (recv_packet(server.socket, pkt)) {
		print_packet("client-recv:", pkt);
		return true;
	}
	debug("client-recv: FAILED\n");
	server.running = false;
	return false;
}

static void client_show_cursor() {
	printf("\e[?25h");
	fflush(stdout);
}

static void client_restore_terminal() {
	if (has_term)
		tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_term);
	client_show_cursor();
}

static int client_mainloop() {
	client.need_resize = true;
	Packet pkt = {
		.type = MSG_ATTACH,
		.u = { .b = client.readonly },
		.len = sizeof(pkt.u.b),
	};
	client_send_packet(&pkt);

	while (server.running) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(server.socket, &fds);

		if (client.need_resize) {
			struct winsize ws;
			if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != -1) {
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
				case MSG_RESIZE:
					client.need_resize = true;
					break;
				case MSG_EXIT:
					client_send_packet(&pkt);
					close(server.socket);
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
				debug("client-stdin: %c\n", pkt.u.msg[0]);
				pkt.len = len;
				if (pkt.u.msg[0] == KEY_REDRAW) {
					client.need_resize = true;
				} else if (pkt.u.msg[0] == KEY_DETACH) {
					pkt.type = MSG_DETACH;
					pkt.len = 0;
					client_send_packet(&pkt);
					close(server.socket);
					return -1;
				} else if (!client.readonly) {
					client_send_packet(&pkt);
				}
			}
		}
	}

	return -EIO;
}
