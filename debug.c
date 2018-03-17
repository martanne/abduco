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
		[MSG_EXIT]    = "EXIT",
		[MSG_PID]     = "PID",
	};
	const char *type = "UNKNOWN";
	if (pkt->type < countof(msgtype) && msgtype[pkt->type])
		type = msgtype[pkt->type];

	fprintf(stderr, "%s: %s ", prefix, type);
	switch (pkt->type) {
	case MSG_CONTENT:
		fwrite(pkt->u.msg, pkt->len, 1, stderr);
		break;
	case MSG_RESIZE:
		fprintf(stderr, "%"PRIu16"x%"PRIu16, pkt->u.ws.cols, pkt->u.ws.rows);
		break;
	case MSG_ATTACH:
		fprintf(stderr, "readonly: %d low-priority: %d",
			pkt->u.i & CLIENT_READONLY,
			pkt->u.i & CLIENT_LOWPRIORITY);
		break;
	case MSG_EXIT:
		fprintf(stderr, "status: %"PRIu32, pkt->u.i);
		break;
	case MSG_PID:
		fprintf(stderr, "pid: %"PRIu32, pkt->u.i);
		break;
	default:
		fprintf(stderr, "len: %"PRIu32, pkt->len);
		break;
	}
	fprintf(stderr, "\n");
}

#endif /* NDEBUG */
