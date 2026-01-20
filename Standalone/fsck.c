#define BLKSIZE 512

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "fuzix_fs.h"
#include "util.h"

typedef uint32_t fsblk_t;

#define MAXDEPTH 20 /* Maximum depth of directory tree to search */

/* Superblock s_fmod values */
#define FMOD_DIRTY 1
#define FMOD_CLEAN 2

struct fsck_super {
	uint16_t mounted; /* host order: SMOUNTED or SMOUNTED_V2 */
	uint32_t isize;
	uint32_t fsize;
	uint16_t nfree;
	fsblk_t free[50];
	int16_t ninode;
	uint16_t inode[50];
	uint8_t fmod;
	uint8_t timeh;
	uint32_t time;
	fsblk_t tfree;
	uint16_t tinode;
	uint8_t shift;
};

struct fsck_inode {
	uint16_t mode;
	uint16_t nlink;
	uint16_t uid;
	uint16_t gid;
	uint32_t size;
	uint32_t atime;
	uint32_t mtime;
	uint32_t ctime;
	fsblk_t addr[20];
};

static int aflag;
static int yflag;
static int error_flags;

static int fs_v2;
static struct fsck_super super;
static uint32_t inode_count;

static uint8_t *bitmap;
static int32_t *linkmap;

static void fsck_panic(const char *s)
{
	fprintf(stderr, "panic: %s\n", s);
	exit(error_flags | 8);
}

static int yes_noerror(void)
{
	static char buf[16];

	if (yflag) {
		puts("y");
		return 1;
	}
	for (;;) {
		if (fgets(buf, sizeof(buf), stdin) == NULL)
			exit(error_flags | 8);
		if (isupper((unsigned char)buf[0]))
			buf[0] = (char)tolower((unsigned char)buf[0]);
		if (buf[0] == 'y')
			return 1;
		if (buf[0] == 'n')
			return 0;
	}
}

static int yes(void)
{
	int ret = yes_noerror();

	if (ret)
		error_flags |= 1;
	else
		error_flags |= 4;
	return ret;
}

static void bitmap_set(fsblk_t b)
{
	bitmap[b >> 3] |= (uint8_t)(1U << (b & 7));
}

static int bitmap_test(fsblk_t b)
{
	return (bitmap[b >> 3] & (uint8_t)(1U << (b & 7))) ? 1 : 0;
}

static void read_block(fsblk_t blk, uint8_t buf[512])
{
	if (blk > (fsblk_t)UINT_MAX) {
		fprintf(stderr, "fsck: block %lu too large for backend\n",
		        (unsigned long)blk);
		exit(error_flags | 8);
	}
	if (bdread((unsigned int)blk, buf) < 0)
		exit(error_flags | 8);
}

static void write_block(fsblk_t blk, uint8_t buf[512])
{
	if (blk > (fsblk_t)UINT_MAX) {
		fprintf(stderr, "fsck: block %lu too large for backend\n",
		        (unsigned long)blk);
		exit(error_flags | 8);
	}
	if (bdwrite((unsigned int)blk, buf))
		exit(error_flags | 8);
}

static void copy_block(fsblk_t dst, fsblk_t src)
{
	uint8_t buf[512];

	read_block(src, buf);
	write_block(dst, buf);
}

static int16_t swizzle_s16(int16_t v)
{
	return (int16_t)swizzle16((uint16_t)v);
}

static uint16_t blk_get_u16(const uint8_t *buf, uint16_t idx)
{
	uint16_t v;
	memcpy(&v, buf + (idx * sizeof(v)), sizeof(v));
	return swizzle16(v);
}

static void blk_set_u16(uint8_t *buf, uint16_t idx, uint16_t v)
{
	uint16_t out = swizzle16(v);
	memcpy(buf + (idx * sizeof(out)), &out, sizeof(out));
}

static uint32_t blk_get_u32(const uint8_t *buf, uint16_t idx)
{
	uint32_t v;
	memcpy(&v, buf + (idx * sizeof(v)), sizeof(v));
	return swizzle32(v);
}

static void blk_set_u32(uint8_t *buf, uint16_t idx, uint32_t v)
{
	uint32_t out = swizzle32(v);
	memcpy(buf + (idx * sizeof(out)), &out, sizeof(out));
}

static int blk_in_range(fsblk_t blk)
{
	return (blk >= super.isize && blk < super.fsize);
}

