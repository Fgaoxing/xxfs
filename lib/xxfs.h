#ifndef _XXFS_H
#define _XXFS_H

#include <stddef.h>
#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;
typedef int64_t  s64;
typedef int32_t  s32;
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
typedef _Bool bool;
#define true  1
#define false 0
#endif

#ifndef likely
#define likely(x)       __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x)     __builtin_expect(!!(x), 0)
#endif
#define always_inline   __attribute__((always_inline)) inline
#define __cold          __attribute__((cold))
#define __aligned(x)    __attribute__((aligned(x)))
#define prefetch_r(p)   __builtin_prefetch((p), 0, 1)
#define prefetch_w(p)   __builtin_prefetch((p), 1, 1)

#define XXFS_OK         0
#define XXFS_ENOENT     -2
#define XXFS_EEXIST     -17
#define XXFS_ENOMEM     -12
#define XXFS_ENOSPC     -28
#define XXFS_EIO        -5
#define XXFS_EINVAL     -22
#define XXFS_ENOTDIR    -20
#define XXFS_EISDIR     -21
#define XXFS_ENOTEMPTY  -39
#define XXFS_EACCES     -13
#define XXFS_ENAMETOOLONG -36

#define XXFS_BLOCK_SIZE      4096
#define XXFS_BLOCK_BITS      12
#define XXFS_MAGIC           0x58585346ULL
#define XXFS_VERSION         3

#define XXFS_BG_BLOCKS       8192
#define XXFS_BG_SIZE         (XXFS_BG_BLOCKS * XXFS_BLOCK_SIZE)

#define XXFS_INODE_SIZE      256
#define XXFS_INODES_PER_BG   4096

#define XXFS_INLINE_MAX      128

#define XXFS_WAL_BLOCKS      2048
#define XXFS_WAL_SIZE        (XXFS_WAL_BLOCKS * XXFS_BLOCK_SIZE)

#define XXFS_PP_BLOCKS       512
#define XXFS_PP_SIZE         (XXFS_PP_BLOCKS * XXFS_BLOCK_SIZE)
#define XXFS_PP_FLUSH_PCT    75

#define XXFS_LRC_DATA        6
#define XXFS_LRC_LOCAL       2
#define XXFS_LRC_GLOBAL      2
#define XXFS_LRC_TOTAL       (XXFS_LRC_DATA + XXFS_LRC_LOCAL + XXFS_LRC_GLOBAL)

#define XXFS_MAX_PATH        4096
#define XXFS_MAX_NAME        255

#define XXFS_FT_REG          1
#define XXFS_FT_DIR          2
#define XXFS_FT_LNK          3
#define XXFS_FT_BLK          4
#define XXFS_FT_CHR          5
#define XXFS_FT_FIFO         6
#define XXFS_FT_SOCK         7

#define XXFS_PERM_OWNER_RX   0500
#define XXFS_PERM_OWNER_RWX  0700
#define XXFS_PERM_GROUP_RX   0050
#define XXFS_PERM_GROUP_RWX  0070
#define XXFS_PERM_OTHER_RX   0005
#define XXFS_PERM_OTHER_RWX  0007
#define XXFS_PERM_SETUID     04000
#define XXFS_PERM_SETGID     02000
#define XXFS_PERM_STICKY     01000

#define XXFS_FLAG_SYNC       0x0001ULL
#define XXFS_FLAG_NOATIME    0x0002ULL
#define XXFS_FLAG_DATACHECK  0x0004ULL
#define XXFS_FLAG_READONLY   0x0008ULL
#define XXFS_FLAG_COMPRESS   0x0010ULL
#define XXFS_FLAG_ENCRYPT    0x0020ULL
#define XXFS_FLAG_NOLOCK     0x0040ULL

struct xxfs_super {
    u32 s_magic;
    u32 s_version;
    u32 s_block_size;
    u32 s_block_size_bits;
    u64 s_block_count;
    u64 s_free_blocks;
    u64 s_inodes_count;
    u64 s_free_inodes;
    u64 s_root_ino;
    u64 s_cow_generation;
    u64 s_main_idx_root;
    u64 s_wal_off;
    u32 s_wal_blocks;
    u64 s_wal_seq;
    u64 s_wal_commit_seq;
    u64 s_pp_ping_off;
    u64 s_pp_pong_off;
    u32 s_pp_blocks;
    u32 s_pp_active;
    u64 s_pp_ping_seq;
    u64 s_pp_pong_seq;
    u64 s_lrc_table_off;
    u32 s_lrc_groups;
    u32 s_bg_count;
    u32 s_bg_blocks;
    u64 s_bg_desc_off;
    u64 s_data_off;
    u64 s_mtime;
    u64 s_wtime;
    u32 s_state;
    u32 s_feature_compat;
    u32 s_feature_ro_compat;
    u32 s_feature_incompat;
    u32 s_checksum;
    u64 s_pdir_off;
    u32 s_pdir_size;
    u8  s_pdir_depth;
    u8  s_pad[3];
    u8  s_reserved[3868];
} __attribute__((packed));

