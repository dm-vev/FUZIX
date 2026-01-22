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

static void fat_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void fat_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
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

static bool fat_is_eoc(struct mount *m, uint32_t v)
{
    if (m->m_fat.fat_type == 32)
        return v >= 0x0FFFFFF8UL;
    return v >= 0xFFF8U;
}

static bool fat_is_bad_cluster(struct mount *m, uint32_t v)
{
    if (m->m_fat.fat_type == 32)
        return v == 0x0FFFFFF7UL;
    return v == 0xFFF7U;
}

static int fat_get_fat_entry(struct mount *m, uint32_t cluster, uint32_t *out)
{
    uint32_t offset;
    uint32_t sector;
    uint16_t ent_off;
    bufptr buf;
    const uint8_t *b;
    uint32_t v;

    if (cluster < 2 || cluster > m->m_fat.max_cluster) {
        udata.u_error = EIO;
        return -1;
    }

    if (m->m_fat.fat_type == 32)
        offset = cluster * 4UL;
    else
        offset = cluster * 2UL;

    sector = m->m_fat.fat_start + (offset / BLKSIZE);
    ent_off = (uint16_t)(offset % BLKSIZE);

    buf = bread(m->m_dev, sector, 0);
    if (buf == NULL)
        return -1;
    b = blkptr(buf, 0, BLKSIZE);

    if (m->m_fat.fat_type == 32) {
        v = fat_le32(b + ent_off) & 0x0FFFFFFFUL;
    } else {
        v = fat_le16(b + ent_off);
    }

    brelse(buf);
    *out = v;
    return 0;
}

static uint32_t fat_eoc_marker(struct mount *m)
{
    if (m->m_fat.fat_type == 32)
        return 0x0FFFFFFFUL;
    return 0xFFFFU;
}

static int fat_set_fat_entry(struct mount *m, uint32_t cluster, uint32_t value)
{
    uint32_t offset;
    uint32_t sector_index;
    uint16_t ent_off;
    uint_fast8_t fi;

    if (cluster < 2 || cluster > m->m_fat.max_cluster) {
        udata.u_error = EIO;
        return -1;
    }

    if (m->m_fat.fat_type == 32)
        offset = cluster * 4UL;
    else
        offset = cluster * 2UL;

    sector_index = offset / BLKSIZE;
    ent_off = (uint16_t)(offset % BLKSIZE);

    for (fi = 0; fi < m->m_fat.fat_count; fi++) {
        uint32_t sector = m->m_fat.fat_start + (uint32_t)fi * m->m_fat.fat_sectors + sector_index;
        bufptr buf = bread(m->m_dev, sector, 0);
        if (buf == NULL)
            return -1;

        if (m->m_fat.fat_type == 32) {
            uint8_t tmp[4];
            uint32_t cur;
            uint32_t v = value & 0x0FFFFFFFUL;
            blktok(tmp, buf, ent_off, sizeof(tmp));
            cur = fat_le32(tmp);
            cur = (cur & 0xF0000000UL) | v;
            fat_put32(tmp, cur);
            blkfromk(tmp, buf, ent_off, sizeof(tmp));
        } else {
            uint8_t tmp[2];
            fat_put16(tmp, (uint16_t)value);
            blkfromk(tmp, buf, ent_off, sizeof(tmp));
        }

        if (bfree(buf, 1))
            return -1;
    }

    return 0;
}

static int fat_zero_cluster(struct mount *m, uint32_t cluster)
{
    uint8_t spc = m->m_fat.sectors_per_cluster;
    uint32_t base_sector;
    uint_fast8_t s;

    if (cluster < 2 || cluster > m->m_fat.max_cluster) {
        udata.u_error = EIO;
        return -1;
    }

    base_sector = m->m_fat.data_start + (cluster - 2UL) * (uint32_t)spc;
    for (s = 0; s < spc; s++) {
        bufptr buf = bread(m->m_dev, base_sector + (uint32_t)s, 1);
        if (buf == NULL)
            return -1;
        blkzero(buf);
        bawrite(buf);
    }
    return 0;
}

static int fat_alloc_cluster(struct mount *m, uint32_t *out_cluster)
{
    uint32_t start = m->m_fat.alloc_hint;
    uint32_t cluster;
    uint32_t scanned = 0;
    uint32_t eoc = fat_eoc_marker(m);

    if (start < 2 || start > m->m_fat.max_cluster)
        start = 2;

    cluster = start;
    while (scanned < m->m_fat.cluster_count) {
        uint32_t v;
        if (fat_get_fat_entry(m, cluster, &v) != 0)
            return -1;
        if (v == 0) {
            if (fat_set_fat_entry(m, cluster, eoc) != 0)
                return -1;
            if (fat_zero_cluster(m, cluster) != 0)
                return -1;
            m->m_fat.alloc_hint = (cluster == m->m_fat.max_cluster) ? 2 : (cluster + 1U);
            *out_cluster = cluster;
            return 0;
        }
        cluster++;
        if (cluster > m->m_fat.max_cluster)
            cluster = 2;
        scanned++;
    }

    udata.u_error = ENOSPC;
    return -1;
}

static int fat_free_chain(struct mount *m, uint32_t start_cluster)
{
    uint32_t cluster = start_cluster;
    uint32_t limit = m->m_fat.cluster_count + 2U;

    while (cluster >= 2 && cluster <= m->m_fat.max_cluster && limit--) {
        uint32_t next;
        if (fat_get_fat_entry(m, cluster, &next) != 0)
            return -1;
        if (fat_set_fat_entry(m, cluster, 0) != 0)
            return -1;
        if (fat_is_eoc(m, next))
            break;
        if (fat_is_bad_cluster(m, next) || next == 0) {
            udata.u_error = EIO;
            return -1;
        }
        cluster = next;
    }
    if (limit == 0) {
        udata.u_error = EIO;
        return -1;
    }
    return 0;
}

