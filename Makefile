PKG ?= pkgconf
PKG_CONFIG_PATH ?= /opt/homebrew/lib/pkgconf
export PKG_CONFIG_PATH

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 $(shell $(PKG) --cflags hidapi 2>/dev/null)
LDFLAGS ?= $(shell $(PKG) --libs hidapi 2>/dev/null)
TARGET ?= lunaar-switch
SRC := $(wildcard src/*.c)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean