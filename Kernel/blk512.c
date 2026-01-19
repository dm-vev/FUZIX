#include "kernel.h"
#include "kdata.h"
#include "printf.h"

#if (BLKSIZE == 512)

/*
 *	File system routines for the usual 512 byte block size
 */

/* Return the number of blocks an inode occupies assuming all blocks present */
blkno_t inode_blocks(inoptr i)
{
    return (i->c_node.i_size + BLKMASK) >> BLKSHIFT;
}

/* Read an inode */
uint_fast8_t breadi(uint16_t dev, uint16_t ino, void *ptr)
{
    struct blkbuf *buf = bread(dev, (ino >> 3) + 2, 0);
    if (buf == NULL)
        return 1;
#ifdef CONFIG_LARGEFS
    {
        struct mount *mnt = fs_tab_get(dev);
        struct dinode *out = ptr;
        uint16_t off = sizeof(struct fuzix_dinode_v1) * (ino & 7);

        if (mnt && mnt->m_fs.s_mounted == SMOUNTED_V2) {
            struct fuzix_dinode_v2 di;
            blktok(&di, buf, off, sizeof(di));
            out->i_mode = di.i_mode;
            out->i_nlink = di.i_nlink;
            out->i_uid = di.i_uid;
            out->i_gid = di.i_gid;
            out->i_size = di.i_size;
            out->i_atime = di.i_atime;
            out->i_mtime = di.i_mtime;
            out->i_ctime = di.i_ctime;
            for (uint_fast8_t i = 0; i < 10; i++)
                out->i_addr[i] = di.i_addr[i];
            for (uint_fast8_t i = 10; i < 20; i++)
                out->i_addr[i] = 0;
        } else {
            struct fuzix_dinode_v1 di;
            blktok(&di, buf, off, sizeof(di));
            out->i_mode = di.i_mode;
            out->i_nlink = di.i_nlink;
            out->i_uid = di.i_uid;
            out->i_gid = di.i_gid;
            out->i_size = di.i_size;
            out->i_atime = di.i_atime;
            out->i_mtime = di.i_mtime;
            out->i_ctime = di.i_ctime;
            for (uint_fast8_t i = 0; i < 20; i++)
                out->i_addr[i] = di.i_addr[i];
        }
    }
#else
    blktok(ptr, buf, sizeof(struct dinode) * (ino & 7), sizeof(struct dinode));
#endif
    brelse(buf);
    return 0;
}

/* Write an inode */
uint_fast8_t bwritei(inoptr ino)
{
    blkno_t blkno = (ino->c_num >> 3) + 2;
    struct blkbuf *buf = bread(ino->c_dev, blkno, 0);
    if (buf == NULL)
        return 1;
#ifdef CONFIG_LARGEFS
    {
        struct mount *mnt = fs_tab_get(ino->c_dev);
        uint16_t off = sizeof(struct fuzix_dinode_v1) * (ino->c_num & 0x07);

        if (mnt && mnt->m_fs.s_mounted == SMOUNTED_V2) {
            struct fuzix_dinode_v2 di;
            memset(&di, 0, sizeof(di));
            di.i_mode = ino->c_node.i_mode;
            di.i_nlink = ino->c_node.i_nlink;
            di.i_uid = ino->c_node.i_uid;
            di.i_gid = ino->c_node.i_gid;
            di.i_size = ino->c_node.i_size;
            di.i_atime = ino->c_node.i_atime;
            di.i_mtime = ino->c_node.i_mtime;
            di.i_ctime = ino->c_node.i_ctime;
            for (uint_fast8_t i = 0; i < 10; i++)
                di.i_addr[i] = ino->c_node.i_addr[i];
            blkfromk(&di, buf, off, sizeof(di));
        } else {
            struct fuzix_dinode_v1 di;
            memset(&di, 0, sizeof(di));
            di.i_mode = ino->c_node.i_mode;
            di.i_nlink = ino->c_node.i_nlink;
            di.i_uid = ino->c_node.i_uid;
            di.i_gid = ino->c_node.i_gid;
            di.i_size = ino->c_node.i_size;
            di.i_atime = ino->c_node.i_atime;
            di.i_mtime = ino->c_node.i_mtime;
            di.i_ctime = ino->c_node.i_ctime;
            for (uint_fast8_t i = 0; i < 20; i++)
                di.i_addr[i] = (uint16_t)ino->c_node.i_addr[i];
            blkfromk(&di, buf, off, sizeof(di));
        }
    }
#else
    blkfromk(&ino->c_node, buf, sizeof(struct dinode) * (ino->c_num & 0x07),
            sizeof(struct dinode));
#endif
    bfree(buf, 2);
    return 0;
}

