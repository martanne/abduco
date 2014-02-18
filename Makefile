include config.mk

SRC += abduco.c
OBJ = ${SRC:.c=.o}

all: clean options abduco

options:
	@echo abduco build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

config.h:
	cp config.def.h config.h

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk

abduco: ${OBJ}
	@echo CC -o $@
	@${CC} -o $@ ${OBJ} ${LDFLAGS}

debug: clean
	@make CFLAGS='${DEBUG_CFLAGS}'

clean:
	@echo cleaning
	@rm -f abduco ${OBJ} abduco-${VERSION}.tar.gz

dist: clean
	@echo creating dist tarball
	@mkdir -p abduco-${VERSION}
	@cp -R LICENSE Makefile README config.def.h config.mk \
		${SRC} debug.c client.c server.c forkpty-aix.c abduco.1 abduco-${VERSION}
	@tar -cf abduco-${VERSION}.tar abduco-${VERSION}
	@gzip abduco-${VERSION}.tar
	@rm -rf abduco-${VERSION}

install: abduco
	@echo stripping executable
	@strip -s abduco
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@cp -f abduco ${DESTDIR}${PREFIX}/bin
	@chmod 755 ${DESTDIR}${PREFIX}/bin/abduco
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@sed "s/VERSION/${VERSION}/g" < abduco.1 > ${DESTDIR}${MANPREFIX}/man1/abduco.1
	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/abduco.1

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/abduco
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/abduco.1

.PHONY: all options clean dist install uninstall debug