static void super_read(void)
{
	uint8_t raw[512];
	uint16_t magic;

	read_block(1, raw);
	memcpy(&magic, raw, sizeof(magic));

	if (magic == SMOUNTED) {
		swizzling = 0;
		fs_v2 = 0;
	} else if (magic == SMOUNTED_V2) {
		swizzling = 0;
		fs_v2 = 1;
	} else if (magic == SMOUNTED_WRONGENDIAN) {
		swizzling = 1;
		fs_v2 = 0;
		puts("Checking filesystem with reversed byte order.");
	} else if (magic == SMOUNTED_V2_WRONGENDIAN) {
		swizzling = 1;
		fs_v2 = 1;
		puts("Checking v2 filesystem with reversed byte order.");
	} else {
		fprintf(stderr, "fsck: invalid magic %u\n", magic);
		exit(16);
	}

	memset(&super, 0, sizeof(super));

	if (fs_v2) {
		struct fuzix_filesys_v2 sb;
		memcpy(&sb, raw, sizeof(sb));
		super.mounted = SMOUNTED_V2;
		super.isize = swizzle32(sb.s_isize);
		super.fsize = swizzle32(sb.s_fsize);
		super.nfree = swizzle16(sb.s_nfree);
		for (uint16_t i = 0; i < 50; i++)
			super.free[i] = swizzle32(sb.s_free[i]);
		super.ninode = (int16_t)swizzle_s16(sb.s_ninode);
		for (uint16_t i = 0; i < 50; i++)
			super.inode[i] = swizzle16(sb.s_inode[i]);
		super.fmod = sb.s_fmod;
		super.timeh = sb.s_timeh;
		super.time = swizzle32(sb.s_time);
		super.tfree = swizzle32(sb.s_tfree);
		super.tinode = swizzle16(sb.s_tinode);
		super.shift = sb.s_shift;
	} else {
		struct fuzix_filesys_v1 sb;
		memcpy(&sb, raw, sizeof(sb));
		super.mounted = SMOUNTED;
		super.isize = swizzle16(sb.s_isize);
		super.fsize = swizzle16(sb.s_fsize);
		super.nfree = swizzle16(sb.s_nfree);
		for (uint16_t i = 0; i < 50; i++)
			super.free[i] = swizzle16(sb.s_free[i]);
		super.ninode = (int16_t)swizzle_s16(sb.s_ninode);
		for (uint16_t i = 0; i < 50; i++)
			super.inode[i] = swizzle16(sb.s_inode[i]);
		super.fmod = sb.s_fmod;
		super.timeh = sb.s_timeh;
		super.time = swizzle32(sb.s_time);
		super.tfree = swizzle16(sb.s_tfree);
		super.tinode = swizzle16(sb.s_tinode);
		super.shift = sb.s_shift;
	}

	if (super.fsize < 3 || super.isize < 2 || super.isize >= super.fsize) {
		fprintf(stderr, "fsck: invalid isize/fsize (isize=%lu fsize=%lu)\n",
		        (unsigned long)super.isize, (unsigned long)super.fsize);
		exit(16);
	}

	if (super.shift != 0) {
		fprintf(stderr, "fsck: unsupported s_shift=%u (only 0 supported)\n",
		        (unsigned)super.shift);
		exit(16);
	}

	/* Inode numbers are 16-bit. */
	inode_count = 8U * (super.isize - 2U);
	if (inode_count > 65536U)
		inode_count = 65536U;
}

static void super_write(void)
{
	uint8_t raw[512];

	memset(raw, 0, sizeof(raw));

	if (fs_v2) {
		struct fuzix_filesys_v2 sb;
		memset(&sb, 0, sizeof(sb));
		sb.s_mounted = swizzle16(SMOUNTED_V2);
		sb.s_pad0 = 0;
		sb.s_isize = swizzle32(super.isize);
		sb.s_fsize = swizzle32(super.fsize);
		sb.s_nfree = swizzle16(super.nfree);
		sb.s_pad1 = 0;
		for (uint16_t i = 0; i < 50; i++)
			sb.s_free[i] = swizzle32(super.free[i]);
		sb.s_ninode = (int16_t)swizzle16((uint16_t)super.ninode);
		for (uint16_t i = 0; i < 50; i++)
			sb.s_inode[i] = swizzle16(super.inode[i]);
		sb.s_fmod = super.fmod;
		sb.s_timeh = super.timeh;
		sb.s_time = swizzle32(super.time);
		sb.s_tfree = swizzle32(super.tfree);
		sb.s_tinode = swizzle16(super.tinode);
		sb.s_shift = super.shift;
		sb.s_pad2 = 0;
		memcpy(raw, &sb, sizeof(sb));
	} else {
		struct fuzix_filesys_v1 sb;
		memset(&sb, 0, sizeof(sb));
		sb.s_mounted = swizzle16(SMOUNTED);
		sb.s_isize = swizzle16((uint16_t)super.isize);
		sb.s_fsize = swizzle16((uint16_t)super.fsize);
		sb.s_nfree = swizzle16(super.nfree);
		for (uint16_t i = 0; i < 50; i++)
			sb.s_free[i] = swizzle16((uint16_t)super.free[i]);
		sb.s_ninode = (int16_t)swizzle16((uint16_t)super.ninode);
		for (uint16_t i = 0; i < 50; i++)
			sb.s_inode[i] = swizzle16(super.inode[i]);
		sb.s_fmod = super.fmod;
		sb.s_timeh = super.timeh;
		sb.s_time = swizzle32(super.time);
		sb.s_tfree = swizzle16((uint16_t)super.tfree);
		sb.s_tinode = swizzle16(super.tinode);
		sb.s_shift = super.shift;
		memcpy(raw, &sb, sizeof(sb));
	}

	write_block(1, raw);
}

static void inode_read(uint16_t ino, struct fsck_inode *out)
{
	uint8_t raw[512];
	uint16_t off = (uint16_t)((ino & 7U) * 64U);
	fsblk_t blk = (fsblk_t)((ino >> 3) + 2);

	read_block(blk, raw);
	memset(out, 0, sizeof(*out));

	if (fs_v2) {
		struct fuzix_dinode_v2 di;
		memcpy(&di, raw + off, sizeof(di));
		out->mode = swizzle16(di.i_mode);
		out->nlink = swizzle16(di.i_nlink);
		out->uid = swizzle16(di.i_uid);
		out->gid = swizzle16(di.i_gid);
		out->size = swizzle32(di.i_size);
		out->atime = swizzle32(di.i_atime);
		out->mtime = swizzle32(di.i_mtime);
		out->ctime = swizzle32(di.i_ctime);
		for (uint8_t i = 0; i < 10; i++)
			out->addr[i] = swizzle32(di.i_addr[i]);
	} else {
		struct fuzix_dinode_v1 di;
		memcpy(&di, raw + off, sizeof(di));
		out->mode = swizzle16(di.i_mode);
		out->nlink = swizzle16(di.i_nlink);
		out->uid = swizzle16(di.i_uid);
		out->gid = swizzle16(di.i_gid);
		out->size = swizzle32(di.i_size);
		out->atime = swizzle32(di.i_atime);
		out->mtime = swizzle32(di.i_mtime);
		out->ctime = swizzle32(di.i_ctime);
		for (uint8_t i = 0; i < 20; i++)
			out->addr[i] = swizzle16(di.i_addr[i]);
	}
}