#ifdef CONFIG_LARGEFS
/* Indirect-block entry access helpers (avoid alignment assumptions). */
static uint16_t indir_get16(bufptr bp, uint16_t idx)
{
    uint16_t v;
    blktok(&v, bp, idx * sizeof(uint16_t), sizeof(v));
    return v;
}

static void indir_set16(bufptr bp, uint16_t idx, uint16_t v)
{
    blkfromk(&v, bp, idx * sizeof(uint16_t), sizeof(v));
}

static uint32_t indir_get32(bufptr bp, uint16_t idx)
{
    uint32_t v;
    blktok(&v, bp, idx * sizeof(uint32_t), sizeof(v));
    return v;
}

static void indir_set32(bufptr bp, uint16_t idx, uint32_t v)
{
    blkfromk(&v, bp, idx * sizeof(uint32_t), sizeof(v));
}
#endif

/*
 * Bmap defines the structure of file system storage by returning
 * the physical block number on a device given the inode and the
 * logical block number in a file.  The block is zeroed if created.
 */
blkno_t bmap(inoptr ip, blkno_t bn, unsigned int rwflg)
{
#ifdef CONFIG_LARGEFS
    uint16_t dev;
    struct mount *mnt;

    if (getmode(ip) == MODE_R(F_BDEV))
        return (bn);

    dev = ip->c_dev;
    mnt = &fs_tab[ip->c_super];

    /* v2 (largefs): 7 direct + single + double + triple indirect (32-bit ptrs). */
    if (mnt->m_fs.s_mounted == SMOUNTED_V2) {
        const uint32_t ndirect = 7;
        const uint32_t nindir = BLKSIZE / sizeof(uint32_t); /* 128 */
        const uint32_t nindir2 = nindir * nindir; /* 16384 */
        const uint32_t nindir3 = nindir2 * nindir; /* 2097152 */

        if (bn < ndirect) {
            blkno_t nb = ip->c_node.i_addr[bn];
            if (nb == 0) {
                if (rwflg || (nb = blk_alloc(dev)) == 0)
                    return NULLBLK;
                ip->c_node.i_addr[bn] = nb;
                ip->c_flags |= CDIRTY;
            }
            return nb;
        }

        bn -= ndirect;

        /* Pick indirection level and the root pointer index. */
        uint8_t level;
        uint32_t index;
        uint8_t root;

        if (bn < nindir) {
            level = 1;
            index = bn;
            root = 7;
        } else if ((bn -= nindir) < nindir2) {
            level = 2;
            index = bn;
            root = 8;
        } else if ((bn -= nindir2) < nindir3) {
            level = 3;
            index = bn;
            root = 9;
        } else {
            udata.u_error = EFBIG;
            return NULLBLK;
        }

        blkno_t nb = ip->c_node.i_addr[root];
        if (nb == 0) {
            if (rwflg || (nb = blk_alloc(dev)) == 0)
                return NULLBLK;
            ip->c_node.i_addr[root] = nb;
            ip->c_flags |= CDIRTY;
        }

        for (uint8_t step = level; step > 0; step--) {
            bufptr bp = bread(dev, nb, 0);
            if (bp == NULL) {
                corrupt_fs(ip->c_dev);
                return NULLBLK;
            }

            uint16_t idx;
            if (step == 1) {
                idx = (uint16_t)index;
            } else if (step == 2) {
                idx = (uint16_t)(index / nindir);
                index = index % nindir;
            } else { /* step == 3 */
                idx = (uint16_t)(index / nindir2);
                index = index % nindir2;
            }

            uint32_t next = indir_get32(bp, idx);
            if (next) {
                brelse(bp);
                nb = next;
                continue;
            }

            if (rwflg || (next = blk_alloc(dev)) == 0) {
                brelse(bp);
                return NULLBLK;
            }
            indir_set32(bp, idx, next);
            bawrite(bp);
            nb = next;
        }
        return nb;
    }

    /* v1 (legacy): 18 direct + single + double indirect (16-bit ptrs). */
    if (bn < 18) {
        blkno_t nb = ip->c_node.i_addr[bn];
        if (nb == 0) {
            if (rwflg || (nb = blk_alloc(dev)) == 0)
                return NULLBLK;
            ip->c_node.i_addr[bn] = nb;
            ip->c_flags |= CDIRTY;
        }
        return nb;
    }

    bn -= 18;

    uint8_t level;
    uint32_t index;
    uint8_t root;

    if (bn < 256) {
        level = 1;
        index = bn;
        root = 18;
    } else if ((bn -= 256) < 256U * 256U) {
        level = 2;
        index = bn;
        root = 19;
    } else {
        udata.u_error = EFBIG;
        return NULLBLK;
    }

    blkno_t nb = ip->c_node.i_addr[root];
    if (nb == 0) {
        if (rwflg || (nb = blk_alloc(dev)) == 0)
            return NULLBLK;
        ip->c_node.i_addr[root] = nb;
        ip->c_flags |= CDIRTY;
    }

    for (uint8_t step = level; step > 0; step--) {
        bufptr bp = bread(dev, nb, 0);
        if (bp == NULL) {
            corrupt_fs(ip->c_dev);
            return NULLBLK;
        }

        uint16_t idx = (step == 1) ? (uint16_t)index : (uint16_t)(index >> 8);

        uint16_t next16 = indir_get16(bp, idx);
        if (next16) {
            brelse(bp);
            nb = next16;
            if (step == 2)
                index &= 0xFF;
            continue;
        }

        if (rwflg) {
            brelse(bp);
            return NULLBLK;
        }

        blkno_t alloc = blk_alloc(dev);
        if (alloc == 0) {
            brelse(bp);
            return NULLBLK;
        }
        indir_set16(bp, idx, (uint16_t)alloc);
        bawrite(bp);
        nb = alloc;
        if (step == 2)
            index &= 0xFF;
    }
    return nb;
#else
    int i;
    bufptr bp;
    int j;
    blkno_t nb;
    int sh;
    uint16_t dev;

    if(getmode(ip) == MODE_R(F_BDEV))
        return(bn);

    dev = ip->c_dev;

    /* blocks 0..17 are direct blocks
    */
    if(bn < 18) {
        nb = ip->c_node.i_addr[bn];
        if(nb == 0) {
            if(rwflg ||(nb = blk_alloc(dev))==0)
                return(NULLBLK);
            ip->c_node.i_addr[bn] = nb;
            ip->c_flags |= CDIRTY;
        }
        return(nb);
    }

    /* addresses 18 and 19 have single and double indirect blocks.
     * the first step is to determine how many levels of indirection.
     */
    bn -= 18;
    sh = 0;
    j = 2;
    if(bn & 0xff00){       /* bn > 255  so double indirect */
        sh = 8;
        bn -= 256;
        j = 1;
    }

    /* fetch the address from the inode
     * Create the first indirect block if needed.
     */
    if(!(nb = ip->c_node.i_addr[20-j]))
    {
        if(rwflg || !(nb = blk_alloc(dev)))
            return(NULLBLK);
        ip->c_node.i_addr[20-j] = nb;
        ip->c_flags |= CDIRTY;
    }

    /* fetch through the indirect blocks
    */
    for(; j<=2; j++) {
        bp = bread(dev, nb, 0);
        if (bp == NULL) {
            corrupt_fs(ip->c_dev);
            return 0;
        }
        i = (bn >> sh) & 0xff;
        nb = *(blkno_t *)blkptr(bp, (sizeof(blkno_t)) * i, sizeof(blkno_t));
        if (nb)
            brelse(bp);
        else
        {
            if(rwflg || !(nb = blk_alloc(dev))) {
                brelse(bp);
                return(NULLBLK);
            }
            blkfromk(&nb, bp, i * sizeof(blkno_t), sizeof(blkno_t));
            bawrite(bp);
        }
        sh -= 8;
    }
    return(nb);
#endif
}

#endif
