# Subhi-OS

Subhi-OS is an educational Linux-based virtual filesystem and storage engine implemented in C11.

## Features

- Disk-image-backed storage with 4096-byte blocks
- Robust block reads/writes and persistence flushing
- Version 2 filesystem format and superblock validation
- Block and inode allocation bitmaps
- Fixed-size inode records and root inode initialization
- Inode allocation, release, reuse, and exhaustion handling
- First-fit data-block allocation and reclamation
- Persistent free-block accounting
- Directory entries and path resolution
- File creation, reading, writing, unlinking, and permission checks
- CLI formatting and filesystem information commands
- Read-only filesystem consistency checking (`check <image>`)

## Requirements

- Linux (Ubuntu 24.04 / WSL2 supported)
- GCC with C11 support
- GNU Make
- Git

## Build and test

```sh
make
make test
```

The build uses strict compiler flags: `-std=c11 -Wall -Wextra -Wpedantic -Werror`.

## CLI usage

```sh
./build/subhi --help
./build/subhi --version
./build/subhi format /tmp/subhi.img 1024
./build/subhi info /tmp/subhi.img
./build/subhi check /tmp/subhi.img
```

`check` validates filesystem metadata and consistency without intentionally modifying the image. It reports bitmap/accounting mismatches, invalid inode records or direct pointers, duplicate block ownership, unowned allocated data blocks, and malformed or inconsistent directory entries. It does not repair corruption or provide journaling/crash recovery.

A consistent image returns exit status `0`; detected inconsistencies return `1`. Invalid invocation returns `2`.

## Filesystem design

The current on-disk format is version 2. It uses a superblock in block 0, a block bitmap, an inode bitmap, a fixed-size inode table, and a data region. Inode 0 is the root directory. Inodes contain up to eight direct data-block pointers; directory entries are 64 bytes each.