static int fat_cluster_for_index(struct mount *m, inoptr ino, uint32_t index,
                                 uint32_t *out_cluster, unsigned int rwflg)
{
    uint32_t cluster;
    uint32_t cur_index;

    cluster = ino->c_fat.start_cluster;
    if (cluster < 2 || cluster > m->m_fat.max_cluster) {
        udata.u_error = EIO;
        return -1;
    }

    if (index == 0) {
        ino->c_fat.cache_cluster = cluster;
        ino->c_fat.cache_index = 0;
        *out_cluster = cluster;
        return 0;
    }

    if (ino->c_fat.cache_cluster &&
        index >= ino->c_fat.cache_index &&
        ino->c_fat.cache_cluster >= 2 &&
        ino->c_fat.cache_cluster <= m->m_fat.max_cluster) {
        cluster = ino->c_fat.cache_cluster;
        cur_index = ino->c_fat.cache_index;
    } else {
        cluster = ino->c_fat.start_cluster;
        cur_index = 0;
    }

    while (cur_index < index) {
        uint32_t next;
        if (fat_get_fat_entry(m, cluster, &next) != 0)
            return -1;
        if (fat_is_eoc(m, next)) {
            if (rwflg)
                return 1;
            if (fat_alloc_cluster(m, &next) != 0)
                return -1;
            if (fat_set_fat_entry(m, cluster, next) != 0)
                return -1;
        }
        if (fat_is_bad_cluster(m, next) || next == 0) {
            udata.u_error = EIO;
            return -1;
        }
        if (next < 2 || next > m->m_fat.max_cluster) {
            udata.u_error = EIO;
            return -1;
        }
        cluster = next;
        cur_index++;
    }

    ino->c_fat.cache_cluster = cluster;
    ino->c_fat.cache_index = index;
    *out_cluster = cluster;
    return 0;
}

blkno_t fat_bmap(inoptr ip, blkno_t bn, unsigned int rwflg)
{
    struct mount *m;
    uint32_t cluster_index;
    uint32_t cluster;
    uint8_t spc;
    uint8_t sector_in_cluster;

    if (!rwflg && (ip->c_flags & CRDONLY)) {
        udata.u_error = EROFS;
        return NULLBLK;
    }

    m = &fs_tab[ip->c_super];
    spc = m->m_fat.sectors_per_cluster;
    if (spc == 0) {
        udata.u_error = EIO;
        return NULLBLK;
    }

    /* FAT16 root directory is not mappable as a regular file. */
    if ((ip->c_fat.flags & FAT_I_ROOT) && m->m_fat.fat_type == 16)
        return NULLBLK;

    if (ip->c_fat.start_cluster == 0) {
        if (rwflg) /* read from empty file */
            return NULLBLK;
        if (ip->c_flags & CRDONLY) {
            udata.u_error = EROFS;
            return NULLBLK;
        }
        if (fat_alloc_cluster(m, &cluster) != 0)
            return NULLBLK;
        ip->c_fat.start_cluster = cluster;
        ip->c_flags |= CDIRTY;
        ip->c_fat.cache_cluster = cluster;
        ip->c_fat.cache_index = 0;
    }

    cluster_index = (uint32_t)bn / (uint32_t)spc;
    sector_in_cluster = (uint8_t)((uint32_t)bn % (uint32_t)spc);

    if (fat_cluster_for_index(m, ip, cluster_index, &cluster, rwflg) != 0)
        return NULLBLK;

    if (cluster < 2 || cluster > m->m_fat.max_cluster) {
        udata.u_error = EIO;
        return NULLBLK;
    }

    return (blkno_t)(m->m_fat.data_start +
                     (cluster - 2UL) * (uint32_t)spc +
                     (uint32_t)sector_in_cluster);
}

static uint8_t fat_sfn_checksum(const uint8_t *sfn11)
{
    uint8_t sum = 0;
    uint_fast8_t i;

    for (i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn11[i]);
    }
    return sum;
}

struct fat_lfn_state {
    uint8_t valid;
    uint8_t checksum;
    uint8_t next_seq;
    uint8_t too_long;
    char name[FILENAME_LEN + 1];
};

static void fat_lfn_reset(struct fat_lfn_state *st)
{
    memset(st, 0, sizeof(*st));
}

static uint16_t fat_lfn_word(const uint8_t *e, uint_fast8_t idx)
{
    uint16_t off;
    if (idx < 5)
        off = (uint16_t)(1 + idx * 2);
    else if (idx < 11)
        off = (uint16_t)(14 + (idx - 5) * 2);
    else
        off = (uint16_t)(28 + (idx - 11) * 2);
    return fat_le16(e + off);
}

