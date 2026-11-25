# dmd5620: AT&T / Teletype DMD 5620 terminal emulator.
# Needs a C99 compiler, GNU make and SDL2. Nothing else.
#
#   make                 build ./dmd5620 (or dmd5620.exe with a MinGW CC)
#   make WERROR=1        warnings are errors (CI)
#   make DEBUG=1         no optimization, debug info
#   make STATIC=1        link SDL2 statically (release builds)
#   make SDL_CONFIG=/path/to/sdl2-config   use that SDL2, not pkg-config
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

.PHONY: all clean install uninstall

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

clean:
	rm -rf dmd5620 dmd5620.exe build/obj-*

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

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(EXE)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/dmd5620.1
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/48x48/apps/dmd5620.png
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/dmd5620.svg
	rm -f $(DESTDIR)$(PREFIX)/share/applications/com.loomcom.dmd5620.desktop
