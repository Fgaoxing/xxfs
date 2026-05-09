# XXFS

**eXtensible eXperimental Filesystem** — 一款高性能用户态哈希索引文件系统

## 特性

- **零系统调用读写** — 用户态直接操作磁盘镜像，无需 VFS 路径
- **Inline Data** — 小文件（≤224字节）直接存储在索引页内，零额外 IO
- **Hash 索引** — 可扩展哈希（Extendible Hashing），点查 O(1)
- **内存缓存** — 三级缓存架构：
  - `icache` — inode 缓存，stat 命中 0.35 us
  - `pcache` — 索引页缓存，4096 槽直接映射 + dirty 延迟写
  - `dcache` — 目录缓存，写回策略
- **原地扩展** — 无需全量拷贝或 rehash
- **POSIX 风格 API** — create/open/read/write/unlink/mkdir/sync

## 性能

| 操作 | XXFS | ext4 | btrfs | XFS |
|------|------|------|-------|-----|
| create | 18.50 us | 9.63 us | 14.55 us | 7.77 us |
| stat | **0.35 us** | 1.11 us | 1.12 us | 0.72 us |
| read small | **0.37 us** | 2.88 us | 3.02 us | 2.33 us |
| unlink | **6.89 us** | 9.13 us | 12.88 us | 16.85 us |

*测试环境：10000 个文件，256MB 镜像，tmpfs 底层存储*

## 架构

```
┌─────────────────────────────────────────────┐
│                 Application                  │
├─────────────────────────────────────────────┤
│  icache  │  pcache (4096 slots, direct map) │
│  dcache  │  dirty page writeback            │
├─────────────────────────────────────────────┤
│           xxfs_lib (xxfs.c)                 │
│  ┌─────────┐  ┌──────────────┐  ┌────────┐ │
│  │ Hash    │  │ Extendible   │  │  I/O   │ │
│  │ (xxh64) │  │ Directory    │  │ Layer  │ │
│  └─────────┘  └──────────────┘  └────────┘ │
├─────────────────────────────────────────────┤
│         OS Abstraction (xxfs_os.c)          │
├─────────────────────────────────────────────┤
│              Disk Image (.img)              │
└─────────────────────────────────────────────┘
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