static void inode_write(uint16_t ino, const struct fsck_inode *in)
{
	uint8_t raw[512];
	uint16_t off = (uint16_t)((ino & 7U) * 64U);
	fsblk_t blk = (fsblk_t)((ino >> 3) + 2);

	read_block(blk, raw);

	if (fs_v2) {
		struct fuzix_dinode_v2 di;
		memset(&di, 0, sizeof(di));
		di.i_mode = swizzle16(in->mode);
		di.i_nlink = swizzle16(in->nlink);
		di.i_uid = swizzle16(in->uid);
		di.i_gid = swizzle16(in->gid);
		di.i_size = swizzle32(in->size);
		di.i_atime = swizzle32(in->atime);
		di.i_mtime = swizzle32(in->mtime);
		di.i_ctime = swizzle32(in->ctime);
		for (uint8_t i = 0; i < 10; i++)
			di.i_addr[i] = swizzle32(in->addr[i]);
		memcpy(raw + off, &di, sizeof(di));
	} else {
		struct fuzix_dinode_v1 di;
		memset(&di, 0, sizeof(di));
		di.i_mode = swizzle16(in->mode);
		di.i_nlink = swizzle16(in->nlink);
		di.i_uid = swizzle16(in->uid);
		di.i_gid = swizzle16(in->gid);
		di.i_size = swizzle32(in->size);
		di.i_atime = swizzle32(in->atime);
		di.i_mtime = swizzle32(in->mtime);
		di.i_ctime = swizzle32(in->ctime);
		for (uint8_t i = 0; i < 20; i++) {
			if (in->addr[i] > 0xFFFFU)
				fsck_panic("v1 inode contains >16-bit block pointer");
			di.i_addr[i] = swizzle16((uint16_t)in->addr[i]);
		}
		memcpy(raw + off, &di, sizeof(di));
	}

	write_block(blk, raw);
}

static fsblk_t blk_alloc0(void)
{
	fsblk_t newno;

	if (super.nfree == 0 || super.nfree > 50)
		fsck_panic("superblock free list corrupt");

	newno = super.free[--super.nfree];
	if (newno == 0) {
		if (super.tfree != 0)
			fsck_panic("superblock tfree mismatch");
		++super.nfree;
		return 0;
	}

	if (super.nfree == 0) {
		uint8_t raw[512];

		read_block(newno, raw);
		if (fs_v2) {
			struct fuzix_freelist_v2 fl;
			memcpy(&fl, raw, sizeof(fl));
			super.nfree = swizzle16(fl.nfree);
			if (super.nfree > 50)
				fsck_panic("free list block corrupt");
			for (uint16_t i = 0; i < 50; i++)
				super.free[i] = swizzle32(fl.free[i]);
		} else {
			struct fuzix_freelist_v1 fl;
			memcpy(&fl, raw, sizeof(fl));
			super.nfree = swizzle16(fl.nfree);
			if (super.nfree > 50)
				fsck_panic("free list block corrupt");
			for (uint16_t i = 0; i < 50; i++)
				super.free[i] = swizzle16(fl.free[i]);
		}
	}

	if (super.tfree == 0)
		fsck_panic("allocating from empty tfree");
	--super.tfree;

	super_write();
	return newno;
}

static fsblk_t blk_alloc_unique(void)
{
	for (;;) {
		fsblk_t b = blk_alloc0();
		if (b == 0)
			return 0;
		if (!blk_in_range(b))
			continue;
		if (!bitmap_test(b))
			return b;
	}
}

enum scan_mode {
	SCAN_MARK = 0,
	SCAN_DEDUP = 1,
};

static int scan_internal_ptr(uint16_t inum, const char *what, fsblk_t *blk,
                             enum scan_mode mode)
{
	fsblk_t b = *blk;
	int changed = 0;

	if (b == 0)
		return 0;

	if (!blk_in_range(b)) {
		printf("Inode %u %s block out of range, val=%lu. Zap? ", inum, what,
		       (unsigned long)b);
		if (yes()) {
			*blk = 0;
			return 1;
		}
		return 0;
	}

	if (mode == SCAN_DEDUP && bitmap_test(b)) {
		printf("Inode %u %s block %lu multiply allocated. Fix? ", inum, what,
		       (unsigned long)b);
		if (yes()) {
			fsblk_t newb = blk_alloc_unique();
			if (newb == 0) {
				puts("Sorry... No more free blocks.");
				error_flags |= 4;
				return 0;
			}
			copy_block(newb, b);
			*blk = newb;
			b = newb;
			changed = 1;
		}
	}

	bitmap_set(b);
	return changed;
}

static int scan_leaf_ptr(uint16_t inum, const char *what, fsblk_t *blk,
                         enum scan_mode mode)
{
	fsblk_t b = *blk;
	int changed = 0;

	if (b == 0)
		return 0;

	if (!blk_in_range(b)) {
		printf("Inode %u %s out of range, val=%lu. Zap? ", inum, what,
		       (unsigned long)b);
		if (yes()) {
			*blk = 0;
			return 1;
		}
		return 0;
	}

	if (mode == SCAN_DEDUP && bitmap_test(b)) {
		printf("Inode %u %s %lu multiply allocated. Fix? ", inum, what,
		       (unsigned long)b);
		if (yes()) {
			fsblk_t newb = blk_alloc_unique();
			if (newb == 0) {
				puts("Sorry... No more free blocks.");
				error_flags |= 4;
				return 0;
			}
			copy_block(newb, b);
			*blk = newb;
			b = newb;
			changed = 1;
		}
	}

