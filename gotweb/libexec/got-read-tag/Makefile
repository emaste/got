
.include "../../../got-version.mk"
.include "../Makefile.inc"

PROG=		got-read-tag
SRCS=		got-read-tag.c error.c inflate.c object_parse.c \
		path.c privsep.c sha1.c

CPPFLAGS = -I${.CURDIR}/../../../include -I${.CURDIR}/../../../lib
LDFLAGS += -L${.OBJDIR}/../openbsd-compat
LIBADD = util z
LDSTATIC = ${STATIC}

.PATH:	${.CURDIR}/../../../lib ${.CURDIR}/../../../libexec/got-read-tag

.include <bsd.prog.mk>
