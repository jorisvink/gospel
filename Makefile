# litany for weechat (gospel) Makefile

CC?=cc
OBJDIR?=obj

PLUGIN=gospel.so
SHARED_FLAGS=-shared

CFLAGS+=-std=c99 -Wall -Werror -Wstrict-prototypes
CFLAGS+=-Wmissing-prototypes -Wmissing-declarations -Wshadow
CFLAGS+=-Wpointer-arith -Wcast-qual -Wsign-compare -O2 -fPIC
CFLAGS+=-fstack-protector-all -Wtype-limits -fno-common -g
CFLAGS+=-Iinclude

CFLAGS+=$(shell pkg-config --cflags libkyrka)
CFLAGS+=$(shell pkg-config --cflags weechat)

LDFLAGS+=$(shell pkg-config --libs libkyrka)

SRC=	src/plugin.c \
	src/chat.c \
	src/config.c \
	src/gospel.c \
	src/liturgy.c \
	src/tunnel.c

ifeq ("$(SANITIZE)", "1")
	CFLAGS+=-fsanitize=address,undefined
endif

ifeq ("$(COVERAGE)", "1")
	CFLAGS+=-fprofile-arcs -ftest-coverage
endif

ifeq ("$(OSNAME)", "")
OSNAME=$(shell uname -s | sed -e 's/[-_].*//g' | tr A-Z a-z)
endif

ifeq ("$(OSNAME)", "linux")
	CFLAGS+=-DPLATFORM_LINUX
	CFLAGS+=-D_GNU_SOURCE=1 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
else ifeq ("$(OSNAME)", "darwin")
	SHARED_FLAGS+=-Wl,-force_load $(LIBKYRKA)/lib/libkyrka.a
	CFLAGS+=-DPLATFORM_DARWIN
else ifeq ("$(OSNAME)", "openbsd")
	CFLAGS+=-DPLATFORM_OPENBSD
endif

all: $(PLUGIN)

OBJS=	$(SRC:%.c=$(OBJDIR)/%.o)

$(PLUGIN): $(OBJDIR) $(OBJS)
	$(CC) $(SHARED_FLAGS) $(OBJS) $(LDFLAGS) -o $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(shell dirname $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(PLUGIN)

.PHONY: all clean force
