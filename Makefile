CC     = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
TARGET = better_autotile

all: $(TARGET)

$(TARGET): main.c vendor/cJSON.c
	$(CC) $(CFLAGS) -o $@ $^

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install clean
