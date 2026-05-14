CC     = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
TARGET = better_autotile

CJSON_CFLAGS  := $(shell pkg-config --cflags libcjson 2>/dev/null)
CJSON_LDFLAGS := $(shell pkg-config --libs   libcjson 2>/dev/null || echo "-lcjson")

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ $< $(CJSON_LDFLAGS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install clean
