#ifdef NDEBUG
static void debug(const char *errstr, ...) { }
static void print_client_packet(const char *prefix, ClientPacket *pkt) { }
static void print_client_packet_state(const char *prefix, ClientPacketState *pkt) { }
static void print_server_packet(const char *prefix, ServerPacket *pkt) { }
static void print_server_packet_state(const char *prefix, ServerPacketState *pkt) { }
#else

static void debug(const char *errstr, ...) {
	va_list ap;
	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
}

static void print_client_packet(const char *prefix, ClientPacket *pkt) {
	char *s = "UNKNOWN";
	switch (pkt->type) {
	case MSG_CONTENT:
		s = "CONTENT";
		break;
	case MSG_ATTACH:
		s = "ATTACH";
		break;
	case MSG_DETACH:
		s = "DETACH";
		break;
	case MSG_RESIZE:
		s = "RESIZE";
		break;
	case MSG_REDRAW:
		s = "REDRAW";
		break;
	}

	if (pkt->type == MSG_CONTENT) {
		fprintf(stderr, "%s %s len: %d content: ", prefix, s, pkt->len);
		for (size_t i = 0; i < pkt->len && i < sizeof(pkt->u.msg); i++)
			fprintf(stderr, "%c", pkt->u.msg[i]);
		fprintf(stderr, "\n");
	} else {
		fprintf(stderr, "%s %s\n", prefix, s);
	}
}

static void print_client_packet_state(const char *prefix, ClientPacketState *pkt) {
	fprintf(stderr, "%s %d/%d\n", prefix, pkt->off, sizeof(ClientPacket));
	if (is_client_packet_complete(pkt))
		print_client_packet(prefix, &pkt->pkt);
}

static void print_server_packet(const char *prefix, ServerPacket *pkt) {
	fprintf(stderr, "%s len: %d buf: \n\t%s\n", prefix, pkt->len, pkt->buf);
}

static void print_server_packet_state(const char *prefix, ServerPacketState *pkt) {
	fprintf(stderr, "%s %d/%d\n", prefix, pkt->off, pkt->pkt->len);
	if (is_server_packet_complete(pkt))
		print_server_packet(prefix, pkt->pkt);
}

#endif /* NDEBUG */