static void fat_lfn_take(struct fat_lfn_state *st, const uint8_t *e)
{
    uint8_t ord = e[0];
    uint8_t seq = ord & 0x1F;
    uint_fast8_t i;
    uint32_t base;

    /* Validate the VFAT LFN entry shape. */
    if (e[11] != 0x0F || e[12] != 0 || fat_le16(e + 26) != 0) {
        fat_lfn_reset(st);
        return;
    }

    if (ord & 0x40) {
        if (seq == 0) {
            fat_lfn_reset(st);
            return;
        }
        memset(st, 0, sizeof(*st));
        st->valid = 1;
        st->checksum = e[13];
        st->next_seq = seq;
    }

    if (!st->valid || seq == 0 || seq != st->next_seq || e[13] != st->checksum) {
        fat_lfn_reset(st);
        return;
    }

    base = (uint32_t)(seq - 1U) * 13U;
    if (base >= FILENAME_LEN)
        st->too_long = 1;

    for (i = 0; i < 13; i++) {
        uint32_t pos = base + i;
        uint16_t wc = fat_lfn_word(e, i);
        uint8_t c;

        if (wc == 0x0000 || wc == 0xFFFF)
            c = 0;
        else if (wc < 0x80)
            c = (uint8_t)wc;
        else
            c = '_';

        if (pos < FILENAME_LEN)
            st->name[pos] = c;
    }
    st->name[FILENAME_LEN] = 0;

    st->next_seq--;
}

static void fat_sfn_to_name(const uint8_t *e, char *out)
{
    uint_fast8_t i;
    uint_fast8_t p = 0;
    uint8_t nt = e[12];
    bool lower_name = !!(nt & 0x08);
    bool lower_ext = !!(nt & 0x10);
    bool has_ext = false;

    for (i = 8; i < 11; i++) {
        if (e[i] != ' ') {
            has_ext = true;
            break;
        }
    }

    for (i = 0; i < 8; i++) {
        uint8_t c = e[i];
        if (c == ' ')
            break;
        if (lower_name && c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + 0x20);
        if (p < FILENAME_LEN)
            out[p++] = (char)c;
    }

    if (has_ext && p < FILENAME_LEN)
        out[p++] = '.';

    if (has_ext) {
        for (i = 8; i < 11; i++) {
            uint8_t c = e[i];
            if (c == ' ')
                break;
            if (lower_ext && c >= 'A' && c <= 'Z')
                c = (uint8_t)(c + 0x20);
            if (p < FILENAME_LEN)
                out[p++] = (char)c;
        }
    }

    out[p] = 0;
}

static bool fat_namecomp_ci(const uint8_t *n1, const char *n2)
{
    uint_fast8_t n;

    n = FILENAME_LEN;
    while (*n1 && *n1 != '/') {
        uint8_t c1 = *n1++;
        uint8_t c2 = (uint8_t)*n2++;
        if (c1 >= 'a' && c1 <= 'z')
            c1 = (uint8_t)(c1 - 0x20);
        if (c2 >= 'a' && c2 <= 'z')
            c2 = (uint8_t)(c2 - 0x20);
        if (c1 != c2)
            return false;
        if (--n == 0)
            return true;
    }

    return (*n2 == '\0' || *n2 == '/');
}

static uint16_t fat_make_ino(uint32_t sector, uint16_t off)
{
    uint32_t v = sector ^ ((uint32_t)off << 16) ^ (sector >> 16);
    uint16_t ino = (uint16_t)(v ^ (v >> 16));
    if (ino < 2)
        ino += 2;
    if (ino == ROOTINODE)
        ino++;
    return ino;
}

static uint16_t fat_make_ino_cluster(uint32_t cluster)
{
    uint32_t v = cluster ^ (cluster >> 16);
    uint16_t ino = (uint16_t)(v ^ (v >> 8));
    if (ino < 2)
        ino += 2;
    if (ino == ROOTINODE)
        ino++;
    return ino;
}

static int fat_dir_map(struct mount *m, inoptr dir, uint32_t raw_off,
                       uint32_t *out_sector, uint16_t *out_off)
{
    uint32_t sector_index;
    uint16_t off;

    off = (uint16_t)(raw_off % BLKSIZE);
    sector_index = raw_off / BLKSIZE;

    if ((m->m_fat.fat_type == 16) && (dir->c_fat.flags & FAT_I_ROOT)) {
        if (sector_index >= m->m_fat.root_dir_sectors)
            return 1;
        *out_sector = m->m_fat.root_dir_start + sector_index;
        *out_off = off;
        return 0;
    }

    if (dir->c_fat.start_cluster == 0)
        return 1;

    {
        uint8_t spc = m->m_fat.sectors_per_cluster;
        uint32_t cluster_index = sector_index / (uint32_t)spc;
        uint8_t sector_in_cluster = (uint8_t)(sector_index % (uint32_t)spc);
        uint32_t cluster;
        int r;

        r = fat_cluster_for_index(m, dir, cluster_index, &cluster, 1);
        if (r != 0)
            return r;
        if (cluster < 2 || cluster > m->m_fat.max_cluster) {
            udata.u_error = EIO;
            return -1;
        }

        *out_sector = m->m_fat.data_start +
                      (cluster - 2UL) * (uint32_t)spc +
                      (uint32_t)sector_in_cluster;
        *out_off = off;
        return 0;
    }
}

static int fat_dir_read_raw(struct mount *m, inoptr dir, uint32_t raw_off,
                            uint8_t *out, uint32_t *out_sector, uint16_t *out_off)
{
    uint32_t sector;
    uint16_t off;
    bufptr buf;
    int r;

    r = fat_dir_map(m, dir, raw_off, &sector, &off);
    if (r != 0)
        return r;

    buf = bread(m->m_dev, sector, 0);
    if (buf == NULL)
        return -1;
    blktok(out, buf, off, 32);
    brelse(buf);

    if (out_sector)
        *out_sector = sector;
    if (out_off)
        *out_off = off;
    return 0;
}

