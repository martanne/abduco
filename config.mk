# abduco version
VERSION = 0.1

# Customize below to fit your system

PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

INCS = -I. -I/usr/include -I/usr/local/include
LIBS = -lc -lutil

CFLAGS += -std=c99 -Os ${INCS} -DVERSION=\"${VERSION}\" -DNDEBUG
LDFLAGS += -L/usr/lib -L/usr/local/lib ${LIBS}

DEBUG_CFLAGS = ${CFLAGS} -UNDEBUG -O0 -g -ggdb -Wall

CC = cc
