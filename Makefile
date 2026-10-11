CC := gcc

CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDFLAGS :=

TARGET := build/subhi
SOURCES := src/main.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@

build/%.o: src/%.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	bash tests/test_cli.sh

clean:
	rm -rf build