static inoptr fat_i_open_dir_cluster(struct mount *m, uint32_t cluster)
{
    inoptr free_slot = NULLINODE;
    inoptr j;
    uint8_t e[32];

    if (m->m_fat.fat_type == 16 && cluster == 0)
        return fat_iroot(m);
    if (m->m_fat.fat_type == 32 && cluster == m->m_fat.root_cluster)
        return fat_iroot(m);

    for (j = i_tab; j < i_tab + ITABSIZE; j++) {
        if (!j->c_refs) {
            if (free_slot == NULLINODE)
                free_slot = j;
            continue;
        }
        if (j->c_dev == m->m_dev &&
            j->c_super == (uint8_t)(m - fs_tab) &&
            getmode(j) == MODE_R(F_DIR) &&
            j->c_fat.start_cluster == cluster) {
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
    free_slot->c_super = (uint8_t)(m - fs_tab);
    free_slot->c_num = fat_make_ino_cluster(cluster);
    free_slot->c_magic = CMAGIC;
    free_slot->c_flags = (m->m_flags & MS_RDONLY) ? CRDONLY : 0;
    free_slot->c_refs = 1;

    free_slot->c_node.i_mode = F_DIR | 0777;
    free_slot->c_node.i_nlink = 2;
    free_slot->c_node.i_uid = 0;
    free_slot->c_node.i_gid = 0;
    free_slot->c_node.i_size = 0;

    free_slot->c_fat.start_cluster = cluster;
    free_slot->c_fat.attrib = 0x10;

    /* Attempt to learn the parent cluster from the ".." entry. */
    free_slot->c_fat.parent_cluster = cluster;
    if (fat_dir_read_raw(m, free_slot, 32, e, NULL, NULL) == 0) {
        if (e[0] == '.' && e[1] == '.' && e[11] != 0x0F) {
            uint32_t pcl = (uint32_t)fat_le16(e + 26);
            if (m->m_fat.fat_type == 32)
                pcl |= (uint32_t)fat_le16(e + 20) << 16;
            if (pcl)
                free_slot->c_fat.parent_cluster = pcl;
            else if (m->m_fat.fat_type == 16)
                free_slot->c_fat.parent_cluster = 0;
        }
    }

    return free_slot;
}

static inoptr fat_i_open_dirent(struct mount *m, inoptr parent,
                                uint32_t dirent_sector, uint16_t dirent_off,
                                const uint8_t *e)
{
    inoptr free_slot = NULLINODE;
    inoptr nindex = NULLINODE;
    inoptr j;
    uint32_t start_cluster;
    uint32_t size;
    uint8_t attrib;
    bool is_dir;

    attrib = e[11];
    is_dir = !!(attrib & 0x10);

    start_cluster = (uint32_t)fat_le16(e + 26);
    if (m->m_fat.fat_type == 32)
        start_cluster |= (uint32_t)fat_le16(e + 20) << 16;

    size = fat_le32(e + 28);

    for (j = i_tab; j < i_tab + ITABSIZE; j++) {
        if (!j->c_refs) {
            if (free_slot == NULLINODE)
                free_slot = j;
        }
        if (j->c_dev == m->m_dev &&
            j->c_super == (uint8_t)(m - fs_tab) &&
            !(j->c_fat.flags & FAT_I_ROOT) &&
            j->c_fat.dirent_sector == dirent_sector &&
            j->c_fat.dirent_offset == dirent_off) {
            nindex = j;
            break;
        }
    }

    if (nindex != NULLINODE && nindex->c_refs) {
        nindex->c_refs++;
        return nindex;
    }

    if (nindex == NULLINODE)
        nindex = free_slot;
    if (nindex == NULLINODE) {
        udata.u_error = ENFILE;
        return NULLINODE;
    }

    memset(&nindex->c_node, 0, sizeof(nindex->c_node));
    memset(&nindex->c_fat, 0, sizeof(nindex->c_fat));

    nindex->c_dev = m->m_dev;
    nindex->c_super = (uint8_t)(m - fs_tab);
    nindex->c_num = fat_make_ino(dirent_sector, dirent_off);
    nindex->c_magic = CMAGIC;
    nindex->c_flags = (m->m_flags & MS_RDONLY) ? CRDONLY : 0;
    nindex->c_refs = 1;

    nindex->c_fat.start_cluster = start_cluster;
    nindex->c_fat.dirent_sector = dirent_sector;
    nindex->c_fat.dirent_offset = dirent_off;
    nindex->c_fat.attrib = attrib;
    nindex->c_fat.parent_cluster = parent->c_fat.start_cluster;

    if (is_dir) {
        nindex->c_node.i_mode = F_DIR | 0777;
        nindex->c_node.i_nlink = 2;
        nindex->c_node.i_size = 0;
    } else {
        nindex->c_node.i_mode = F_REG | 0666;
        nindex->c_node.i_nlink = 1;
        nindex->c_node.i_size = size;
    }

    if (attrib & 0x01)
        nindex->c_node.i_mode &= (uint16_t)~0222;
    if (attrib & 0x01)
        nindex->c_flags |= CRDONLY;

    nindex->c_node.i_uid = 0;
    nindex->c_node.i_gid = 0;

    return nindex;
}

inoptr fat_srch_dir(inoptr wd, uint8_t *compname)
{
    struct mount *m = &fs_tab[wd->c_super];
    struct fat_lfn_state lfn;
    uint32_t raw_off = 0;
    uint8_t e[32];
    uint32_t dirent_sector;
    uint16_t dirent_off;

    if (compname[0] == '.' && compname[1] == '\0')
        return i_ref(wd);

    if (compname[0] == '.' && compname[1] == '.' && compname[2] == '\0') {
        if (wd->c_fat.flags & FAT_I_ROOT)
            return i_ref(wd);
        return fat_i_open_dir_cluster(m, wd->c_fat.parent_cluster);
    }

    fat_lfn_reset(&lfn);

    while (1) {
        int r;
        uint8_t attr;
        char sname[FILENAME_LEN + 1];
        bool have_lfn;

        r = fat_dir_read_raw(m, wd, raw_off, e, &dirent_sector, &dirent_off);
        if (r != 0)
            break;
        raw_off += 32;

        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5) {
            fat_lfn_reset(&lfn);
            continue;
        }

        attr = e[11];
        if (attr == 0x0F) {
            fat_lfn_take(&lfn, e);
            continue;
        }

        memset(sname, 0, sizeof(sname));
        have_lfn = (lfn.valid && lfn.next_seq == 0 && lfn.checksum == fat_sfn_checksum(e) && lfn.name[0]);
        fat_sfn_to_name(e, sname);

        if (attr & 0x08) {
            fat_lfn_reset(&lfn);
            continue; /* volume label */
        }

        if ((have_lfn && fat_namecomp_ci(compname, lfn.name)) || fat_namecomp_ci(compname, sname)) {
            inoptr out = fat_i_open_dirent(m, wd, dirent_sector, dirent_off, e);
            fat_lfn_reset(&lfn);
            return out;
        }
        fat_lfn_reset(&lfn);
    }

    return NULLINODE;
}

void fat_readi_dir(inoptr ino, uint_fast8_t flag)
{
    struct mount *m;
    struct fat_lfn_state lfn;
    uint8_t e[32];
    uint32_t dirent_sector;
    uint16_t dirent_off;

    used(flag);

    m = &fs_tab[ino->c_super];
    fat_lfn_reset(&lfn);
    udata.u_done = 0;

    if (udata.u_offset < 0 || ((uint32_t)udata.u_offset & 31U)) {
        udata.u_error = EINVAL;
        goto out;
    }

    if (udata.u_sysio && udata.u_count >= DIR_LEN)
        memset(udata.u_base, 0, DIR_LEN);

    while (udata.u_count >= DIR_LEN) {
        uint8_t attr;
        char name[FILENAME_LEN + 1];
        bool have_lfn;
        struct direct d;
        int r;

        r = fat_dir_read_raw(m, ino, (uint32_t)udata.u_offset, e, &dirent_sector, &dirent_off);
        if (r != 0)
            break;
        udata.u_offset += DIR_LEN;

        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5) {
            fat_lfn_reset(&lfn);
            continue;
        }

        attr = e[11];
        if (attr == 0x0F) {
            fat_lfn_take(&lfn, e);
            continue;
        }

        if (attr & 0x08) {
            fat_lfn_reset(&lfn);
            continue; /* volume label */
        }

        memset(name, 0, sizeof(name));
        have_lfn = (lfn.valid && lfn.next_seq == 0 && lfn.checksum == fat_sfn_checksum(e) && lfn.name[0]);
        if (have_lfn)
            memcpy(name, lfn.name, sizeof(name));
        else
            fat_sfn_to_name(e, name);
        fat_lfn_reset(&lfn);

        memset(&d, 0, sizeof(d));
        d.d_ino = fat_make_ino(dirent_sector, dirent_off);
        memcpy(d.d_name, name, FILENAME_LEN);

        if (udata.u_sysio)
            memcpy(udata.u_base, &d, DIR_LEN);
        else if (uput(&d, udata.u_base, DIR_LEN) != 0) {
            udata.u_error = EFAULT;
            break;
        }

        udata.u_base += DIR_LEN;
        udata.u_count -= DIR_LEN;
        udata.u_done += DIR_LEN;
    }

out:
    if (udata.u_done == 0 && udata.u_error)
        udata.u_done = (usize_t)-1;
}

