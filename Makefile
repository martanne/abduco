-include config.mk

VERSION = 0.6

CFLAGS_STD ?= -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -DNDEBUG
CFLAGS_STD += -DVERSION=\"${VERSION}\"

LDFLAGS_STD ?= -lc -lutil

STRIP ?= strip
INSTALL ?= install

ZSHPREFIX ?= /usr/local

SRC = abduco.c

all: abduco

config.h:
	cp config.def.h config.h

config.mk:
	@touch $@

abduco: config.h config.mk *.c
	${CC} ${CFLAGS} ${CFLAGS_STD} ${CFLAGS_AUTO} ${CFLAGS_EXTRA} ${SRC} ${LDFLAGS} ${LDFLAGS_STD} ${LDFLAGS_AUTO} -o $@

debug: clean
	make CFLAGS_EXTRA='${CFLAGS_DEBUG}'

clean:
	@echo cleaning
	@rm -f abduco abduco-*.tar.gz

dist: clean
	@echo creating dist tarball
	@git archive --prefix=abduco-${VERSION}/ -o abduco-${VERSION}.tar.gz HEAD

install: abduco
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@${INSTALL} abduco ${DESTDIR}${PREFIX}/bin
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@sed "s/VERSION/${VERSION}/g" < abduco.1 > ${DESTDIR}${MANPREFIX}/man1/abduco.1
	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/abduco.1
	@echo installing zsh completion file to ${DESTDIR}${ZSHPREFIX}/share/zsh/site-functions
	@install -Dm644 abduco.zsh ${DESTDIR}${ZSHPREFIX}/share/zsh/site-functions/_abduco

install-strip: install
	${STRIP} ${DESTDIR}${PREFIX}/bin/abduco

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/abduco
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/abduco.1
	@echo removing zsh completion file from ${DESTDIR}/usr/share/zsh/site-functions
	@rm -f ${DESTDIR}${ZSHPREFIX}/share/zsh/site-functions/_abduco

.PHONY: all clean dist install install-strip uninstall debug
