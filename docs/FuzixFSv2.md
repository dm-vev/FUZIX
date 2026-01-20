# FUZIX filesystem v2 (large filesystem support)

This document describes the planned/implemented on‑disk format update needed
to support filesystems significantly larger than 32MB (e.g. 32GB SD card
partitions) on platforms such as `rpipico` (RP2040/RP2350).

## Background (current v1 limits)

The classic UZI/FUZIX filesystem format (“v1”) uses:

- 512‑byte blocks
- 16‑bit block numbers in metadata (superblock freelist and inode `i_addr[]`)
- a V6‑style block mapping (direct + single + double indirect)

This implies:

- Max addressable blocks: `2^16 = 65536` → `65536 * 512 = 32MB` filesystem limit.
- Kernel also enforces a ~32MB file size limit for the same reason.

## Goals

- Support filesystems up to at least 32GB on 512‑byte block devices.
- Keep v1 compatibility (mount and operate existing small filesystems).
- Keep kernel small and predictable (no dynamic allocation for core metadata).
- Maintain reasonable maximum file size on v2 (>= hundreds of MB).

## On‑disk layout (unchanged)

Block numbers below are filesystem block numbers (512 bytes).

- Block `0`: boot block / unused
- Block `1`: superblock
- Blocks `2..(isize-1)`: inode table
- Blocks `isize..(fsize-1)`: data blocks

Directory entries remain unchanged (`uint16_t inode` + 30‑byte name).

## Superblock

### v1 (legacy)

Uses 16‑bit `s_isize/s_fsize` and 16‑bit freelist block numbers.

### v2 (largefs)

Key changes:

- `s_isize/s_fsize/s_tfree`: 32‑bit
- freelist block numbers: 32‑bit
- new magic value (distinct from v1) to indicate v2

The freelist “stack of blocks” algorithm remains the same, but the on‑disk
freelist block format uses 32‑bit block numbers.

## Inodes and block mapping

### v1 (legacy)

- Inode size: 64 bytes
- `i_addr[20]` contains 18 direct + single + double indirect pointers
- Indirect blocks contain 16‑bit block numbers (256 entries per block)

### v2 (largefs)

- Inode size remains 64 bytes (keeps inode table density unchanged)
- Block pointers are 32‑bit (128 entries per indirect block)
- Mapping uses fewer direct pointers but adds triple‑indirect to preserve file
  size capability:

  - 7 direct
  - 1 single indirect
  - 1 double indirect
  - 1 triple indirect

This yields a max file size of approximately:

`(7 + 128 + 128^2 + 128^3) * 512 ~= 1.08GiB`

## Compatibility and versioning

- v1 and v2 are distinguished by the superblock magic.
- Kernel selects v1/v2 code paths per mounted filesystem.
- Userspace tools (mkfs/fsck/statvfs/df) must understand both formats to be
  useful on mixed systems.

## Tooling

Host utilities in `Standalone/` support creating and checking v2 filesystems:

- Create a v2 filesystem (512-byte blocks only):
  - `Standalone/mkfs -2 <device|image> <isize> <fsize>`
- Check/repair:
  - `Standalone/fsck -y <device|image>`

For large partitions (e.g. SD cards), creating/checking the filesystem on the
host is recommended.
