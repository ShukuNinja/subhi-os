
CC := gcc

CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDFLAGS :=

TARGET := build/subhi

SOURCES := src/main.c src/vfs_disk.c src/vfs_format.c \
           src/vfs_inode.c src/vfs_alloc.c src/vfs_dir.c \
           src/vfs_path.c

LIB_SOURCES := src/vfs_disk.c src/vfs_format.c \
               src/vfs_inode.c src/vfs_alloc.c \
               src/vfs_dir.c src/vfs_path.c

OBJECTS := $(SOURCES:src/%.c=build/%.o)

TEST_DISK := build/test_disk
TEST_FORMAT := build/test_format
TEST_INODE := build/test_inode
TEST_ALLOC := build/test_alloc
TEST_DIR := build/test_dir
TEST_PATH := build/test_path

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

$(TEST_FORMAT): tests/test_format.c src/vfs_disk.c \
                src/vfs_format.c src/vfs_inode.c \
                include/vfs_disk.h include/vfs_format.h \
                include/vfs_inode.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_format.c src/vfs_disk.c \
		src/vfs_format.c src/vfs_inode.c -o $@

$(TEST_INODE): tests/test_inode.c src/vfs_disk.c \
               src/vfs_format.c src/vfs_inode.c \
               include/vfs_disk.h include/vfs_format.h \
               include/vfs_inode.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_inode.c src/vfs_disk.c \
		src/vfs_format.c src/vfs_inode.c -o $@

$(TEST_ALLOC): tests/test_alloc.c src/vfs_disk.c \
               src/vfs_format.c src/vfs_inode.c src/vfs_alloc.c \
               include/vfs_disk.h include/vfs_format.h \
               include/vfs_inode.h include/vfs_alloc.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_alloc.c src/vfs_disk.c \
		src/vfs_format.c src/vfs_inode.c src/vfs_alloc.c -o $@

$(TEST_DIR): tests/test_dir.c $(LIB_SOURCES) \
             include/vfs_disk.h include/vfs_format.h \
             include/vfs_inode.h include/vfs_alloc.h \
             include/vfs_dir.h include/vfs_path.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_dir.c $(LIB_SOURCES) -o $@

$(TEST_PATH): tests/test_path.c $(LIB_SOURCES) \
              include/vfs_disk.h include/vfs_format.h \
              include/vfs_inode.h include/vfs_alloc.h \
              include/vfs_dir.h include/vfs_path.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_path.c $(LIB_SOURCES) -o $@

test: $(TEST_DISK) $(TEST_FORMAT) $(TEST_INODE) \
      $(TEST_ALLOC) $(TEST_DIR) $(TEST_PATH)
	./$(TEST_DISK)
	./$(TEST_FORMAT)
	./$(TEST_INODE)
	./$(TEST_ALLOC)
	./$(TEST_DIR)
	./$(TEST_PATH)
	bash ./tests/test_cli.sh

clean:
	rm -rf build
