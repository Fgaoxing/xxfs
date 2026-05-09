# XXFS

**eXtensible eXperimental Filesystem** — 基于 Disk-Paged Hash 的高性能用户态文件系统

## 核心技术：Disk-Paged Hash

XXFS 使用 **Disk-Paged Hash** 作为主索引结构，专为磁盘存储优化：

- **页对齐存储** — 每个哈希桶是一个 4K 页面，与磁盘块对齐
- **Inline Data** — 小文件（≤224字节）直接存储在索引页内，零额外 IO
- **可扩展哈希** — Extendible Hashing，支持原地扩展，无需全量 rehash
- **Swiss Table 风格缓存** — icache 使用 ctrl byte 快速过滤，内存占用降低 65%

## 特性

- **零系统调用读写** — 用户态直接操作磁盘镜像，无需 VFS 路径
- **Inline Data** — 小文件直接存储在索引页内，stat 命中 0.27 us
- **三级缓存架构**：
  - `icache` — Swiss Table 风格，ctrl byte 快速过滤，只用 hash 匹配
  - `pcache` — 索引页缓存，4096 槽直接映射 + dirty 延迟写
  - `dcache` — 目录缓存，写回策略
- **原地扩展** — 无需全量拷贝或 rehash
- **POSIX 风格 API** — create/open/read/write/unlink/mkdir/sync

## 性能

| 操作 | XXFS | ext4 | btrfs |
|------|------|------|-------|
| create | **3.66 us** | 7.58 us | 9.46 us |
| stat | **0.27 us** | 1.01 us | 0.80 us |
| stat (random) | **0.27 us** | 0.97 us | 0.86 us |
| read small | **0.30 us** | 3.51 us | 3.18 us |
| read (random) | **0.25 us** | 2.90 us | 1.92 us |
| mkdir | **5.83 us** | 14.58 us | 8.64 us |
| unlink | **7.21 us** | 7.36 us | 10.71 us |

*测试环境：10000 个文件，256MB 镜像，tmpfs 底层存储*

**关键优势**：

- **create** 比 ext4 快 **2.1x**，比 btrfs 快 **2.6x**
- **stat/read** 比所有内核文件系统快 **3.0-13.8x**
- **mkdir** 比 ext4 快 **2.5x**，比 btrfs 快 **1.5x**

## 架构

```
┌─────────────────────────────────────────────┐
│                 Application                  │
├─────────────────────────────────────────────┤
│  icache (Swiss Table)  │  pcache (4096)     │
│  ctrl byte + hash only │  dirty writeback   │
│  153 bytes/entry       │  direct mapped     │
├─────────────────────────────────────────────┤
│           xxfs_lib (xxfs.c)                 │
│  ┌─────────────────────────────────────────┐│
│  │         Disk-Paged Hash                 ││
│  │  ┌──────────┐  ┌──────────────────────┐ ││
│  │  │ xxh64    │  │ Extendible Directory │ ││
│  │  │ Hash     │  │ (256K entries max)   │ ││
│  │  └──────────┘  └──────────────────────┘ ││
│  │  ┌──────────────────────────────────────┐│
│  │  │ Page Slots (56 per 4K page)         ││
│  │  │ + Inline Area (1344 bytes)          ││
│  │  └──────────────────────────────────────┘│
│  └─────────────────────────────────────────┘│
├─────────────────────────────────────────────┤
│         OS Abstraction (xxfs_os.c)          │
├─────────────────────────────────────────────┤
│              Disk Image (.img)              │
└─────────────────────────────────────────────┘
```

### Disk-Paged Hash 结构

每个 4K 页面包含：

- **Page Header** (16 bytes): count, local_depth, inline_used, overflow_next
- **Slots** (56 × 48 bytes): hash, key_prefix, file_off, klen, vlen, flags
- **Inline Area** (1344 bytes): 小 key-value 直接存储

```
+──────────────────────────────────────────+
│ Page Header (16B)                        │
├──────────────────────────────────────────┤
│ Slot[0]  │ hash(8) │ prefix(8) │ off(8)  │
│          │ klen(4) │ vlen(4)   │ flags(1)│
├──────────────────────────────────────────┤
│ Slot[1] ... Slot[55]                     │
├──────────────────────────────────────────┤
│ Inline Area (1344B)                      │
│ [key0][val0][key1][val1]...              │
└──────────────────────────────────────────┘
```