	bitmap_set(b);
	return changed;
}

static int inode_scan_blocks_v1(uint16_t inum, struct fsck_inode *ino,
                                enum scan_mode mode)
{
	uint32_t nblocks = (ino->size + 511U) / 512U;
	int inode_modified = 0;

	for (uint8_t i = 0; i < 18; i++) {
		if (ino->addr[i] == 0)
			continue;
		if (nblocks == 0 || i >= nblocks) {
			printf("Inode %u direct block %u past EOF. Zap? ", inum,
			       (unsigned)i);
			if (yes()) {
				ino->addr[i] = 0;
				inode_modified = 1;
				continue;
			}
		}
		if (scan_leaf_ptr(inum, "direct block", &ino->addr[i], mode))
			inode_modified = 1;
	}

	/* Single indirect (18) */
	{
		uint32_t need = (nblocks > 18U) ? (nblocks - 18U) : 0;
		fsblk_t root = ino->addr[18];
		int root_modified = 0;
		uint8_t root_touched = 0;

		if (root != 0 && need == 0) {
			printf("Inode %u single indirect block past EOF. Zap? ",
			       inum);
			if (yes()) {
				ino->addr[18] = 0;
				inode_modified = 1;
				root = 0;
			} else {
				need = 256U;
			}
		}

		if (root != 0) {
			/* internal pointer block itself */
			if (scan_internal_ptr(inum, "single indirect", &ino->addr[18],
			                      mode))
				inode_modified = 1;
			root = ino->addr[18];
		}

		if (root != 0 && need != 0) {
			uint8_t raw[512];
			read_block(root, raw);
			for (uint16_t idx = 0; idx < 256U && idx < need; idx++) {
				fsblk_t p = blk_get_u16(raw, idx);
				if (p == 0)
					continue;
				if (scan_leaf_ptr(inum, "indirect data block", &p, mode)) {
					blk_set_u16(raw, idx, (uint16_t)p);
					root_modified = 1;
				}
				root_touched = 1;
			}
			if (root_modified && root_touched)
				write_block(root, raw);
		}
	}

	/* Double indirect (19) */
	{
		uint32_t need = 0;
		if (nblocks > (18U + 256U))
			need = nblocks - (18U + 256U);

		fsblk_t root = ino->addr[19];
		int root_modified = 0;

		if (root != 0 && need == 0) {
			printf("Inode %u double indirect block past EOF. Zap? ", inum);
			if (yes()) {
				ino->addr[19] = 0;
				inode_modified = 1;
				root = 0;
			} else {
				need = 256U * 256U;
			}
		}

		if (root != 0) {
			if (scan_internal_ptr(inum, "double indirect", &ino->addr[19],
			                      mode))
				inode_modified = 1;
			root = ino->addr[19];
		}

		if (root != 0 && need != 0) {
			uint8_t raw_root[512];
			read_block(root, raw_root);

			uint32_t need_l1 = (need + 255U) / 256U;
			if (need_l1 > 256U)
				need_l1 = 256U;

			for (uint16_t i1 = 0; i1 < 256U && i1 < need_l1; i1++) {
				fsblk_t l1 = blk_get_u16(raw_root, i1);
				int l1_modified = 0;

				if (l1 == 0)
					continue;

				/* validate/dedup internal pointer */
				{
					fsblk_t tmp = l1;
					if (scan_internal_ptr(inum, "double indirect l1", &tmp,
					                      mode)) {
						if (tmp == 0) {
							blk_set_u16(raw_root, i1, 0);
							root_modified = 1;
							continue;
						}
						blk_set_u16(raw_root, i1, (uint16_t)tmp);
						root_modified = 1;
						l1 = tmp;
					}
				}

				uint8_t raw_l1[512];
				read_block(l1, raw_l1);

				uint32_t group = need - (uint32_t)i1 * 256U;
				if (group > 256U)
					group = 256U;

				for (uint16_t i2 = 0; i2 < 256U && i2 < group; i2++) {
					fsblk_t p = blk_get_u16(raw_l1, i2);
					if (p == 0)
						continue;
					if (scan_leaf_ptr(inum, "double indirect data block", &p,
					                  mode)) {
						blk_set_u16(raw_l1, i2, (uint16_t)p);
						l1_modified = 1;
					}
				}
				if (l1_modified)
					write_block(l1, raw_l1);
			}

			if (root_modified)
				write_block(root, raw_root);
		}
	}

	return inode_modified;
}

static int inode_scan_blocks_v2(uint16_t inum, struct fsck_inode *ino,
                                enum scan_mode mode)
{
	uint32_t nblocks = (ino->size + 511U) / 512U;
	int inode_modified = 0;

	for (uint8_t i = 0; i < 7; i++) {
		if (ino->addr[i] == 0)
			continue;
		if (nblocks == 0 || i >= nblocks) {
			printf("Inode %u direct block %u past EOF. Zap? ", inum,
			       (unsigned)i);
			if (yes()) {
				ino->addr[i] = 0;
				inode_modified = 1;
				continue;
			}
		}
		if (scan_leaf_ptr(inum, "direct block", &ino->addr[i], mode))
			inode_modified = 1;
	}