int fat_trunc_blocks(inoptr ino, blkno_t nblock)
{
    struct mount *m;
    uint8_t spc;
    uint32_t keep_clusters;
    uint32_t last_cluster;
    uint32_t next;
    int r;

    if (ino->c_flags & CRDONLY) {
        udata.u_error = EROFS;
        return -1;
    }

    m = &fs_tab[ino->c_super];

    /* Root directory on FAT16 cannot be truncated. */
    if ((ino->c_fat.flags & FAT_I_ROOT) && m->m_fat.fat_type == 16) {
        udata.u_error = EROFS;
        return -1;
    }

    spc = m->m_fat.sectors_per_cluster;
    if (spc == 0) {
        udata.u_error = EIO;
        return -1;
    }

    if (nblock == 0)
        keep_clusters = 0;
    else
        keep_clusters = ((uint32_t)nblock + (uint32_t)spc - 1U) / (uint32_t)spc;

    if (keep_clusters == 0) {
        if (ino->c_fat.start_cluster) {
            if (fat_free_chain(m, ino->c_fat.start_cluster) != 0)
                return -1;
        }
        ino->c_fat.start_cluster = 0;
        ino->c_fat.cache_cluster = 0;
        ino->c_fat.cache_index = 0;
        ino->c_flags |= CDIRTY;
        return 0;
    }

    if (ino->c_fat.start_cluster == 0)
        return 0;

    /* Find last cluster to keep. */
    r = fat_cluster_for_index(m, ino, keep_clusters - 1U, &last_cluster, 1);
    if (r == 1)
        return 0; /* file shorter than requested */
    if (r != 0)
        return -1;

    if (fat_get_fat_entry(m, last_cluster, &next) != 0)
        return -1;

    if (!fat_is_eoc(m, next)) {
        if (fat_set_fat_entry(m, last_cluster, fat_eoc_marker(m)) != 0)
            return -1;
        if (fat_free_chain(m, next) != 0)
            return -1;
    }

    ino->c_fat.cache_cluster = last_cluster;
    ino->c_fat.cache_index = keep_clusters - 1U;
    ino->c_flags |= CDIRTY;
    return 0;
}

