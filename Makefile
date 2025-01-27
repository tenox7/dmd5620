SRCDIR = src
LIBDIR = dmd_core
GIT = git
CARGO = cargo
CFLAGS = $(shell pkg-config --cflags sdl2 SDL2_gfx) -Wall -Werror -std=gnu99
GTKLIBS = $(shell pkg-config --libs sdl2 SDL2_gfx)
EXE = dmd5620
CSRC = $(wildcard src/*.c)
OBJ = $(CSRC:.c=.o)
LDFLAGS = $(GTKLIBS) -lm -lpthread -lc -ldl -lutil
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

$(EXE): $(CORELIB) $(OBJ)
	@$(CC) $(CFLAGS) -o $@ $^ $(CORELIB) $(LDFLAGS)

install: $(EXE)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(EXE) $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/man/man1
	install -m 644 dmd5620.man $(DESTDIR)$(PREFIX)/man/man1/dmd5620.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(EXE)
	rm -f $(DESTDIR)$(PREFIX)/man/man1/dmd5620.1