	/* Single indirect (7): 128 entries */
	{
		uint32_t need = (nblocks > 7U) ? (nblocks - 7U) : 0;
		if (need > 128U)
			need = 128U;
		fsblk_t root = ino->addr[7];
		int root_modified = 0;

		if (root != 0 && (nblocks <= 7U)) {
			printf("Inode %u single indirect block past EOF. Zap? ", inum);
			if (yes()) {
				ino->addr[7] = 0;
				inode_modified = 1;
				root = 0;
			} else {
				need = 128U;
			}
		}

		if (root != 0) {
			if (scan_internal_ptr(inum, "single indirect", &ino->addr[7],
			                      mode))
				inode_modified = 1;
			root = ino->addr[7];
		}

		if (root != 0 && need != 0) {
			uint8_t raw[512];
			read_block(root, raw);
			for (uint16_t idx = 0; idx < 128U && idx < need; idx++) {
				fsblk_t p = blk_get_u32(raw, idx);
				if (p == 0)
					continue;
				if (scan_leaf_ptr(inum, "indirect data block", &p, mode)) {
					blk_set_u32(raw, idx, p);
					root_modified = 1;
				}
			}
			if (root_modified)
				write_block(root, raw);
		}
	}

	/* Double indirect (8): 128 * 128 entries */
	{
		uint32_t need = 0;
		if (nblocks > (7U + 128U))
			need = nblocks - (7U + 128U);
		if (need > 128U * 128U)
			need = 128U * 128U;

		fsblk_t root = ino->addr[8];
		int root_modified = 0;

		if (root != 0 && (nblocks <= (7U + 128U))) {
			printf("Inode %u double indirect block past EOF. Zap? ", inum);
			if (yes()) {
				ino->addr[8] = 0;
				inode_modified = 1;
				root = 0;
			} else {
				need = 128U * 128U;
			}
		}

		if (root != 0) {
			if (scan_internal_ptr(inum, "double indirect", &ino->addr[8],
			                      mode))
				inode_modified = 1;
			root = ino->addr[8];
		}

		if (root != 0 && need != 0) {
			uint8_t raw_root[512];
			read_block(root, raw_root);

			uint32_t need_l1 = (need + 127U) / 128U;
			if (need_l1 > 128U)
				need_l1 = 128U;

			for (uint16_t i1 = 0; i1 < 128U && i1 < need_l1; i1++) {
				fsblk_t l1 = blk_get_u32(raw_root, i1);
				int l1_modified = 0;

				if (l1 == 0)
					continue;

				{
					fsblk_t tmp = l1;
					if (scan_internal_ptr(inum, "double indirect l1", &tmp,
					                      mode)) {
						if (tmp == 0) {
							blk_set_u32(raw_root, i1, 0);
							root_modified = 1;
							continue;
						}
						blk_set_u32(raw_root, i1, tmp);
						root_modified = 1;
						l1 = tmp;
					}
				}

				uint8_t raw_l1[512];
				read_block(l1, raw_l1);

				uint32_t group = need - (uint32_t)i1 * 128U;
				if (group > 128U)
					group = 128U;

				for (uint16_t i2 = 0; i2 < 128U && i2 < group; i2++) {
					fsblk_t p = blk_get_u32(raw_l1, i2);
					if (p == 0)
						continue;
					if (scan_leaf_ptr(inum, "double indirect data block", &p,
					                  mode)) {
						blk_set_u32(raw_l1, i2, p);
						l1_modified = 1;
					}
				}

				if (l1_modified)
					write_block(l1, raw_l1);
			}

			if (root_modified)
				write_block(root, raw_root);
		}
	}

	/* Triple indirect (9): 128 * 128 * 128 entries */
	{
		uint32_t need = 0;
		if (nblocks > (7U + 128U + 128U * 128U))
			need = nblocks - (7U + 128U + 128U * 128U);
		if (need > 128U * 128U * 128U)
			need = 128U * 128U * 128U;

		fsblk_t root = ino->addr[9];
		int root_modified = 0;

		if (root != 0 && (nblocks <= (7U + 128U + 128U * 128U))) {
			printf("Inode %u triple indirect block past EOF. Zap? ", inum);
			if (yes()) {
				ino->addr[9] = 0;
				inode_modified = 1;
				root = 0;
			} else {
				need = 128U * 128U * 128U;
			}
		}

		if (root != 0) {
			if (scan_internal_ptr(inum, "triple indirect", &ino->addr[9],
			                      mode))
				inode_modified = 1;
			root = ino->addr[9];
		}

		if (root != 0 && need != 0) {
			uint8_t raw_root[512];
			read_block(root, raw_root);

			uint32_t per_l2 = 128U * 128U;
			uint32_t need_l2 = (need + (per_l2 - 1U)) / per_l2;
			if (need_l2 > 128U)
				need_l2 = 128U;

			for (uint16_t i2 = 0; i2 < 128U && i2 < need_l2; i2++) {
				fsblk_t l2 = blk_get_u32(raw_root, i2);
				int l2_modified = 0;

				if (l2 == 0)
					continue;

				{
					fsblk_t tmp = l2;
					if (scan_internal_ptr(inum, "triple indirect l2", &tmp,
					                      mode)) {
						if (tmp == 0) {
							blk_set_u32(raw_root, i2, 0);
							root_modified = 1;
							continue;
						}
						blk_set_u32(raw_root, i2, tmp);
						root_modified = 1;
						l2 = tmp;
					}
				}

				uint8_t raw_l2[512];
				read_block(l2, raw_l2);

				uint32_t group2 = need - (uint32_t)i2 * per_l2;
				if (group2 > per_l2)
					group2 = per_l2;

				uint32_t need_l1 = (group2 + 127U) / 128U;
				if (need_l1 > 128U)
					need_l1 = 128U;

				for (uint16_t i1 = 0; i1 < 128U && i1 < need_l1; i1++) {
					fsblk_t l1 = blk_get_u32(raw_l2, i1);
					int l1_modified = 0;

					if (l1 == 0)
						continue;

					{
						fsblk_t tmp = l1;
						if (scan_internal_ptr(inum, "triple indirect l1", &tmp,
						                      mode)) {
							if (tmp == 0) {
								blk_set_u32(raw_l2, i1, 0);
								l2_modified = 1;
								continue;
							}
							blk_set_u32(raw_l2, i1, tmp);
							l2_modified = 1;
							l1 = tmp;
						}
					}

					uint8_t raw_l1[512];
					read_block(l1, raw_l1);

					uint32_t group1 = group2 - (uint32_t)i1 * 128U;
					if (group1 > 128U)
						group1 = 128U;

					for (uint16_t idx = 0; idx < 128U && idx < group1; idx++) {
						fsblk_t p = blk_get_u32(raw_l1, idx);
						if (p == 0)
							continue;
						if (scan_leaf_ptr(inum, "triple indirect data block", &p,
						                  mode)) {
							blk_set_u32(raw_l1, idx, p);
							l1_modified = 1;
						}
					}

					if (l1_modified)
						write_block(l1, raw_l1);
				}

				if (l2_modified)
					write_block(l2, raw_l2);
			}

			if (root_modified)
				write_block(root, raw_root);
		}
	}

