#ifdef NDEBUG
static void debug(const char *errstr, ...) { }
static void print_packet(const char *prefix, Packet *pkt) { }
#else

static void debug(const char *errstr, ...) {
	va_list ap;
	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
}

static void print_packet(const char *prefix, Packet *pkt) {
	static const char *msgtype[] = {
		[MSG_CONTENT] = "CONTENT",
		[MSG_ATTACH]  = "ATTACH",
		[MSG_DETACH]  = "DETACH",
		[MSG_RESIZE]  = "RESIZE",
		[MSG_REDRAW]  = "REDRAW",
		[MSG_EXIT]    = "EXIT",
	};
	const char *type = "UNKNOWN";
	if (pkt->type < countof(msgtype) && msgtype[pkt->type])
		type = msgtype[pkt->type];

	fprintf(stderr, "%s: %s ", prefix, type);
	switch (pkt->type) {
	case MSG_CONTENT:
		for (size_t i = 0; i < pkt->len && i < sizeof(pkt->u.msg); i++)
			fprintf(stderr, "%c", pkt->u.msg[i]);
		break;
	case MSG_RESIZE:
		fprintf(stderr, "%dx%d", pkt->u.ws.ws_col, pkt->u.ws.ws_row);
		break;
	default:
		fprintf(stderr, "len: %d", pkt->len);
		break;
	}
	fprintf(stderr, "\n");
}

#endif /* NDEBUG */