static bool fat_is_leap_year(uint16_t year)
{
    if ((year % 4) != 0)
        return false;
    if ((year % 100) != 0)
        return true;
    return (year % 400) == 0;
}

static uint8_t fat_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint8_t d = dim[month - 1];
    if (month == 2 && fat_is_leap_year(year))
        d++;
    return d;
}

static void fat_unix_to_dos(uint32_t t, uint16_t *dos_date, uint16_t *dos_time)
{
    uint32_t days = t / 86400UL;
    uint32_t rem = t % 86400UL;
    uint16_t year = 1970;
    uint8_t month = 1;
    uint8_t day;
    uint8_t hour = (uint8_t)(rem / 3600UL);
    uint8_t min = (uint8_t)((rem % 3600UL) / 60UL);
    uint8_t sec = (uint8_t)(rem % 60UL);

    while (1) {
        uint16_t diy = fat_is_leap_year(year) ? 366 : 365;
        if (days < diy)
            break;
        days -= diy;
        year++;
        if (year > 2107) {
            year = 2107;
            days = 364;
            hour = 23;
            min = 59;
            sec = 58;
            break;
        }
    }

    while (month <= 12) {
        uint8_t dim = fat_days_in_month(year, month);
        if (days < dim)
            break;
        days -= dim;
        month++;
    }

    day = (uint8_t)(days + 1U);

    if (year < 1980) {
        year = 1980;
        month = 1;
        day = 1;
        hour = 0;
        min = 0;
        sec = 0;
    }

    *dos_date = (uint16_t)(((year - 1980) << 9) | ((uint16_t)month << 5) | day);
    *dos_time = (uint16_t)(((uint16_t)hour << 11) | ((uint16_t)min << 5) | ((uint16_t)(sec / 2U)));
}

void fat_wr_inode(inoptr ino)
{
    struct mount *m = &fs_tab[ino->c_super];
    bufptr buf;
    uint8_t e[32];
    uint32_t start_cluster;
    uint8_t attr;
    bool is_dir;
    uint16_t d, t;

    if (!(ino->c_flags & CDIRTY))
        return;

    /* Deleted/unlinked or synthetic entries have no directory entry to update. */
    if (ino->c_node.i_nlink == 0 ||
        (ino->c_fat.dirent_sector == 0 && ino->c_fat.dirent_offset == 0)) {
        ino->c_flags &= ~CDIRTY;
        return;
    }

    buf = bread(m->m_dev, ino->c_fat.dirent_sector, 0);
    if (buf == NULL)
        return;

    blktok(e, buf, ino->c_fat.dirent_offset, 32);

    if (e[0] == 0x00 || e[0] == 0xE5 || e[11] == 0x0F) {
        brelse(buf);
        ino->c_flags &= ~CDIRTY;
        return;
    }

    is_dir = (getmode(ino) == MODE_R(F_DIR));
    attr = (uint8_t)(ino->c_fat.attrib & 0x0F);
    if (is_dir)
        attr |= 0x10;
    else
        attr |= 0x20;

    if (!(ino->c_node.i_mode & 0222))
        attr |= 0x01;
    else
        attr &= (uint8_t)~0x01;

    e[11] = attr;
    ino->c_fat.attrib = attr;

    start_cluster = ino->c_fat.start_cluster;
    fat_put16(e + 26, (uint16_t)(start_cluster & 0xFFFF));
    if (m->m_fat.fat_type == 32)
        fat_put16(e + 20, (uint16_t)(start_cluster >> 16));
    else
        fat_put16(e + 20, 0);

    if (is_dir)
        fat_put32(e + 28, 0);
    else
        fat_put32(e + 28, (uint32_t)ino->c_node.i_size);

    fat_unix_to_dos(ino->c_node.i_ctime, &d, &t);
    fat_put16(e + 14, t);
    fat_put16(e + 16, d);

    fat_unix_to_dos(ino->c_node.i_atime, &d, &t);
    fat_put16(e + 18, d);

    fat_unix_to_dos(ino->c_node.i_mtime, &d, &t);
    fat_put16(e + 22, t);
    fat_put16(e + 24, d);

    blkfromk(e, buf, ino->c_fat.dirent_offset, 32);
    bfree(buf, 1);

    ino->c_flags &= ~CDIRTY;
}

static int fat_write_dirent(struct mount *m, uint32_t sector, uint16_t off, const uint8_t *e)
{
    bufptr buf = bread(m->m_dev, sector, 0);
    if (buf == NULL)
        return -1;
    blkfromk(e, buf, off, 32);
    return bfree(buf, 1);
}

static int fat_mark_deleted(struct mount *m, uint32_t sector, uint16_t off)
{
    bufptr buf = bread(m->m_dev, sector, 0);
    uint8_t del = 0xE5;
    if (buf == NULL)
        return -1;
    blkfromk(&del, buf, off, 1);
    return bfree(buf, 1);
}