struct xxfs_bg_desc {
    u64 bg_block_bitmap;
    u64 bg_inode_bitmap;
    u32 bg_free_blocks;
    u32 bg_free_inodes;
    u32 bg_checksum;
    u8  bg_reserved[44];
};

struct xxfs_inode {
    u16 i_mode;
    u16 i_uid;
    u16 i_gid;
    u16 i_file_type;
    u32 i_nlinks;
    u64 i_size;
    u64 i_blocks;
    u64 i_atime;
    u64 i_mtime;
    u64 i_ctime;
    u64 i_btime;
    u64 i_flags;
    u64 i_generation;
    u32 i_win_attrs;
    u32 i_uid_high;
    u32 i_gid_high;
    u32 i_reserved1;
    u64 i_parent_ino;
    u8  i_name[XXFS_MAX_NAME + 1];
    union {
        u8  i_inline[XXFS_INLINE_MAX];
        struct {
            u64 i_extent_off;
            u32 i_extent_len;
            u32 i_reserved2;
        };
        struct {
            u64 i_extents_off;
            u32 i_extents_count;
            u32 i_reserved3;
        };
    };
    u32 i_checksum;
    u8  i_reserved4[4];
};

struct xxfs_extent {
    u64 e_off;
    u32 e_len;
    u32 e_checksum;
};

struct xxfs_block_header {
    u32 bh_magic;
    u32 bh_checksum;
    u64 bh_block_id;
    u64 bh_seq;
    u16 bh_type;
    u16 bh_lrc_group;
    u8  bh_lrc_index;
    u8  bh_flags;
    u8  bh_reserved[32];
};

struct xxfs_wal_entry {
    u32 we_magic;
    u32 we_len;
    u64 we_seq;
    u64 we_tx_id;
    u16 we_type;
    u16 we_flags;
    u32 we_header_crc;
    u8  we_data[];
};

struct xxfs_wal_footer {
    u32 wf_data_crc;
    u32 wf_footer_crc;
    u32 wf_magic;
};

#define XXFS_WE_BEGIN    1
#define XXFS_WE_INDEX    2
#define XXFS_WE_COMMIT   3
#define XXFS_WE_MAGIC    0xXX574500U
#define XXFS_WF_MAGIC    0xXX574600U

struct xxfs_pp_header {
    u32 pp_magic;
    u32 pp_block_size;
    u32 pp_total_blocks;
    u32 pp_dirty_count;
    u64 pp_seq;
    u64 pp_min_seq;
    u32 pp_checksum;
    u8  pp_reserved[4068];
};

struct xxfs_pp_slot {
    u64 ps_logical_block;
    u32 ps_offset;
    u32 ps_len;
    u64 ps_ino;
    u32 ps_flags;
    u32 ps_checksum;
};

struct xxfs_lrc_group {
    u32 lg_magic;
    u32 lg_group_id;
    u64 lg_data_blocks[XXFS_LRC_DATA];
    u64 lg_local_blocks[XXFS_LRC_LOCAL];
    u64 lg_global_blocks[XXFS_LRC_GLOBAL];
    u32 lg_checksum;
    u8  lg_reserved[3976];
};

struct xxfs_dir_entry {
    u64 d_ino;
    u8  d_type;
    u8  d_name_len;
    u8  d_reserved[6];
};

struct xxfs_dir_child {
    u64 dc_hash;
    u8  dc_name[XXFS_MAX_NAME + 1];
    u8  dc_type;
};

struct xxfs_readdir_ctx {
    struct xxfs_dir_child *entries;
    u32 count;
    u32 cap;
};

struct xxfs;

struct xxfs_os_file {
    void *opaque[4];
};

struct xxfs_os_lock {
    void *opaque[4];
};

void  *xxfs_os_alloc(size_t size);
void  *xxfs_os_zalloc(size_t size);
void   xxfs_os_free(void *ptr);
void  *xxfs_os_memcpy(void *dst, const void *src, size_t n);
int    xxfs_os_memcmp(const void *a, const void *b, size_t n);
void  *xxfs_os_memset(void *dst, int c, size_t n);

