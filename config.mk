# abduco version
VERSION = 0.1

# Customize below to fit your system

PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

INCS = -I.
LIBS = -lc -lutil

# AIX
#LIBS = -lc

# Solaris
#LIBS = -lc -lsocket

CFLAGS += -std=c99 -Os ${INCS} -DVERSION=\"${VERSION}\" -DNDEBUG -D_GNU_SOURCE
LDFLAGS += ${LIBS}

DEBUG_CFLAGS = ${CFLAGS} -UNDEBUG -O0 -g -ggdb -Wall

CC = cc
STRIP = strip
