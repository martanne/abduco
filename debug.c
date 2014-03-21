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
	// XXX: look up table
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
	case MSG_EXIT:
		s = "EXIT";
		break;
	}

	if (pkt->type == MSG_CONTENT) {
		fprintf(stderr, "%s %s len: %d content: ", prefix, s, pkt->len);
		for (size_t i = 0; i < pkt->len && i < sizeof(pkt->u.msg); i++)
			fprintf(stderr, "%c", pkt->u.msg[i]);
		fprintf(stderr, "\n");
	} else if (pkt->type == MSG_RESIZE) {
		fprintf(stderr, "%s %s %d x %d\n", prefix, s, pkt->u.ws.ws_col, pkt->u.ws.ws_row);
	} else {
		fprintf(stderr, "%s %s len: %d\n", prefix, s, pkt->len);
	}
}

#endif /* NDEBUG */
