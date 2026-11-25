# dmd5620: AT&T / Teletype DMD 5620 terminal emulator.
# Needs a C99 compiler, GNU make and SDL2. Nothing else.
#
#   make                 build ./dmd5620 (or dmd5620.exe with a MinGW CC)
#   make WERROR=1        warnings are errors (CI)
#   make DEBUG=1         no optimization, debug info
#   make STATIC=1        link SDL2 statically (release builds)
#   make SDL_CONFIG=/path/to/sdl2-config   use that SDL2, not pkg-config
#   make layers          build layers (AT&T windows for the DMD), 32ld and the
#                        demos into ./layers
#   make install PREFIX=/usr/local
#
# FreeBSD: use gmake. Windows: MinGW-w64, cross or in MSYS2.

PREFIX  ?= /usr/local
PKG_CONFIG ?= pkg-config

TARGET := $(shell $(CC) -dumpmachine)
ifneq (,$(findstring mingw,$(TARGET)))
WINDOWS = 1
EXE     = dmd5620.exe
else
EXE     = dmd5620
endif

ifdef SDL_CONFIG
SDL_CFLAGS ?= $(shell $(SDL_CONFIG) --cflags)
SDL_LIBS   ?= $(shell $(SDL_CONFIG) $(if $(STATIC),--static-libs,--libs))
else
SDL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS   ?= $(shell $(PKG_CONFIG) $(if $(STATIC),--static) --libs sdl2)
endif

# CFLAGS, LDFLAGS and LDLIBS are yours (e.g. CFLAGS="-O2 -arch arm64").
CFLAGS  ?= -O2
XCFLAGS  = -std=gnu99 -fno-common -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare $(SDL_CFLAGS)
ifdef WERROR
XCFLAGS += -Werror
endif
ifdef DEBUG
XCFLAGS += -g -O0
endif
XLIBS    = $(SDL_LIBS)
XLDFLAGS =

ifdef WINDOWS
XLIBS   += -lws2_32 -lm
ifdef STATIC
XLDFLAGS += -static
endif
else
UNAME := $(shell uname -s)
ifneq (,$(filter Linux FreeBSD NetBSD OpenBSD DragonFly,$(UNAME)))
XLIBS   += -lutil
endif
ifneq ($(UNAME),Darwin)
XLIBS   += -lm
endif
endif

CORE_SRC = core/cpu.c core/bus.c core/duart.c core/dmd.c core/rom.c
FE_SRC   = $(wildcard src/*.c)
OBJDIR  ?= build/obj-$(TARGET)
OBJ      = $(addprefix $(OBJDIR)/,$(CORE_SRC:.c=.o) $(FE_SRC:.c=.o)) \
           $(if $(WINDOWS),$(OBJDIR)/dmd5620.res.o)

.PHONY: all clean install uninstall layers

all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(CFLAGS) $(XCFLAGS) $(LDFLAGS) $(XLDFLAGS) -o $@ $(OBJ) $(XLIBS) $(LDLIBS)

$(OBJDIR)/core/%.o: core/%.c core/core.h core/dmd_core.h | $(OBJDIR)/core
	$(CC) $(CFLAGS) $(XCFLAGS) -c -o $@ $<

$(OBJDIR)/core/cpu.o: core/optable.h

$(OBJDIR)/src/%.o: src/%.c $(wildcard src/*.h) core/dmd_core.h | $(OBJDIR)/src
	$(CC) $(CFLAGS) $(XCFLAGS) -c -o $@ $<

$(OBJDIR)/dmd5620.res.o: assets/dmd5620.rc assets/dmd5620.ico src/version.h | $(OBJDIR)
	$(WINDRES) -O coff -o $@ $<

$(OBJDIR) $(OBJDIR)/core $(OBJDIR)/src:
	mkdir -p $@

WINDRES ?= $(subst gcc,windres,$(CC))

# layers: the host side of the DMD's window system, from the git submodule
# dmd-layers; 32ld (loads programs into the DMD) and the demos, from the
# git submodule dev5620. They go to layers/ next to dmd5620, where dmd5620
# finds them and puts them on the shell's PATH. macOS, Linux and FreeBSD.
LAYERS_SRC ?= dmd-layers
DEV5620_SRC ?= dev5620
LAYERS_OUT ?= $(CURDIR)/layers
ifeq ($(UNAME),Linux)
LAYERS_CFLAGS ?= -O -w -Dconst=
LD32_CFLAGS ?= -O -w -std=gnu89
else
LAYERS_CFLAGS ?= -O -w -std=gnu89 -D_FORTIFY_SOURCE=0 -Wno-return-mismatch
LD32_CFLAGS ?= $(LAYERS_CFLAGS)
endif
LD32_SRC = $(addprefix $(DEV5620_SRC)/32ld/,32ld.c proto.c pcheck.c ver.c coff.c)

layers: $(LAYERS_SRC)/Makefile $(DEV5620_SRC)/32ld/32ld.c
	rm -rf $(LAYERS_OUT)
	$(MAKE) -j1 -C $(LAYERS_SRC) clean
	$(MAKE) -j1 -C $(LAYERS_SRC) DMD=$(LAYERS_OUT) EXTRACFLAGS="$(LAYERS_CFLAGS)" install
	rm -rf $(LAYERS_OUT)/xt
	$(CC) $(LD32_CFLAGS) -DUSE_HOSTAGENT -I$(DEV5620_SRC)/32ld -I$(LAYERS_SRC)/include \
		-o $(LAYERS_OUT)/bin/32ld $(LD32_SRC) $(LAYERS_OUT)/lib/libwindows.a
	cp -R $(DEV5620_SRC)/dmdcore/. $(LAYERS_OUT)/
	@for f in bin/layers bin/jwin bin/xtagent bin/32ld bin/demo lib/layersys/movelogin; do \
		test -x $(LAYERS_OUT)/$$f || { echo "layers: $$f was not built"; exit 1; }; \
	done

$(LAYERS_SRC)/Makefile:
	git submodule update --init $(LAYERS_SRC)

$(DEV5620_SRC)/32ld/32ld.c:
	git submodule update --init $(DEV5620_SRC)

clean:
	rm -rf dmd5620 dmd5620.exe build/obj-* layers

install: $(EXE)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(EXE) $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 dmd5620.man $(DESTDIR)$(PREFIX)/share/man/man1/dmd5620.1
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/48x48/apps
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 644 assets/dmd5620.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/48x48/apps
	install -m 644 assets/dmd5620.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 644 assets/com.loomcom.dmd5620.desktop $(DESTDIR)$(PREFIX)/share/applications
	if [ -x layers/bin/layers ]; then \
		install -d $(DESTDIR)$(PREFIX)/lib/dmd5620 && \
		rm -rf $(DESTDIR)$(PREFIX)/lib/dmd5620/layers && \
		cp -R layers $(DESTDIR)$(PREFIX)/lib/dmd5620/; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(EXE)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/dmd5620.1
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/48x48/apps/dmd5620.png
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/dmd5620.svg
	rm -f $(DESTDIR)$(PREFIX)/share/applications/com.loomcom.dmd5620.desktop
	rm -rf $(DESTDIR)$(PREFIX)/lib/dmd5620