### Swiss Table 风格 icache

```
icache_entry {
    u64 hash;        // xxh64 哈希值
    u8  ctrl;        // 控制字节: 0x00=空, 0x80+低7位=有效
    inode inode;     // 144 字节 inode 数据
}

查找流程:
1. 计算 ctrl = (hash & 0x7F) | 0x80
2. 直接索引: idx = hash & mask
3. 探测最多 16 次:
   - if (e->ctrl == EMPTY) return NOT_FOUND
   - if (e->ctrl == ctrl && e->hash == hash) return FOUND
   - idx = (idx + 1) & mask
```

**内存节省**：

- 旧结构：432 字节/entry (hash + klen + key_prefix + key[256] + inode + lru_tick + valid)
- 新结构：153 字节/entry (hash + ctrl + inode)
- **节省 65% 内存**

### 磁盘布局

```
+───────────────────+
│   Super Block     │  Block 0
├───────────────────+
│  Bitmap + Alloc   │  Blocks 1-N
├───────────────────+
│  Extensible Hash  │
│    Directory      │  256K entries max
├───────────────────+
│   Data Pages      │  4K pages, inline area
├───────────────────+
│   File Extents    │  Large file data
└───────────────────┘
```

## 构建

### 依赖

- [xmake](https://xmake.io/) 或 [CMake](https://cmake.org/)
- readline（可选，用于交互式 shell）

### xmake

```bash
xmake f --readline=y
xmake -j4
```

### CMake

```bash
mkdir build && cd build
cmake .. -DUSE_READLINE=ON
make -j4
```

## 工具链

| 工具 | 说明 |
|------|------|
| `mkfs.xxfs` | 格式化磁盘镜像 |
| `fsck.xxfs` | 文件系统检查与修复 |
| `xxfs-shell` | 交互式 shell（支持方向键历史） |
| `xxfs-bench` | 性能基准测试 |
| `posix-bench` | POSIX 文件系统基准测试（对比 ext4/btrfs） |

## 使用

### 创建并格式化

```bash
dd if=/dev/zero of=myfs.img bs=1M count=256
mkfs.xxfs myfs.img
```

### 交互式 Shell

```bash
./xxfs-shell myfs.img
> ls
> mkdir /test
> echo "hello" > /test/file.txt
> cat /test/file.txt
> sync
```

### Benchmark

```bash
# XXFS benchmark
./xxfs-bench -n 10000 -s 256

# 对比多种文件系统 (ext4, btrfs, XFS, FAT32, exFAT, ZFS)
./bench/compare.sh 10000
```

支持的对比文件系统：
- **ext4** — Linux默认文件系统
- **btrfs** — CoW文件系统
- **XFS** — 高性能文件系统
- **FAT32** — 简单的兼容性格式
- **exFAT** — 优化的闪存文件系统
- **ZFS** — 企业级文件系统

脚本会自动检测系统上可用的文件系统工具。

## API

```c
struct xxfs *xxfs_mount(const char *path, u64 flags);
void xxfs_umount(struct xxfs *fs);

int xxfs_create(struct xxfs *fs, const char *path, u16 mode, u16 uid, u16 gid);
int xxfs_open(struct xxfs *fs, const char *path, u32 flags, struct xxfs_file **fp);
ssize_t xxfs_read(struct xxfs *fs, struct xxfs_file *fp, void *buf, size_t count);
ssize_t xxfs_write(struct xxfs *fs, struct xxfs_file *fp, const void *buf, size_t count);
int xxfs_close(struct xxfs *fs, struct xxfs_file *fp);
int xxfs_unlink(struct xxfs *fs, const char *path);
int xxfs_mkdir(struct xxfs *fs, const char *path, u16 mode, u16 uid, u16 gid);
int xxfs_rmdir(struct xxfs *fs, const char *path);
int xxfs_rename(struct xxfs *fs, const char *oldpath, const char *newpath);
int xxfs_stat(struct xxfs *fs, const char *path, struct xxfs_inode *out);
int xxfs_sync(struct xxfs *fs);
```

## 测试

```bash
xmake run test-basic
```

## 许可证

MIT
