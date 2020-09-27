.PATH:${.CURDIR}/../../lib

.include "../../got-version.mk"

PROG=		got-read-gitconfig
SRCS=		got-read-gitconfig.c error.c inflate.c object_parse.c \
		path.c privsep.c sha1.c gitconfig.c

CPPFLAGS = -I${.CURDIR}/../../include -I${.CURDIR}/../../lib
LIBADD = util z

.include <bsd.prog.mk>