	return inode_modified;
}

static int inode_scan_blocks(uint16_t inum, struct fsck_inode *ino,
                             enum scan_mode mode)
{
	if (fs_v2)
		return inode_scan_blocks_v2(inum, ino, mode);
	return inode_scan_blocks_v1(inum, ino, mode);
}

static fsblk_t inode_getblk_v1(const struct fsck_inode *ino, uint32_t lbn)
{
	if (lbn < 18U)
		return ino->addr[lbn];
	lbn -= 18U;

	if (lbn < 256U) {
		fsblk_t ind = ino->addr[18];
		uint8_t raw[512];
		if (ind == 0)
			return 0;
		read_block(ind, raw);
		return blk_get_u16(raw, (uint16_t)lbn);
	}

	lbn -= 256U;

	{
		fsblk_t dind = ino->addr[19];
		uint8_t raw1[512], raw2[512];
		fsblk_t ind;
		uint16_t i1 = (uint16_t)(lbn >> 8);
		uint16_t i2 = (uint16_t)(lbn & 0xFF);

		if (dind == 0)
			return 0;
		read_block(dind, raw1);
		ind = blk_get_u16(raw1, i1);
		if (ind == 0)
			return 0;
		read_block(ind, raw2);
		return blk_get_u16(raw2, i2);
	}
}

static fsblk_t inode_getblk_v2(const struct fsck_inode *ino, uint32_t lbn)
{
	if (lbn < 7U)
		return ino->addr[lbn];
	lbn -= 7U;

	if (lbn < 128U) {
		fsblk_t ind = ino->addr[7];
		uint8_t raw[512];
		if (ind == 0)
			return 0;
		read_block(ind, raw);
		return blk_get_u32(raw, (uint16_t)lbn);
	}

	lbn -= 128U;

	if (lbn < 128U * 128U) {
		fsblk_t dind = ino->addr[8];
		uint8_t raw1[512], raw2[512];
		uint16_t i1 = (uint16_t)(lbn / 128U);
		uint16_t i2 = (uint16_t)(lbn % 128U);
		fsblk_t ind;

		if (dind == 0)
			return 0;
		read_block(dind, raw1);
		ind = blk_get_u32(raw1, i1);
		if (ind == 0)
			return 0;
		read_block(ind, raw2);
		return blk_get_u32(raw2, i2);
	}

	lbn -= 128U * 128U;

	if (lbn < 128U * 128U * 128U) {
		fsblk_t tind = ino->addr[9];
		uint32_t per_l2 = 128U * 128U;
		uint16_t i1 = (uint16_t)(lbn / per_l2);
		uint32_t rem = lbn % per_l2;
		uint16_t i2 = (uint16_t)(rem / 128U);
		uint16_t i3 = (uint16_t)(rem % 128U);
		uint8_t raw1[512], raw2[512], raw3[512];
		fsblk_t l2, l1;

		if (tind == 0)
			return 0;
		read_block(tind, raw1);
		l2 = blk_get_u32(raw1, i1);
		if (l2 == 0)
			return 0;
		read_block(l2, raw2);
		l1 = blk_get_u32(raw2, i2);
		if (l1 == 0)
			return 0;
		read_block(l1, raw3);
		return blk_get_u32(raw3, i3);
	}

	return 0;
}

static fsblk_t inode_getblk(const struct fsck_inode *ino, uint32_t lbn)
{
	if (fs_v2)
		return inode_getblk_v2(ino, lbn);
	return inode_getblk_v1(ino, lbn);
}

static void dir_read(const struct fsck_inode *ino, uint32_t idx, direct *d)
{
	fsblk_t blk = inode_getblk(ino, idx / 16U);
	uint8_t raw[512];

	if (blk == 0)
		fsck_panic("missing block in directory");
	read_block(blk, raw);
	memcpy(d, raw + (32U * (idx % 16U)), 32);
}

static void dir_write(const struct fsck_inode *ino, uint32_t idx, const direct *d)
{
	fsblk_t blk = inode_getblk(ino, idx / 16U);
	uint8_t raw[512];

	if (blk == 0)
		fsck_panic("missing block in directory");
	read_block(blk, raw);
	memcpy(raw + (32U * (idx % 16U)), d, 32);
	write_block(blk, raw);
}

static void mkentry(uint16_t inum);

static int depth;