int    xxfs_os_file_open(struct xxfs_os_file *f, const char *path, int flags);
void   xxfs_os_file_close(struct xxfs_os_file *f);
s64    xxfs_os_file_size(struct xxfs_os_file *f);
int    xxfs_os_file_pwrite(struct xxfs_os_file *f, const void *buf, size_t len, s64 off);
int    xxfs_os_file_pread(struct xxfs_os_file *f, void *buf, size_t len, s64 off);
int    xxfs_os_file_sync(struct xxfs_os_file *f);
int    xxfs_os_file_truncate(struct xxfs_os_file *f, s64 size);
int    xxfs_os_file_extend(struct xxfs_os_file *f, s64 size);
void  *xxfs_os_file_mmap(struct xxfs_os_file *f, size_t len);
void   xxfs_os_file_munmap(void *ptr, size_t len);
int    xxfs_os_file_msync(void *ptr, size_t len);
int    xxfs_os_mkdir(const char *path, u32 mode);
int    xxfs_os_rmdir(const char *path);

void   xxfs_os_lock_init(struct xxfs_os_lock *l);
void   xxfs_os_lock_destroy(struct xxfs_os_lock *l);
void   xxfs_os_read_lock(struct xxfs_os_lock *l);
void   xxfs_os_read_unlock(struct xxfs_os_lock *l);
void   xxfs_os_write_lock(struct xxfs_os_lock *l);
void   xxfs_os_write_unlock(struct xxfs_os_lock *l);

u64    xxfs_os_time(void);
u32    xxfs_os_crc32c(const void *data, size_t len);

struct xxfs *xxfs_mount(const char *path, u64 flags);
void         xxfs_umount(struct xxfs *fs);

int  xxfs_create(struct xxfs *fs, const char *path, u16 mode, u16 uid, u16 gid);
int  xxfs_mkdir(struct xxfs *fs, const char *path, u16 mode, u16 uid, u16 gid);
int  xxfs_unlink(struct xxfs *fs, const char *path);
int  xxfs_rmdir(struct xxfs *fs, const char *path);
int  xxfs_rename(struct xxfs *fs, const char *old_path, const char *new_path);
int  xxfs_symlink(struct xxfs *fs, const char *target, const char *linkpath);
int  xxfs_readlink(struct xxfs *fs, const char *path, char *buf, u32 *len);
int  xxfs_chmod(struct xxfs *fs, const char *path, u16 mode);
int  xxfs_chown(struct xxfs *fs, const char *path, u16 uid, u16 gid);
int  xxfs_utime(struct xxfs *fs, const char *path, u64 atime, u64 mtime);

int  xxfs_write(struct xxfs *fs, const char *path, const void *buf, u64 off, u32 len, u32 *written);
int  xxfs_read(struct xxfs *fs, const char *path, void *buf, u64 off, u32 len, u32 *read_bytes);

int  xxfs_stat(struct xxfs *fs, const char *path, struct xxfs_inode *out);
int  xxfs_readdir(struct xxfs *fs, const char *path, struct xxfs_readdir_ctx *ctx);
void xxfs_readdir_free(struct xxfs_readdir_ctx *ctx);
u64  xxfs_count(const struct xxfs *fs);
int  xxfs_sync(struct xxfs *fs);

struct xxfs_fs_info {
    u32 version;
    u32 block_size;
    u64 block_count;
    u64 free_blocks;
    u64 inodes_count;
    u64 cow_generation;
};

int  xxfs_info(const struct xxfs *fs, struct xxfs_fs_info *info);

#ifdef XXFS_PROFILE
struct xxfs_profile {
    u64 hash_ns;
    u64 icache_ns;
    u64 pcache_ns;
    u64 pwrite_ns;
    u64 pread_ns;
    u64 alloc_ns;
    u64 dcache_ns;
    u32 create_cnt;
    u32 write_cnt;
};
int  xxfs_get_profile(struct xxfs *fs, struct xxfs_profile *prof);
#endif

int  xxfs_defrag_file(struct xxfs *fs, const char *path);
int  xxfs_defrag_all(struct xxfs *fs);
int  xxfs_defrag_index(struct xxfs *fs);

int  xxfs_snapshot_create(struct xxfs *fs, const char *name);
int  xxfs_snapshot_rollback(struct xxfs *fs, const char *name);

int  xxfs_lrc_verify(struct xxfs *fs, u32 group_id);
int  xxfs_lrc_repair(struct xxfs *fs, u32 group_id, u32 index);

int  xxfs_fsck(const char *path, int repair);
int  xxfs_fsck_check_super(const char *path);
int  xxfs_fsck_check_index(const char *path);

int  xxfs_mkfs(const char *path, u64 size_mb, u32 flags);

#endif
