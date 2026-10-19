# pane — a Wayland display server written from scratch.
#
# Dependencies: libxkbcommon (+ xkeyboard-config data), zlib, and the kernel's API
# headers (<drm/drm.h>, <linux/input.h>), which LFS installs in chapter 5.

CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Isrc -Ibuild
XKB_CFLAGS := $(shell pkg-config --cflags xkbcommon 2>/dev/null)
XKB_LIBS   := $(shell pkg-config --libs xkbcommon 2>/dev/null || echo -lxkbcommon)
PREFIX  ?= /usr/local

SERVER_SRC = src/main.c src/event.c src/wire.c src/core.c src/compositor.c src/xdg.c \
             src/seat.c src/wm.c src/input.c src/render.c src/font.c src/css.c \
             src/backend_drm.c src/backend_fbdev.c src/backend_headless.c
CLIENT_SRC = client/demo.c src/wire.c

all: build/pane build/pane-demo

build/default_theme.inc: theme.css
	@mkdir -p build
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< > $@

build/pane: $(SERVER_SRC) src/*.h build/default_theme.inc
	$(CC) $(CFLAGS) $(XKB_CFLAGS) -o $@ $(SERVER_SRC) $(XKB_LIBS) -lz -lm

build/pane-demo: $(CLIENT_SRC) src/wire.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(XKB_CFLAGS) -o $@ $(CLIENT_SRC) $(XKB_LIBS) -lm

install: all
	install -Dm755 build/pane $(DESTDIR)$(PREFIX)/bin/pane
	install -Dm755 build/pane-demo $(DESTDIR)$(PREFIX)/bin/pane-demo
	install -Dm644 theme.css $(DESTDIR)$(PREFIX)/share/pane/theme.css

clean:
	rm -rf build

.PHONY: all install clean
