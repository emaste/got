.PATH:${.CURDIR}/../../lib

PROG = fetch_test
SRCS = error.c privsep.c reference.c sha1.c object.c object_parse.c path.c \
	opentemp.c repository.c lockfile.c object_cache.c pack.c inflate.c \
	deflate.c delta.c delta_cache.c object_idset.c object_create.c \
	fetch.c gotconfig.c fetch_test.c

CPPFLAGS = -I${.CURDIR}/../../include -I${.CURDIR}/../../lib
LDADD = -lutil -lz

MAN=

regress: ${PROG} .PHONY
	${.OBJDIR}/fetch_test -q

.include <bsd.prog.mk>
