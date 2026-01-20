
/**************************************************
UZI (Unix Z80 Implementation) Utilities:  mkfs.c
***************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#define BLKSIZE 512
#include "fuzix_fs.h"
#include "util.h"

/* This makes a filesystem 
 *
 * example use:
 *   ./mkfs ./blankfs.img 64 4096
 * (this will write a 2MB filesystem with 64 blocks of inodes to ./blankfs.img)
 *
 * */

char zero512[512];

direct dirbuf[64] = {
        { ROOTINODE, "." },
        { ROOTINODE, ".."}
};

	struct dinode inode[8];

	void dwrite(uint32_t blk, const void *addr);

	union disk {
		struct filesys fs;
	uint8_t zero[512];
} fs_super;

static void usage(void)
{
	printf("Usage: mkfs [-2] [-X] [-b blocksize] device isize fsize\n");
	exit(1);
}

static uint8_t validate(uint16_t bsize)
{
	switch(bsize) {
	case 512:
		return 0;
	case 1024:
		return 1;
	case 2048:
		return 2;
	case 4096:
		return 3;
	case 8192:
		return 4;
	case 16384:
		return 5;
	default:
		fprintf(stderr, "mkfs: unsupported block size.\n");
		exit(1);
	}
}

int main(int argc, char **argv)
{
	uint32_t fsize32, isize32;
	uint16_t bsize = 512, shift = 0;
	uint32_t j;
	uint32_t s;
	int opt;
	time_t t = time(NULL);
	uint8_t fs_v2 = 0;

	while((opt = getopt(argc, argv, "2Xb:")) != -1) {
		switch(opt) {
			case '2':
				fs_v2 = 1;
				break;
			case 'X':	
				swizzling = 1;
				break;
			case 'b':
				if (fs_v2) {
					fprintf(stderr, "mkfs: v2 requires 512-byte blocks.\n");
					exit(1);
				}
				bsize = atoi(optarg);
				shift = validate(bsize);
				break;
			default:
				usage();
		}
	}
	if (argc - optind != 3)
		usage();

	if (sizeof(inode) != 512) {
		printf("inode is the wrong size -- %d\n",
		       (int) sizeof(inode));
	}

	isize32 = (uint32_t) strtoul(argv[optind + 1], NULL, 0);
	fsize32 = (uint32_t) strtoul(argv[optind + 2], NULL, 0);

	if (fsize32 < 3 || isize32 < 2 || isize32 >= fsize32) {
		printf("Bad parameter values\n");
		return -1;
	}
	if (!fs_v2) {
		if (fsize32 > 65535 || isize32 > 65535) {
			fprintf(stderr, "mkfs: v1 is limited to 65535 blocks.\n");
			return -1;
		}
	}

	memset(zero512, 0, 512);

	printf("Making %s%d byte/block filesystem with %s byte order on device %s with fsize = %lu and isize = %lu.\n",
	       fs_v2 ? "v2 " : "",
	       bsize, swizzling==0 ? "normal" : "reversed", argv[optind],
	       (unsigned long)fsize32, (unsigned long)isize32);

	if (fd_open(argv[optind], O_CREAT)) {
		printf("Can't open device");
		return -1;
	}

	s = fsize32 << shift;

	/* v1: keep legacy behaviour (optionally larger block size via shift). */
	if (!fs_v2) {
		uint16_t fsize = (uint16_t)fsize32;
		uint16_t isize = (uint16_t)isize32;

		/* Zero out the blocks */
		for (j = 0; j < s; ++j)
			dwrite(j, zero512);

		/* Initialize the super-block */
		memset(&fs_super, 0, sizeof(fs_super));
		fs_super.fs.s_mounted = swizzle16(SMOUNTED);	/* Magic number */
		fs_super.fs.s_isize = swizzle16(isize);
		fs_super.fs.s_fsize = swizzle16(fsize);
		fs_super.fs.s_nfree = swizzle16(1);
		fs_super.fs.s_free[0] = 0;
		fs_super.fs.s_tfree = 0;
		fs_super.fs.s_ninode = 0;
		fs_super.fs.s_tinode = swizzle16(8 * (isize - 2) - 2);
		fs_super.fs.s_shift = shift;
		fs_super.fs.s_time = swizzle32(t);
		fs_super.fs.s_timeh = t >> 32;

		/* Free each block, building the free list. This is done in
		   terms of the block shift, while isize is in 512 byte blocks.
		   Adjust isize so that it's in block terms and references the
		   block after the last inode */
		isize <<= shift;

		/* Don't free the block isize because it's got the / directory in it */
		for (j = fsize - 1; j > isize; --j) {
			int n;
			if (swizzle16(fs_super.fs.s_nfree) == 50) {
				union {
					struct fuzix_freelist_v1 fl;
					uint8_t raw[512];
				} flb;
				memset(flb.raw, 0, sizeof(flb.raw));
				flb.fl.nfree = fs_super.fs.s_nfree;
				for (uint16_t i = 0; i < 50; i++)
					flb.fl.free[i] = fs_super.fs.s_free[i];
				dwrite(j, flb.raw);
				fs_super.fs.s_nfree = 0;
			}

			fs_super.fs.s_tfree =
			    swizzle16(swizzle16(fs_super.fs.s_tfree) + 1);
			n = swizzle16(fs_super.fs.s_nfree);
			fs_super.fs.s_free[n++] = swizzle16((uint16_t)j);
			fs_super.fs.s_nfree = swizzle16(n);
		}

		/* The inodes are already zeroed out */
		/* create the root dir */
		inode[ROOTINODE].i_mode = swizzle16(F_DIR | (0777 & MODE_MASK));
		inode[ROOTINODE].i_nlink = swizzle16(3);
		inode[ROOTINODE].i_size = swizzle32(64);
		inode[ROOTINODE].i_addr[0] = swizzle16(isize);

		/* Reserve reserved inode */
		inode[0].i_nlink = swizzle16(1);
		inode[0].i_mode = ~0;

		dwrite(2, inode);

		dirbuf[0].d_ino = swizzle16(dirbuf[0].d_ino);
		dirbuf[1].d_ino = swizzle16(dirbuf[1].d_ino);
		dwrite(isize, dirbuf);

		/* Write out super block */
		dwrite(1, &fs_super);
		return 0;
	}

	/* v2 (largefs): fixed 512-byte blocks, 32-bit block numbers. */
	{
		struct fuzix_dinode_v2 inode2[8];
		union {
			struct fuzix_filesys_v2 fs;
			uint8_t raw[512];
		} sb;
		union {
			struct fuzix_freelist_v2 fl;
			uint8_t raw[512];
		} flb;

		memset(inode2, 0, sizeof(inode2));
		memset(sb.raw, 0, sizeof(sb.raw));

		/* Clear only metadata blocks (avoid O(fsize) zeroing). */
		for (j = 0; j < isize32; ++j)
			dwrite(j, zero512);

		/* Initialize the superblock. */
		sb.fs.s_mounted = swizzle16(SMOUNTED_V2);
		sb.fs.s_pad0 = 0;
		sb.fs.s_isize = swizzle32(isize32);
		sb.fs.s_fsize = swizzle32(fsize32);
		sb.fs.s_nfree = swizzle16(1);
		sb.fs.s_pad1 = 0;
		sb.fs.s_free[0] = 0;
		sb.fs.s_tfree = 0;
		sb.fs.s_ninode = 0;
		{
			uint32_t tinode = 8U * (isize32 - 2U) - 2U;
			if (tinode > 0xFFFFU)
				tinode = 0xFFFFU;
			sb.fs.s_tinode = swizzle16((uint16_t)tinode);
		}
		sb.fs.s_shift = 0;
		sb.fs.s_pad2 = 0;
		sb.fs.s_time = swizzle32(t);
		sb.fs.s_timeh = t >> 32;

		/* Build free list (v2 free list block format). */
		for (j = fsize32 - 1; j > isize32; --j) {
			uint16_t n = swizzle16(sb.fs.s_nfree);
			if (n == 50) {
				memset(flb.raw, 0, sizeof(flb.raw));
				flb.fl.nfree = sb.fs.s_nfree;
				flb.fl.pad = 0;
				for (uint16_t i = 0; i < 50; i++)
					flb.fl.free[i] = sb.fs.s_free[i];
				dwrite(j, flb.raw);
				sb.fs.s_nfree = 0;
				n = 0;
			}
			sb.fs.s_tfree = swizzle32(swizzle32(sb.fs.s_tfree) + 1);
			sb.fs.s_free[n++] = swizzle32(j);
			sb.fs.s_nfree = swizzle16(n);
		}

		/* Root inode and directory. */
		inode2[ROOTINODE].i_mode = swizzle16(F_DIR | (0777 & MODE_MASK));
		inode2[ROOTINODE].i_nlink = swizzle16(3);
		inode2[ROOTINODE].i_size = swizzle32(64);
		inode2[ROOTINODE].i_addr[0] = swizzle32(isize32);

		/* Reserve reserved inode */
		inode2[0].i_nlink = swizzle16(1);
		inode2[0].i_mode = ~0;

		dwrite(2, inode2);

		dirbuf[0].d_ino = swizzle16(dirbuf[0].d_ino);
		dirbuf[1].d_ino = swizzle16(dirbuf[1].d_ino);
		dwrite(isize32, dirbuf);

		dwrite(1, sb.raw);
	}
	return 0;
}

void dwrite(uint32_t blk, const void *addr)
{
	if (lseek(dev_fd, (off_t)dev_offset + ((off_t)blk) * 512, SEEK_SET) == (off_t)-1) {
		perror("lseek");
		exit(1);
	}
	if (write(dev_fd, addr, 512) != 512) {
		perror("write");
		exit(1);
	}
}
