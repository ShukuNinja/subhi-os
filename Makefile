CC := gcc

CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDFLAGS :=

TARGET := build/subhi
SOURCES := src/main.c src/vfs_disk.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)

TEST_DISK := build/test_disk

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@

build/%.o: src/%.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_DISK): tests/test_disk.c src/vfs_disk.c include/vfs_disk.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_disk.c src/vfs_disk.c -o $@

test: $(TARGET) $(TEST_DISK)
	bash tests/test_cli.sh
	./$(TEST_DISK)

clean:
	rm -rf build
