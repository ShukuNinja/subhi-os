# Subhi-OS

Subhi-OS is an educational Linux-based virtual filesystem and storage engine implemented in C11. It uses a persistent disk-image-backed storage layer and uses **filesystem format version 3**.

## Implemented capabilities

- Disk-image storage with 4096-byte blocks and POSIX `pread`, `pwrite`, and `fdatasync`
- Version-3 superblock, allocation bitmaps, fixed-size inodes, and direct data-block pointers
- File create, read, write, and deletion through the CLI
- Directory create, listing, and empty-directory removal through the CLI
- Absolute-path resolution and permission checks in the filesystem library
- Process-local write-through block cache (16 blocks) with physical-I/O/cache counters
- Process-local sorted directory index (up to four indexed directories per process), invalidated after directory mutations
- Storage utilization and per-invocation I/O statistics via `stats`
- Read-only filesystem consistency validation via `check`

## Requirements

- Linux (Ubuntu 24.04 / WSL2 supported)
- GCC with C11 support
- GNU Make

## Build and test

```sh
make clean
make
make test
```

The build uses `-std=c11 -Wall -Wextra -Wpedantic -Werror -g`.

## CLI usage

```sh
./build/subhi --help
./build/subhi --version
./build/subhi format /tmp/subhi.img 1024
./build/subhi info /tmp/subhi.img
./build/subhi mkdir /tmp/subhi.img /docs
./build/subhi create /tmp/subhi.img /docs/notes.txt
./build/subhi write /tmp/subhi.img /docs/notes.txt "My persistent note"
./build/subhi read /tmp/subhi.img /docs/notes.txt
./build/subhi ls /tmp/subhi.img /docs
./build/subhi stats /tmp/subhi.img
./build/subhi check /tmp/subhi.img
./build/subhi rm /tmp/subhi.img /docs/notes.txt
./build/subhi rmdir /tmp/subhi.img /docs
```

Formatting creates a new image and refuses to overwrite an existing path. File contents are replaced by `write`; the current maximum file size is 32 KiB. Paths must be absolute. `rmdir` only removes empty directories. The CLI uses virtual filesystem administrator credentials (UID/GID 0); permission checks remain implemented in the library, but this version does not yet provide a user/account management interface.

## Indexing, caching, and metrics

The block cache is a process-local, write-through LRU cache. Directory lookups use a process-local sorted index rebuilt from on-disk directory entries and invalidated after mutations. These caches are volatile and are rebuilt after each CLI process starts; they do not change the version-3 disk layout.

`stats` derives current block/inode utilization from on-disk metadata and reports I/O/cache counters and a measured rate for the statistics scan itself. These are **per-invocation** measurements, not long-term throughput history or benchmark results. Directory indexing and block caching are process-local, so separate one-command CLI invocations do not share their caches.

## Version 3 journaling and recovery limits

Version 3 reserves blocks 1–2 for a write-ahead undo journal; the allocation bitmap and subsequent metadata are shifted accordingly. Each filesystem block write records and flushes the old block image and a checksummed active journal header before changing the target block. On opening an image, a valid pending journal entry is replayed by restoring the previous block image. A completed single-block write is therefore recoverable to its previous contents after an interrupted write.

**Scope limitation:** this is a single-block undo journal, not a multi-block transaction manager. It recovers the block update that was in progress, but it does not roll back all earlier block writes from a larger operation. A crash between separate writes in a multi-step operation can still leave cross-block metadata inconsistent. The integrity checker detects supported inconsistencies but does not repair them. The journal is educational and does not claim production-grade guarantees against arbitrary torn writes, concurrent processes, or hardware that lies about flush completion. Version-2 images are not automatically migrated; format a new image for Version 3 and preserve any Version-2 image you need.

## Integrity checking and exit status

`check <image>` reads filesystem metadata and reports detected inconsistencies without intentionally modifying the image. It returns `0` for a consistent filesystem, `1` for detected inconsistencies or operational failures, and `2` for invalid CLI usage.