static void ckdir(uint16_t inum, uint16_t pnum, const char *name)
{
	struct fsck_inode ino;
	direct dentry;
	uint32_t nentries;

	inode_read(inum, &ino);
	if ((ino.mode & F_MASK) != F_DIR)
		return;

	++depth;
	if (depth > MAXDEPTH) {
		--depth;
		return;
	}

	if ((ino.size % 32U) != 0) {
		printf("Directory inode %u has improper length. Fix? ", inum);
		if (yes()) {
			ino.size &= ~31U;
			inode_write(inum, &ino);
		}
	}

	nentries = ino.size / 32U;

	for (uint32_t j = 0; j < nentries; j++) {
		dir_read(&ino, j, &dentry);

		/* Ensure trailing bytes after NUL are cleared. */
		{
			uint32_t k;
			int dirty = 0;
			for (k = 0; k < 30U; k++) {
				if (dentry.d_name[k] == '\0')
					break;
			}
			for (; k < 30U; k++) {
				if (dentry.d_name[k] != '\0') {
					dentry.d_name[k] = '\0';
					dirty = 1;
				}
			}
			if (dirty)
				dir_write(&ino, j, &dentry);
		}

		if (dentry.d_ino == 0)
			continue;

		{
			uint16_t target = swizzle16(dentry.d_ino);

			if (target < ROOTINODE || target >= inode_count) {
				printf("Directory entry %s%-1.30s has out-of-range inode %u. Zap? ",
				       name, dentry.d_name, target);
				if (yes()) {
					dentry.d_ino = 0;
					dentry.d_name[0] = '\0';
					dir_write(&ino, j, &dentry);
					continue;
				}
			}

			if (dentry.d_ino && linkmap[target] == -1) {
				printf("Directory entry %s%-1.30s points to bogus inode %u. Zap? ",
				       name, dentry.d_name, target);
				if (yes()) {
					dentry.d_ino = 0;
					dentry.d_name[0] = '\0';
					dir_write(&ino, j, &dentry);
					continue;
				}
			}

			++linkmap[target];

			for (uint8_t c = 0; c < 30U && dentry.d_name[c]; c++) {
				if (dentry.d_name[c] == '/') {
					printf("Directory entry %s%-1.30s contains slash. Fix? ",
					       name, dentry.d_name);
					if (yes()) {
						dentry.d_name[c] = 'X';
						dir_write(&ino, j, &dentry);
					}
				}
			}

			if (strncmp(dentry.d_name, ".", 30) == 0 && target != inum) {
				printf("Dot entry %s%-1.30s points to wrong place. Fix? ",
				       name, dentry.d_name);
				if (yes()) {
					dentry.d_ino = swizzle16(inum);
					dir_write(&ino, j, &dentry);
				}
			}
			if (strncmp(dentry.d_name, "..", 30) == 0 && target != pnum) {
				printf("DotDot entry %s%-1.30s points to wrong place. Fix? ",
				       name, dentry.d_name);
				if (yes()) {
					dentry.d_ino = swizzle16(pnum);
					dir_write(&ino, j, &dentry);
				}
			}

			if (target != pnum && target != inum && depth < MAXDEPTH) {
				size_t newlen = strlen(name) + strlen(dentry.d_name) + 2;
				char *ename = malloc(newlen);
				if (!ename) {
					fprintf(stderr, "Not enough memory.\n");
					exit(error_flags | 8);
				}
				strcpy(ename, name);
				strcat(ename, dentry.d_name);
				strcat(ename, "/");
				ckdir(target, inum, ename);
				free(ename);
			}
		}
	}

	--depth;
}

static void mkentry(uint16_t inum)
{
	struct fsck_inode rootino;
	direct dentry;

	inode_read(ROOTINODE, &rootino);
	for (uint32_t d = 0; d < (rootino.size / 32U); d++) {
		dir_read(&rootino, d, &dentry);
		if (dentry.d_ino == 0 && dentry.d_name[0] == '\0') {
			dentry.d_ino = swizzle16(inum);
			snprintf(dentry.d_name, sizeof(dentry.d_name), "l+f%u", inum);
			dir_write(&rootino, d, &dentry);
			return;
		}
	}
	puts("Sorry... No empty slots in root directory.");
	error_flags |= 4;
}

static void pass1(void)
{
	uint32_t icount = 0;

	memset(linkmap, 0xFF, inode_count * sizeof(*linkmap));

	for (uint16_t n = ROOTINODE; n < inode_count; n++) {
		struct fsck_inode ino;
		uint16_t mode;

		inode_read(n, &ino);

		if (ino.mode == 0)
			continue;

		mode = ino.mode & F_MASK;
		if (mode != F_REG && mode != F_DIR && mode != F_BDEV && mode != F_CDEV) {
			printf("Inode %u with mode 0%o is not of correct type. Zap? ",
			       n, ino.mode);
			if (yes()) {
				ino.mode = 0;
				ino.nlink = 0;
				inode_write(n, &ino);
				continue;
			}
		}

		linkmap[n] = 0;
		++icount;

		if (mode == F_REG || mode == F_DIR) {
			int modified = inode_scan_blocks(n, &ino, SCAN_MARK);
			if (modified)
				inode_write(n, &ino);
		}
	}

	{
		uint32_t expected = inode_count - ROOTINODE - icount;
		if (expected > 0xFFFFU)
			expected = 0xFFFFU;
		if (super.tinode != (uint16_t)expected) {
			printf("Free inode count in superblock is %u, should be %u. Fix? ",
			       super.tinode, (unsigned)expected);
			if (yes()) {
				super.tinode = (uint16_t)expected;
				super_write();
			}
		}
	}
}

