.PATH:${.CURDIR}/../../lib

.include "../../got-version.mk"

PROG=		got-read-pack
SRCS=		got-read-pack.c delta.c error.c inflate.c object_cache.c \
		object_idset.c object_parse.c opentemp.c pack.c path.c \
		privsep.c sha1.c delta_cache.c

CPPFLAGS = -I${.CURDIR}/../../include -I${.CURDIR}/../../lib
LDADD = -lutil -lz
DPADD = ${LIBZ} ${LIBUTIL}

.include <bsd.prog.mk>
