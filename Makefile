CC := gcc

CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDFLAGS :=

TARGET := build/subhi
SOURCES := src/main.c src/vfs_disk.c src/vfs_format.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)

TEST_DISK := build/test_disk
TEST_FORMAT := build/test_format

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

$(TEST_FORMAT): tests/test_format.c src/vfs_disk.c src/vfs_format.c \
                include/vfs_disk.h include/vfs_format.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_format.c src/vfs_disk.c src/vfs_format.c -o $@

test: $(TARGET) $(TEST_DISK) $(TEST_FORMAT)
	bash tests/test_cli.sh
	./$(TEST_DISK)
	./$(TEST_FORMAT)

clean:
	rm -rf build
