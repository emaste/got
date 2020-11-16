CPPFLAGS += -DGOT_LIBEXECDIR=${LIBEXECDIR} -DGOT_VERSION=${GOT_VERSION}
#CFLAGS += -DGOT_PACK_NO_MMAP
#CFLAGS += -DGOT_NO_OBJ_CACHE
#CFLAGS += -DGOT_OBJ_CACHE_DEBUG

OPENBSD_COMPAT := ${.PARSEDIR}/openbsd-compat
CFLAGS += ${CPPFLAGS} -I${OPENBSD_COMPAT}
CFLAGS += -Wno-pointer-sign

LDADD += -lopenbsd-compat
LIBADD += md

WARNS = 0
CFLAGS += -fPIC

BINDIR ?= /usr/bin
LIBEXECDIR ?= /usr/libexec
MANDIR ?= /usr/share/man/man

CFLAGS += -Werror -Wall -Wstrict-prototypes -Wunused-variable

DEBUG = -O0 -g
