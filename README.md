# Subhi-OS

Subhi-OS is an educational Linux-based virtual filesystem and storage
engine implemented in C11.

## Current features

- Disk-image-backed storage with 4096-byte blocks
- Block-level reads, writes, and persistence checks
- Version 2 filesystem format and superblock validation
- Block-allocation bitmap
- Inode-allocation bitmap
- Fixed-size inode records
- Root inode initialization
- Inode allocation, release, reuse, and exhaustion handling
- First-fit data-block allocation and reclamation
- Persistent free-block accounting
- Command-line formatting and filesystem information commands

## Requirements

- Linux
- GCC with C11 support
- GNU Make
- Git

## Build

```sh
make
