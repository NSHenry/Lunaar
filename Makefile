PKG ?= pkgconf
PKG_CONFIG_PATH ?= /opt/homebrew/lib/pkgconf
export PKG_CONFIG_PATH

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 $(shell $(PKG) --cflags hidapi 2>/dev/null)
LDFLAGS ?= $(shell $(PKG) --libs hidapi 2>/dev/null)
BIN_DIR ?= bin
TARGET ?= $(BIN_DIR)/lunaar-switch
SRC := $(wildcard src/*.c)

$(TARGET): $(SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BIN_DIR)

.PHONY: clean