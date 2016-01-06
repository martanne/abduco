static char KEY_DETACH = CTRL('\\');
static char KEY_REDRAW = 0;

/* Where to place the "abduco" directory storing all session socket files.
 * The first directory to succeed is used. */
static struct Dir {
	char *path;    /* fixed (absolute) path to a directory */
	char *env;     /* environment variable to use if (set) */
	bool personal; /* if false a user owned sub directory will be created */
} socket_dirs[] = {
	{ .env  = "ABDUCO_SOCKET_DIR", false },
	{ .env  = "HOME",              true  },
	{ .env  = "TMPDIR",            false },
	{ .path = "/tmp",              false },
};
