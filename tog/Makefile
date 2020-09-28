.PATH:${.CURDIR}/../lib

.include "../got-version.mk"

PROG=		tog
SRCS=		tog.c blame.c commit_graph.c delta.c diff.c \
		diffreg.c error.c fileindex.c object.c object_cache.c \
		object_idset.c object_parse.c opentemp.c path.c pack.c \
		privsep.c reference.c repository.c sha1.c worktree.c \
		utf8.c inflate.c buf.c rcsutil.c diff3.c \
		lockfile.c deflate.c object_create.c delta_cache.c \
		gotconfig.c
MAN =		${PROG}.1

CPPFLAGS = -I${.CURDIR}/../include -I${.CURDIR}/../lib
LDFLAGS += -L${.OBJDIR}/../openbsd-compat

.if defined(PROFILE)
LIBADD = panel_p ncursesw_p util_p z_p pthread_p c_p
.else
LIBADD = panel ncursesw util z pthread
.endif

.if ${GOT_RELEASE} != "Yes"
MAN=
.endif

.include <bsd.prog.mk>
