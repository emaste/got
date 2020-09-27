.PATH:${.CURDIR}/../../lib

.include "../../got-version.mk"

PROG=		got-fetch-pack
SRCS=		got-fetch-pack.c error.c inflate.c object_parse.c \
		path.c privsep.c sha1.c

CPPFLAGS = -I${.CURDIR}/../../include -I${.CURDIR}/../../lib
LIBADD = util z

.include <bsd.prog.mk>
