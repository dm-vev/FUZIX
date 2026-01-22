#include <kernel.h>
#include <kdata.h>
#include <printf.h>

#ifdef CONFIG_FATFS

static uint16_t fat_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t fat_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool fat_is_pow2_u8(uint8_t v)
{
    return v && ((v & (v - 1)) == 0);
}

int fat_mount(struct mount *m, uint16_t dev, uint16_t flags)
{
    bufptr buf;
    const uint8_t *b;
    uint16_t sig;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors16;
    uint32_t total_sectors32;
    uint16_t fat_size16;
    uint32_t fat_size32;
    uint32_t total_sectors;
    uint32_t fat_sectors;
    uint32_t root_dir_sectors;
    uint32_t first_data_sector;
    uint32_t data_sectors;
    uint32_t cluster_count;
    uint8_t fat_type;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;

    buf = bread(dev, 0, 0);
    if (buf == NULL)
        return -1;

    b = blkptr(buf, 0, BLKSIZE);
    sig = fat_le16(b + 510);
    if (sig != 0xAA55) {
        udata.u_error = EINVAL;
        brelse(buf);
        return -1;
    }

    bytes_per_sector = fat_le16(b + 11);
    sectors_per_cluster = b[13];
    reserved_sectors = fat_le16(b + 14);
    fat_count = b[16];
    root_entry_count = fat_le16(b + 17);
    total_sectors16 = fat_le16(b + 19);
    total_sectors32 = fat_le32(b + 32);
    fat_size16 = fat_le16(b + 22);
    fat_size32 = fat_le32(b + 36);

    total_sectors = total_sectors16 ? (uint32_t)total_sectors16 : total_sectors32;
    fat_sectors = fat_size16 ? (uint32_t)fat_size16 : fat_size32;

    if (bytes_per_sector != BLKSIZE ||
        !fat_is_pow2_u8(sectors_per_cluster) ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        total_sectors == 0 ||
        fat_sectors == 0) {
        udata.u_error = EINVAL;
        brelse(buf);
        return -1;
    }

    root_dir_sectors =
        ((uint32_t)root_entry_count * 32U + (uint32_t)bytes_per_sector - 1U) /
        (uint32_t)bytes_per_sector;

    first_data_sector =
        (uint32_t)reserved_sectors + (uint32_t)fat_count * fat_sectors + root_dir_sectors;

    if (first_data_sector >= total_sectors) {
        udata.u_error = EINVAL;
        brelse(buf);
        return -1;
    }

    data_sectors = total_sectors - first_data_sector;
    cluster_count = data_sectors / (uint32_t)sectors_per_cluster;

    if (cluster_count < 4085U) {
        udata.u_error = EINVAL;
        brelse(buf);
        return -1; /* FAT12 not supported */
    } else if (cluster_count < 65525U) {
        fat_type = 16;
    } else {
        fat_type = 32;
    }

    memset(&m->m_fat, 0, sizeof(m->m_fat));
    m->m_fstype = FSTYPE_FAT;
    m->m_fat.fat_type = fat_type;
    m->m_fat.bytes_per_sector = bytes_per_sector;
    m->m_fat.sectors_per_cluster = sectors_per_cluster;
    m->m_fat.reserved_sectors = reserved_sectors;
    m->m_fat.fat_count = fat_count;
    m->m_fat.total_sectors = total_sectors;
    m->m_fat.fat_start = (uint32_t)reserved_sectors;
    m->m_fat.fat_sectors = fat_sectors;
    m->m_fat.data_start = first_data_sector;
    m->m_fat.cluster_count = cluster_count;
    m->m_fat.max_cluster = cluster_count + 1U;
    m->m_fat.alloc_hint = 2;

    if (fat_type == 32) {
        root_cluster = fat_le32(b + 44);
        fsinfo_sector = fat_le16(b + 48);
        if (root_cluster < 2 || root_cluster > m->m_fat.max_cluster) {
            udata.u_error = EINVAL;
            brelse(buf);
            return -1;
        }
        m->m_fat.root_cluster = root_cluster;
        m->m_fat.fsinfo_sector = fsinfo_sector;
        m->m_fat.fsinfo_valid = 0;
    } else {
        /* FAT16 root directory is a fixed region after the FATs. */
        m->m_fat.root_dir_start = (uint32_t)reserved_sectors + (uint32_t)fat_count * fat_sectors;
        m->m_fat.root_dir_sectors = root_dir_sectors;
    }

    /*
     * Keep the generic FUZIX superblock fields in the "clean/no-op" state so
     * sync()/umount() won't try to write an unrelated block-1 superblock.
     */
    m->m_fs.s_mounted = 0;
    m->m_fs.s_fmod = FMOD_CLEAN;

    /* Honor a dirty/unclean medium by forcing r/o is a FUZIX concept; FAT has
       no equivalent. Caller mount flags control read-only behaviour. */
    used(flags);

    brelse(buf);
    return 0;
}

inoptr fat_iroot(struct mount *m)
{
    inoptr free_slot = NULLINODE;
    inoptr j;

    for (j = i_tab; j < i_tab + ITABSIZE; j++) {
        if (!j->c_refs) {
            if (free_slot == NULLINODE)
                free_slot = j;
            continue;
        }
        if (j->c_dev == m->m_dev && (j->c_fat.flags & FAT_I_ROOT)) {
            j->c_refs++;
            return j;
        }
    }

    if (free_slot == NULLINODE) {
        udata.u_error = ENFILE;
        return NULLINODE;
    }

    memset(&free_slot->c_node, 0, sizeof(free_slot->c_node));
    memset(&free_slot->c_fat, 0, sizeof(free_slot->c_fat));

    free_slot->c_dev = m->m_dev;
    free_slot->c_num = ROOTINODE;
    free_slot->c_super = (uint8_t)(m - fs_tab);
    free_slot->c_magic = CMAGIC;
    free_slot->c_flags = (m->m_flags & MS_RDONLY) ? CRDONLY : 0;
    free_slot->c_refs = 1;

    free_slot->c_node.i_mode = F_DIR | 0777;
    free_slot->c_node.i_nlink = 2;
    free_slot->c_node.i_uid = 0;
    free_slot->c_node.i_gid = 0;
    free_slot->c_node.i_size = 0;

    free_slot->c_fat.flags = FAT_I_ROOT;
    if (m->m_fat.fat_type == 32) {
        free_slot->c_fat.start_cluster = m->m_fat.root_cluster;
        free_slot->c_fat.parent_cluster = m->m_fat.root_cluster;
    } else {
        free_slot->c_fat.start_cluster = 0;
        free_slot->c_fat.parent_cluster = 0;
    }

    return free_slot;
}

#endif
