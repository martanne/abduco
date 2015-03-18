# abduco version
VERSION = 0.4

# Customize below to fit your system

PREFIX ?= /usr/local
MANPREFIX = ${PREFIX}/share/man

INCS = -I.
LIBS = -lc -lutil

CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
CFLAGS += -std=c99 -pedantic -Wall ${INCS} -DVERSION=\"${VERSION}\" -DNDEBUG ${CPPFLAGS}
LDFLAGS += ${LIBS}

DEBUG_CFLAGS = ${CFLAGS} -UNDEBUG -O0 -g -ggdb

CC ?= cc
STRIP ?= strip
