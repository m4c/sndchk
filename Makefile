# Makefile for sndchk - Real-time audio diagnostics for FreeBSD

PROG=	sndchk
SRCS=	sndchk.c

CFLAGS+=	-Wall -Wextra -O2
LDFLAGS+=	

# FreeBSD standard install paths
PREFIX?=	/usr/local
BINDIR=		${PREFIX}/bin
MANDIR=		${PREFIX}/share/man/man1

.PHONY: all clean install uninstall

all: ${PROG}

${PROG}: ${SRCS}
	${CC} ${CFLAGS} -o ${PROG} ${SRCS} ${LDFLAGS}

clean:
	rm -f ${PROG} *.o *.core

install: ${PROG}
	install -m 755 ${PROG} ${BINDIR}/${PROG}

uninstall:
	rm -f ${BINDIR}/${PROG}