static int fat_sfn_from_name(const uint8_t *name, uint8_t *sfn)
{
    uint_fast8_t i;
    uint_fast8_t p = 0;
    uint_fast8_t ext = 8;
    uint_fast8_t dot = 0xFF;

    memset(sfn, ' ', 11);

    if (name[0] == '.' && name[1] == '\0') {
        sfn[0] = '.';
        return 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        sfn[0] = '.';
        sfn[1] = '.';
        return 0;
    }

    for (i = 0; i < FILENAME_LEN && name[i]; i++) {
        if (name[i] == '.') {
            if (dot != 0xFF)
                return (udata.u_error = EINVAL), -1;
            dot = i;
        }
    }

    for (i = 0; i < FILENAME_LEN && name[i]; i++) {
        uint8_t c = name[i];

        if (c == '.') {
            ext = 8;
            p = 0;
            continue;
        }

        if (c <= 0x20 || c >= 0x7F)
            return (udata.u_error = EINVAL), -1;
        if (c == '\"' || c == '*' || c == '+' || c == ',' || c == '/' ||
            c == ':' || c == ';' || c == '<' || c == '=' || c == '>' ||
            c == '?' || c == '[' || c == '\\' || c == ']' || c == '|')
            return (udata.u_error = EINVAL), -1;

        if (c >= 'a' && c <= 'z')
            c = (uint8_t)(c - 0x20);

        if (dot != 0xFF && i > dot) {
            if (ext >= 11)
                return (udata.u_error = EINVAL), -1;
            sfn[ext++] = c;
            continue;
        }

        if (p >= 8)
            return (udata.u_error = EINVAL), -1;
        sfn[p++] = c;
    }

    if (sfn[0] == ' ')
        return (udata.u_error = EINVAL), -1;

    return 0;
}

static int fat_dir_find_free(struct mount *m, inoptr dir, uint32_t *out_raw_off)
{
    uint32_t raw_off = 0;
    uint32_t first_deleted = 0xFFFFFFFFUL;
    uint32_t end_off = 0xFFFFFFFFUL;
    uint8_t e[32];

    if (dir->c_fat.start_cluster == 0 && !(dir->c_fat.flags & FAT_I_ROOT)) {
        uint32_t c;
        if (fat_alloc_cluster(m, &c) != 0)
            return -1;
        dir->c_fat.start_cluster = c;
        dir->c_fat.cache_cluster = c;
        dir->c_fat.cache_index = 0;
        dir->c_flags |= CDIRTY;
        *out_raw_off = 0;
        return 0;
    }

    while (1) {
        int r = fat_dir_read_raw(m, dir, raw_off, e, NULL, NULL);
        if (r == 0) {
            if (e[0] == 0x00) {
                end_off = raw_off;
                break;
            }
            if (e[0] == 0xE5 && first_deleted == 0xFFFFFFFFUL)
                first_deleted = raw_off;
            raw_off += 32;
            continue;
        }
        if (r != 1)
            return -1;
        break;
    }

    if (first_deleted != 0xFFFFFFFFUL) {
        *out_raw_off = first_deleted;
        return 0;
    }
    if (end_off != 0xFFFFFFFFUL) {
        *out_raw_off = end_off;
        return 0;
    }

    /* Need to grow the directory (except FAT16 root). */
    if ((dir->c_fat.flags & FAT_I_ROOT) && m->m_fat.fat_type == 16) {
        udata.u_error = ENOSPC;
        return -1;
    }

    {
        uint32_t last;
        uint32_t newc;
        uint32_t next;
        uint32_t cluster = dir->c_fat.start_cluster;
        uint32_t limit = m->m_fat.cluster_count + 2U;

        if (cluster == 0) {
            udata.u_error = EIO;
            return -1;
        }

        last = cluster;
        while (limit--) {
            if (fat_get_fat_entry(m, last, &next) != 0)
                return -1;
            if (fat_is_eoc(m, next))
                break;
            if (fat_is_bad_cluster(m, next) || next == 0 || next > m->m_fat.max_cluster) {
                udata.u_error = EIO;
                return -1;
            }
            last = next;
        }
        if (limit == 0) {
            udata.u_error = EIO;
            return -1;
        }

        if (fat_alloc_cluster(m, &newc) != 0)
            return -1;
        if (fat_set_fat_entry(m, last, newc) != 0)
            return -1;

        *out_raw_off = raw_off;
        return 0;
    }
}

