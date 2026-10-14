CC := gcc

CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDFLAGS :=

TARGET := build/subhi

# Sources for the main executable.
SOURCES := src/main.c \
           src/vfs_disk.c \
           src/vfs_format.c \
           src/vfs_inode.c \
           src/vfs_alloc.c \
           src/vfs_dir.c \
           src/vfs_path.c \
           src/vfs_file.c \
           src/vfs_perm.c \
           src/vfs_check.c \
           src/vfs_namespace.c

OBJECTS := $(patsubst src/%.c,build/%.o,$(SOURCES))

# Core filesystem sources, excluding the checker.
# The checker is linked separately only where required.
LIB_SOURCES := src/vfs_disk.c \
               src/vfs_format.c \
               src/vfs_inode.c \
               src/vfs_alloc.c \
               src/vfs_dir.c \
               src/vfs_path.c \
               src/vfs_file.c \
               src/vfs_perm.c \
               src/vfs_namespace.c

TEST_DISK   := build/test_disk
TEST_FORMAT := build/test_format
TEST_INODE  := build/test_inode
TEST_ALLOC  := build/test_alloc
TEST_DIR    := build/test_dir
TEST_PATH   := build/test_path
TEST_FILE   := build/test_file
TEST_PERM   := build/test_perm
TEST_CHECK  := build/test_check
TEST_JOURNAL := build/test_journal

.PHONY: all clean test

all: $(TARGET)

# Main executable.
$(TARGET): $(OBJECTS)
	mkdir -p build
	$(CC) $(LDFLAGS) $^ -o $@

# Compile each source file into its corresponding object file.
build/%.o: src/%.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Virtual disk tests.
$(TEST_DISK): tests/test_disk.c src/vfs_disk.c include/vfs_disk.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_disk.c src/vfs_disk.c -o $@

# Filesystem format tests.
$(TEST_FORMAT): tests/test_format.c src/vfs_disk.c \
                src/vfs_format.c src/vfs_inode.c \
                include/vfs_disk.h include/vfs_format.h \
                include/vfs_inode.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_format.c src/vfs_disk.c \
		src/vfs_format.c src/vfs_inode.c -o $@

# Inode management tests.
$(TEST_INODE): tests/test_inode.c src/vfs_disk.c \
               src/vfs_format.c src/vfs_inode.c \
               include/vfs_disk.h include/vfs_format.h \
               include/vfs_inode.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_inode.c src/vfs_disk.c \
		src/vfs_format.c src/vfs_inode.c -o $@

# Block allocation tests.
$(TEST_ALLOC): tests/test_alloc.c src/vfs_disk.c \
               src/vfs_format.c src/vfs_inode.c src/vfs_alloc.c \
               include/vfs_disk.h include/vfs_format.h \
               include/vfs_inode.h include/vfs_alloc.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_alloc.c src/vfs_disk.c \
		src/vfs_format.c src/vfs_inode.c src/vfs_alloc.c -o $@

# Directory management tests.
$(TEST_DIR): tests/test_dir.c $(LIB_SOURCES) \
             include/vfs_disk.h include/vfs_format.h \
             include/vfs_inode.h include/vfs_alloc.h \
             include/vfs_dir.h include/vfs_path.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_dir.c $(LIB_SOURCES) -o $@

# Path resolution tests.
$(TEST_PATH): tests/test_path.c $(LIB_SOURCES) \
              include/vfs_disk.h include/vfs_format.h \
              include/vfs_inode.h include/vfs_alloc.h \
              include/vfs_dir.h include/vfs_path.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_path.c $(LIB_SOURCES) -o $@

# File operation tests.
$(TEST_FILE): tests/test_file.c $(LIB_SOURCES) \
              include/vfs_file.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_file.c $(LIB_SOURCES) -o $@

# Permission tests.
$(TEST_PERM): tests/test_perm.c src/vfs_perm.c \
              include/vfs_perm.h include/vfs_inode.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_perm.c src/vfs_perm.c -o $@

# Filesystem integrity checker tests.
$(TEST_CHECK): tests/test_check.c $(LIB_SOURCES) src/vfs_check.c \
               include/vfs_check.h include/vfs_dir.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		tests/test_check.c $(LIB_SOURCES) src/vfs_check.c -o $@

# Write-ahead journal recovery test.
$(TEST_JOURNAL): tests/test_journal.c src/vfs_disk.c src/vfs_format.c \
                 src/vfs_inode.c src/vfs_alloc.c include/vfs_disk.h \
                 include/vfs_format.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_journal.c src/vfs_disk.c \
		src/vfs_format.c src/vfs_inode.c src/vfs_alloc.c -o $@

# Run the complete test suite.
test: $(TARGET) $(TEST_DISK) $(TEST_FORMAT) $(TEST_INODE) \
      $(TEST_ALLOC) $(TEST_DIR) $(TEST_PATH) $(TEST_FILE) \
      $(TEST_PERM) $(TEST_CHECK) $(TEST_JOURNAL)
	./$(TEST_DISK)
	./$(TEST_FORMAT)
	./$(TEST_INODE)
	./$(TEST_ALLOC)
	./$(TEST_DIR)
	./$(TEST_PATH)
	./$(TEST_FILE)
	./$(TEST_PERM)
	./$(TEST_CHECK)
	./$(TEST_JOURNAL)
	bash ./tests/test_cli.sh

# Remove generated build artifacts.
clean:
	rm -rf build
