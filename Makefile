SRCDIR = src
LIBDIR = dmd_core
GIT = git
CARGO = cargo
CFLAGS = $(shell pkg-config --cflags sdl2) -Wall -Werror -std=gnu99
SDLLIBS = $(shell pkg-config --libs sdl2)
UNAME := $(shell uname -s)
EXE = dmd5620
CSRC = $(wildcard src/*.c)
OBJ = $(CSRC:.c=.o)
LDFLAGS = $(SDLLIBS) -lz -lm -lpthread -lc -lutil
ifeq ($(UNAME),Darwin)
LDFLAGS += -liconv
else
LDFLAGS += -ldl
endif
CORELIB = $(LIBDIR)/target/release/libdmd_core.a

ifeq ($(PREFIX),)
PREFIX := /usr/local
endif

ifdef DEBUG
	CFLAGS+ = -g -O0
else
	CFLAGS += -O3 -Os
endif

.PHONY: all clean

all: $(EXE)

clean:
	@rm -f $(EXE) $(OBJ)

deepclean: clean
	@cd $(LIBDIR) && $(CARGO) clean

$(CORELIB):
	$(if $(wildcard ./dmd_core/Cargo.toml),,$(error The submodule dmd_core is not checked out.))
	@cd $(LIBDIR) && $(CARGO) build --release

$(OBJ): $(wildcard src/*.h)

$(EXE): $(CORELIB) $(OBJ)
	@$(CC) $(CFLAGS) -o $@ $(OBJ) $(CORELIB) $(LDFLAGS)

install: $(EXE)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(EXE) $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/man/man1
	install -m 644 dmd5620.man $(DESTDIR)$(PREFIX)/man/man1/dmd5620.1
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/48x48/apps
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 644 assets/dmd5620.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/48x48/apps
	install -m 644 assets/dmd5620.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 644 assets/com.loomcom.dmd5620.desktop $(DESTDIR)$(PREFIX)/share/applications

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(EXE)
	rm -f $(DESTDIR)$(PREFIX)/man/man1/dmd5620.1
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/48x48/apps/dmd5620.png
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/dmd5620.png
	rm -f $(DESTDIR)$(PREFIX)/share/applications/com.loomcom.dmd5620.desktop
