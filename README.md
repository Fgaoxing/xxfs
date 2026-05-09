# XXFS

**eXtensible eXperimental Filesystem** — 基于 Disk-Paged Hash 的高性能用户态文件系统

## 核心技术：Disk-Paged Hash

XXFS 使用 **Disk-Paged Hash** 作为主索引结构，专为磁盘存储优化：

- **页对齐存储** — 每个哈希桶是一个 4K 页面，与磁盘块对齐
- **Inline Data** — 小文件（≤224字节）直接存储在索引页内，零额外 IO
- **可扩展哈希** — Extendible Hashing，支持原地扩展，无需全量 rehash
- **Dirty Page 写回** — 修改延迟写回，减少磁盘 IO 次数

## 特性

- **零系统调用读写** — 用户态直接操作磁盘镜像，无需 VFS 路径
- **Inline Data** — 小文件直接存储在索引页内，stat 命中 0.35 us
- **三级缓存架构**：
  - `icache` — inode 缓存，open addressing 哈希表
  - `pcache` — 索引页缓存，4096 槽直接映射 + dirty 延迟写
  - `dcache` — 目录缓存，写回策略
- **原地扩展** — 无需全量拷贝或 rehash
- **POSIX 风格 API** — create/open/read/write/unlink/mkdir/sync

## 性能

| 操作 | XXFS | ext4 | btrfs | XFS | NTFS-3G (FUSE) |
|------|------|------|-------|-----|----------------|
| create | 18.50 us | 9.63 us | 14.55 us | 7.77 us | 52.53 us |
| stat | **0.35 us** | 1.11 us | 1.12 us | 0.72 us | 11.00 us |
| read small | **0.37 us** | 2.88 us | 3.02 us | 2.33 us | 32.23 us |
| unlink | **6.89 us** | 9.13 us | 12.88 us | 16.85 us | 17.35 us |

*测试环境：10000 个文件，256MB 镜像，tmpfs 底层存储*

**关键优势**：
- stat/read 操作比所有内核文件系统快 **2-7x**
- unlink 操作比所有对比文件系统快 **1.3-2.5x**
- 比 FUSE 文件系统（NTFS-3G）快 **3-87x**

## 架构

```
┌─────────────────────────────────────────────┐
│                 Application                  │
├─────────────────────────────────────────────┤
│  icache  │  pcache (4096 slots, direct map) │
│  dcache  │  dirty page writeback            │
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

# 对比 ext4/btrfs
./bench/compare.sh 10000
```

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
