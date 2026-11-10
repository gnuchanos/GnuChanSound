CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
PREFIX ?= /usr/local
DESTDIR ?=
VERSION := $(shell cat version.txt)
OUTPUT := build/GnuChanSound
SOURCES := src/main.c
PACKAGES := x11 xft

.PHONY: all build clean install uninstall run check

all: build

build: $(OUTPUT)

$(OUTPUT): $(SOURCES) version.txt
	@command -v $(PKG_CONFIG) >/dev/null || { echo "error: pkg-config is required" >&2; exit 1; }
	@$(PKG_CONFIG) --exists $(PACKAGES) || { echo "error: install libx11-dev, libxft-dev, and pkg-config" >&2; exit 1; }
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Wpedantic $(CFLAGS) \
		-DGNUCHAN_SOUND_VERSION='"$(VERSION)"' \
		$(SOURCES) -o $(OUTPUT) \
		$$($(PKG_CONFIG) --cflags --libs $(PACKAGES)) -lm

run: build
	./$(OUTPUT)

check: build
	./$(OUTPUT) --version
	./$(OUTPUT) --self-test
	BIN="$(OUTPUT)" sh tests/smoke.sh
	OUTPUT="$(OUTPUT)" sh tests/install.sh

install: build
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" sh ./install.sh install

uninstall:
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" sh ./install.sh uninstall

clean:
	rm -rf build
