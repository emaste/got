
.include "../../../got-version.mk"
.include "../Makefile.inc"

PROG=		got-read-gitconfig
SRCS=		got-read-gitconfig.c error.c inflate.c object_parse.c \
		path.c privsep.c sha1.c gitconfig.c

CPPFLAGS = -I${.CURDIR}/../../../include -I${.CURDIR}/../../../lib
LDFLAGS += -L${.OBJDIR}/../openbsd-compat
LIBADD = util z
LDSTATIC = ${STATIC}

.PATH:	${.CURDIR}/../../../lib ${.CURDIR}/../../../libexec/got-read-gitconfig

.include <bsd.prog.mk>
