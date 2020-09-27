.PATH:${.CURDIR}/../../lib

PROG = delta_test
SRCS = delta.c error.c opentemp.c path.c inflate.c sha1.c delta_test.c

CPPFLAGS = -I${.CURDIR}/../../include -I${.CURDIR}/../../lib
LIBADD = z

MAN=

regress: ${PROG} .PHONY
	${.OBJDIR}/delta_test -q

.include <bsd.prog.mk>