static void pass2(void)
{
	uint32_t oldtfree = super.tfree;

	printf("Rebuild free list? ");
	if (!yes_noerror())
		return;

	error_flags |= 1;

	super.ninode = 0;
	super.nfree = 1;
	memset(super.free, 0, sizeof(super.free));
	super.free[0] = 0;
	super.tfree = 0;

	for (fsblk_t j = super.fsize; j-- > super.isize;) {
		if (bitmap_test(j))
			continue;

		if (super.nfree == 50) {
			uint8_t raw[512];
			memset(raw, 0, sizeof(raw));
			if (fs_v2) {
				struct fuzix_freelist_v2 fl;
				memset(&fl, 0, sizeof(fl));
				fl.nfree = swizzle16(super.nfree);
				fl.pad = 0;
				for (uint16_t i = 0; i < 50; i++)
					fl.free[i] = swizzle32(super.free[i]);
				memcpy(raw, &fl, sizeof(fl));
			} else {
				struct fuzix_freelist_v1 fl;
				memset(&fl, 0, sizeof(fl));
				fl.nfree = swizzle16(super.nfree);
				for (uint16_t i = 0; i < 50; i++)
					fl.free[i] = swizzle16((uint16_t)super.free[i]);
				memcpy(raw, &fl, sizeof(fl));
			}
			write_block(j, raw);
			super.nfree = 0;
		}

		++super.tfree;
		super.free[super.nfree++] = j;
	}

	super_write();

	if (oldtfree != super.tfree) {
		printf("During free list regeneration s_tfree was changed to %lu from %lu.\n",
		       (unsigned long)super.tfree, (unsigned long)oldtfree);
	}
}

static void pass3(void)
{
	memset(bitmap, 0, (size_t)((super.fsize + 7U) / 8U));

	for (uint16_t n = ROOTINODE; n < inode_count; n++) {
		struct fsck_inode ino;
		uint16_t mode;

		inode_read(n, &ino);
		mode = ino.mode & F_MASK;
		if (mode != F_REG && mode != F_DIR)
			continue;

		if (inode_scan_blocks(n, &ino, SCAN_DEDUP))
			inode_write(n, &ino);
	}
}

static void pass4(void)
{
	depth = 0;
	linkmap[ROOTINODE] = 1;
	ckdir(ROOTINODE, ROOTINODE, "/");
	if (depth != 0)
		fsck_panic("inconsistent depth");
}

static void pass5(void)
{
	for (uint16_t n = ROOTINODE; n < inode_count; n++) {
		struct fsck_inode ino;

		inode_read(n, &ino);

		if (ino.mode == 0) {
			if (linkmap[n] != -1)
				fsck_panic("inconsistent linkmap");
			continue;
		}

		if (linkmap[n] == -1)
			fsck_panic("inconsistent linkmap");

		if (linkmap[n] > 0 && ino.nlink != (uint16_t)linkmap[n]) {
			printf("Inode %u has link count %u should be %u. Fix? ", n,
			       ino.nlink, (unsigned)linkmap[n]);
			if (yes()) {
				ino.nlink = (uint16_t)linkmap[n];
				inode_write(n, &ino);
			}
		}

		if (linkmap[n] == 0) {
			uint16_t mode = ino.mode & F_MASK;
			if (mode == F_BDEV || mode == F_CDEV || ino.size == 0) {
				printf("Useless inode %u with mode 0%o has become detached. Link count is %u. Zap? ",
				       n, ino.mode, ino.nlink);
				if (yes()) {
					ino.nlink = 0;
					ino.mode = 0;
					inode_write(n, &ino);
					if (super.tinode != 0xFFFFU)
						++super.tinode;
					super_write();
				}
			} else {
				printf("Inode %u has become detached. Link count is %u. ", n,
				       ino.nlink);
				if (ino.nlink == 0)
					printf("Zap? ");
				else
					printf("Fix? ");
				if (yes()) {
					if (ino.nlink == 0) {
						ino.nlink = 0;
						ino.mode = 0;
						inode_write(n, &ino);
						if (super.tinode != 0xFFFFU)
							++super.tinode;
						super_write();
					} else {
						ino.nlink = 1;
						inode_write(n, &ino);
						mkentry(n);
					}
				}
			}
		}
	}
}

int main(int argc, char **argv)
{
	while (argc > 1 && argv[1][0] == '-') {
		if (strcmp(argv[1], "-a") == 0) {
			aflag = 1;
		} else if (strcmp(argv[1], "-y") == 0) {
			yflag = 1;
		} else {
			fprintf(stderr, "Bad option: %s\n", argv[1]);
			return 16;
		}
		argc--;
		argv++;
	}

	if (argc != 2) {
		fprintf(stderr, "syntax: fsck [-a] [-y] [devfile][:offset]\n");
		return 16;
	}

	if (fd_open(argv[1], 0) < 0) {
		puts("Cannot open file");
		return 16;
	}

	super_read();

	if (super.fmod == FMOD_DIRTY) {
		puts("Filesystem was not cleanly unmounted.");
		error_flags |= 1;
	} else if (aflag) {
		bdclose();
		return 0;
	}

	printf("Device has fsize = %lu and isize = %lu. Continue? ",
	       (unsigned long)super.fsize, (unsigned long)super.isize);
	if (!yes_noerror())
		exit(error_flags | 32);

	{
		size_t bitmap_bytes = (size_t)((super.fsize + 7U) / 8U);
		bitmap = calloc(bitmap_bytes, 1);
		linkmap = calloc(inode_count ? inode_count : 1, sizeof(*linkmap));
		if (!bitmap || !linkmap) {
			fprintf(stderr, "Not enough memory.\n");
			exit(error_flags | 8);
		}
		printf("Memory pool %lu bytes\n",
		       (unsigned long)(bitmap_bytes + inode_count * sizeof(*linkmap)));
	}

	puts("Pass 1: Checking inodes...");
	pass1();

	puts("Pass 2: Rebuilding free list...");
	pass2();

	puts("Pass 3: Checking block allocation...");
	pass3();

	puts("Pass 4: Checking directory entries...");
	pass4();

	puts("Pass 5: Checking link counts...");
	pass5();

	if ((error_flags & 5) == 1) {
		super.fmod = FMOD_CLEAN;
		super_write();
	}

	bdclose();
	puts("Done.");
	return error_flags;
}
