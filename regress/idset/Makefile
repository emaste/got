.PATH:${.CURDIR}/../../lib

PROG = idset_test
SRCS = error.c sha1.c object_idset.c inflate.c path.c object_parse.c \
	idset_test.c

CPPFLAGS = -I${.CURDIR}/../../include -I${.CURDIR}/../../lib
LIBADD = util z

MAN=

regress: ${PROG} .PHONY
	${.OBJDIR}/idset_test -q

.include <bsd.prog.mk>