bool fat_ch_link(inoptr wd, uint8_t *oldname, uint8_t *newname, inoptr nindex)
{
    struct mount *m = &fs_tab[wd->c_super];

    if (wd->c_flags & CRDONLY) {
        udata.u_error = EROFS;
        return false;
    }
    if (!(getperm(wd) & OTH_WR)) {
        udata.u_error = EACCES;
        return false;
    }

    /* Insert */
    if (!*oldname) {
        uint8_t sfn[11];
        uint8_t e[32];
        uint32_t raw_off;
        uint32_t sector;
        uint16_t off;
        uint8_t attr;
        uint16_t dd, tt;
        uint32_t start_cluster;

        if (!*newname || nindex == NULLINODE) {
            udata.u_error = EINVAL;
            return false;
        }

        if (fat_sfn_from_name(newname, sfn) != 0)
            return false;

        /* Reject existing entry (case-insensitive). */
        if (!(newname[0] == '.' && (newname[1] == '\0' ||
            (newname[1] == '.' && newname[2] == '\0')))) {
            inoptr exist = fat_srch_dir(wd, newname);
            if (exist) {
                i_deref(exist);
                udata.u_error = EEXIST;
                return false;
            }
        }

        if (fat_dir_find_free(m, wd, &raw_off) != 0)
            return false;
        if (fat_dir_map(m, wd, raw_off, &sector, &off) != 0) {
            udata.u_error = EIO;
            return false;
        }

        memset(e, 0, sizeof(e));
        memcpy(e, sfn, 11);

        attr = (uint8_t)(nindex->c_fat.attrib & 0x0F);
        if (getmode(nindex) == MODE_R(F_DIR))
            attr |= 0x10;
        else
            attr |= 0x20;
        e[11] = attr;

        start_cluster = nindex->c_fat.start_cluster;
        fat_put16(e + 26, (uint16_t)(start_cluster & 0xFFFF));
        if (m->m_fat.fat_type == 32)
            fat_put16(e + 20, (uint16_t)(start_cluster >> 16));

        if (getmode(nindex) == MODE_R(F_DIR))
            fat_put32(e + 28, 0);
        else
            fat_put32(e + 28, (uint32_t)nindex->c_node.i_size);

        fat_unix_to_dos(tod.low, &dd, &tt);
        fat_put16(e + 14, tt);
        fat_put16(e + 16, dd);
        fat_put16(e + 18, dd);
        fat_put16(e + 22, tt);
        fat_put16(e + 24, dd);

        if (fat_write_dirent(m, sector, off, e) != 0)
            return false;

        /* Update the inode's backpointer only for "real" parent dirents. */
        if (!(newname[0] == '.' && (newname[1] == '\0' ||
            (newname[1] == '.' && newname[2] == '\0')))) {
            nindex->c_fat.dirent_sector = sector;
            nindex->c_fat.dirent_offset = off;
            nindex->c_fat.parent_cluster = wd->c_fat.start_cluster;
        }
        nindex->c_fat.attrib = attr;

        return true;
    }

    /* Delete */
    if (*newname || nindex != NULLINODE) {
        udata.u_error = ENOSYS;
        return false;
    }

    {
        struct fat_lfn_state lfn;
        uint32_t raw_off = 0;
        uint32_t lfn_offs[20];
        uint_fast8_t lfn_n = 0;
        uint8_t e[32];
        uint32_t sector;
        uint16_t off;

        fat_lfn_reset(&lfn);

        while (1) {
            int r;
            uint8_t attr;
            char name[FILENAME_LEN + 1];
            bool have_lfn;

            r = fat_dir_read_raw(m, wd, raw_off, e, &sector, &off);
            if (r != 0)
                break;

            if (e[0] == 0x00)
                break;

            attr = e[11];
            if (e[0] == 0xE5) {
                fat_lfn_reset(&lfn);
                lfn_n = 0;
                raw_off += 32;
                continue;
            }

            if (attr == 0x0F) {
                if (lfn_n < 20)
                    lfn_offs[lfn_n++] = raw_off;
                fat_lfn_take(&lfn, e);
                raw_off += 32;
                continue;
            }

            if (attr & 0x08) {
                fat_lfn_reset(&lfn);
                lfn_n = 0;
                raw_off += 32;
                continue;
            }

            memset(name, 0, sizeof(name));
            fat_sfn_to_name(e, name);
            have_lfn = (lfn.valid && lfn.next_seq == 0 && lfn.checksum == fat_sfn_checksum(e) && lfn.name[0]);

            if (fat_namecomp_ci(oldname, name) || (have_lfn && fat_namecomp_ci(oldname, lfn.name))) {
                uint_fast8_t i;
                if (fat_mark_deleted(m, sector, off) != 0)
                    return false;
                if (have_lfn) {
                    for (i = 0; i < lfn_n; i++) {
                        uint32_t ls;
                        uint16_t lo;
                        if (fat_dir_map(m, wd, lfn_offs[i], &ls, &lo) == 0)
                            if (fat_mark_deleted(m, ls, lo) != 0)
                                return false;
                    }
                }
                fat_lfn_reset(&lfn);
                return true;
            }

            fat_lfn_reset(&lfn);
            lfn_n = 0;
            raw_off += 32;
        }

        udata.u_error = ENOENT;
        return false;
    }
}

inoptr fat_newfile(inoptr pino, uint8_t *name)
{
    struct mount *m;
    inoptr nindex = NULLINODE;
    inoptr j;

    if (!pino) {
        udata.u_error = ENXIO;
        return NULLINODE;
    }

    m = &fs_tab[pino->c_super];

    if (pino->c_flags & CRDONLY) {
        udata.u_error = EROFS;
        i_deref(pino);
        return NULLINODE;
    }

    for (j = i_tab; j < i_tab + ITABSIZE; j++) {
        if (!j->c_refs) {
            nindex = j;
            break;
        }
    }

    if (nindex == NULLINODE) {
        udata.u_error = ENFILE;
        i_deref(pino);
        return NULLINODE;
    }

    memset(&nindex->c_node, 0, sizeof(nindex->c_node));
    memset(&nindex->c_fat, 0, sizeof(nindex->c_fat));
    nindex->c_dev = m->m_dev;
    nindex->c_super = pino->c_super;
    nindex->c_num = (uint16_t)(2 + (uint16_t)(nindex - i_tab));
    if (nindex->c_num == ROOTINODE)
        nindex->c_num++;
    nindex->c_magic = CMAGIC;
    nindex->c_flags = (m->m_flags & MS_RDONLY) ? CRDONLY : 0;
    nindex->c_refs = 1;

    nindex->c_node.i_mode = F_REG | 0666;
    nindex->c_node.i_nlink = 1;
    nindex->c_node.i_uid = 0;
    nindex->c_node.i_gid = 0;
    nindex->c_node.i_size = 0;

    nindex->c_fat.start_cluster = 0;
    nindex->c_fat.attrib = 0x20;
    nindex->c_fat.parent_cluster = pino->c_fat.start_cluster;

    i_lock(pino);
    i_lock(nindex);

    if (!fat_ch_link(pino, (uint8_t *)"", name, nindex)) {
        i_unlock_deref(pino);
        i_unlock_deref(nindex);
        return NULLINODE;
    }

    i_unlock_deref(pino);
    return nindex;
}

#endif
