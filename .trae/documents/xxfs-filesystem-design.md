# XXFS 文件系统设计方案 v3

## 终极架构: CoW + 环形WAL + Ping-Pong Cache + LRC + 碎片整理

## 1. 设计目标

| 目标    | 手段                               |
| ----- | -------------------------------- |
| 零碎片   | Ping-Pong Cache 合并小写，磁盘只接收连续大块   |
| 零随机IO | 环形WAL顺序写，Extent连续读，Ping-Pong合并刷盘 |
| 最低写放大 | \~1.25x (环形WAL + 批量合并)           |
| 崩溃安全  | 环形WAL + CoW + CRC32C防半写          |
| 数据校验  | CRC32C检错 + LRC纠错 (索引区) + 可选数据校验  |
| 防半写   | 原子块头 + CRC + 环形WAL               |
| 数据连续  | Ping-Pong合并 + 碎片整理器              |
| 碎片整理  | 极致整理：全Extent重排 + 索引重建            |

## 2. 三大核心组件

```
┌─────────────────────────────────────────────────────┐
│                   用户写入请求                        │
│              (create/write/mkdir/unlink)             │
└────────────────────┬────────────────────────────────┘
                     ▼
┌─────────────────────────────────────────────────────┐
│            Ping-Pong Cache (双缓冲)                   │
│  ┌──────────────┐    ┌──────────────┐               │
│  │  Ping (前台)  │ ←→ │ Pong (后台)   │               │
│  │  接收小写     │    │  合并+刷盘    │               │
│  └──────────────┘    └──────────────┘               │
│  大小: 1MB~4MB, 粒度: 256~1024 块                    │
└────────────────────┬────────────────────────────────┘
                     │ (前台满 → 切换角色)
                     ▼
┌─────────────────────────────────────────────────────┐
│            环形 WAL (Ring Buffer)                     │
│  ┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐             │
│  │H │  │  │  │  │  │  │  │  │  │T │  │             │
│  └──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘             │
│  H=head(写), T=tail(提交), 固定4MB~16MB             │
│  纯顺序追加写，循环复用，零碎片                       │
└────────────────────┬────────────────────────────────┘
                     │ (后台 checkpoint)
                     ▼
┌─────────────────────────────────────────────────────┐
│            磁盘: CoW + 大块 Extent                    │
│  只刷连续大块，永远不写零散4KB                        │
│  ┌─────────────────┐  ┌─────────────────┐           │
│  │ Extent A (1MB)   │  │ Extent B (2MB)   │           │
│  │ [D0 D1 D2 ...]  │  │ [D0 D1 D2 ...]  │           │
│  └─────────────────┘  └─────────────────┘           │
│  旧块延迟回收，物理布局永远连续                       │
└─────────────────────────────────────────────────────┘
```

## 3. 环形 WAL (Ring Buffer)

### 3.1 数据结构

```c
struct xxfs_wal_ring {
    u32 wr_magic;             // 0xXX57414C ('XXWAL')
    u32 wr_version;           // 1
    u32 wr_block_size;        // 4096
    u32 wr_total_blocks;      // 环形总块数 (1024 = 4MB)
    u64 wr_head;              // 下一个写入位置 (逻辑偏移, mod total)
    u64 wr_tail;              // 已checkpoint位置 (逻辑偏移, mod total)
    u64 wr_seq;               // 单调递增序列号
    u64 wr_commit_seq;        // 最新已提交序列号
    u32 wr_checksum;          // CRC32C
    u8  wr_reserved[4056];    // 对齐到 4KB
};

struct xxfs_wal_entry {
    u32 we_magic;             // 0xXX574500 ('XXWE')
    u32 we_len;               // 数据长度
    u64 we_seq;               // 序列号
    u64 we_tx_id;             // 事务 ID
    u16 we_type;              // TX_BEGIN=1, COW_PTR=2, TX_COMMIT=3
    u16 we_flags;             // 标志
    u32 we_checksum;          // CRC32C (防半写)
    u8  we_data[];            // 变长数据
};
```

### 3.2 工作规则

```
写入: head 顺序追加，写满一圈回到开头
提交: tail 跟进，标记已 checkpoint 的区域可复用
空间不足: 触发后台 checkpoint (刷数据到磁盘，释放日志)
崩溃恢复: 重放 (head - tail) 区间即可，秒级完成

环形复用:
  ┌──────────────────────────────────┐
  │  [已提交可复用] [活跃日志] [空闲] │
  │  ^tail          ^head            │
  └──────────────────────────────────┘
  head 追上 tail → 触发 checkpoint 释放空间
```

### 3.3 防半写机制

```c
struct xxfs_wal_entry_header {
    u32 weh_magic;            // 写入前先写 magic
    u32 weh_len;
    u64 weh_seq;
    u64 weh_tx_id;
    u16 weh_type;
    u16 weh_flags;
    u32 weh_header_crc;       // 头部 CRC (不含数据)
};

struct xxfs_wal_entry_footer {
    u32 wef_data_crc;         // 数据 CRC
    u32 wef_footer_crc;       // 尾部 CRC (含 wef_data_crc)
    u32 wef_magic;            // 0xXX574600 ('XXWF') 写入完成标记
};
```

**半写检测**:

1. 读 entry: 先验证 header CRC → 再验证 data CRC → 再验证 footer CRC + magic
2. 任何一步失败 = 半写，截断到上一个有效 entry
3. footer magic 不存在 = 写入未完成，丢弃

## 4. Ping-Pong Cache (双缓冲)

### 4.1 数据结构

```c
#define PP_BUF_BLOCKS  256    // 1MB = 256 × 4KB
#define PP_BUF_SIZE    (PP_BUF_BLOCKS * 4096)

struct xxfs_pp_buf {
    u8  *data;                // 连续 1MB 缓冲区
    u64  block_map[PP_BUF_BLOCKS]; // 逻辑块号 → 缓冲区偏移映射
    u32  dirty_count;         // 脏块数
    u32  seq_base;            // 起始序列号
    u8   is_active;           // 1=前台, 0=后台
    u8   is_flushing;         // 1=正在刷盘
};

struct xxfs_pp_cache {
    struct xxfs_pp_buf ping;  // 前台缓冲
    struct xxfs_pp_buf pong;  // 后台缓冲
    u32 flush_threshold;      // 刷盘阈值 (脏块数)
    u64 flush_count;          // 刷盘次数统计
};
```

### 4.2 工作流程

```
初始状态:
  Ping = 前台 (接收写入)
  Pong = 后台 (空闲)

Step 1: 用户写入进入 Ping 缓冲
  write(inode=5, offset=0, data="hello")
  → Ping.data[offset] = "hello"
  → Ping.dirty_count++
  → Ping.block_map[slot] = logical_block_5

Step 2: Ping 脏块达到阈值 (如 256 块 = 1MB)
  → 触发角色切换:
     Ping 变后台 (开始刷盘)
     Pong 变前台 (接收新写入)

Step 3: 后台线程合并 Ping 缓冲
  → 将所有脏块按逻辑块号排序
  → 分配 1 个连续 Extent (1MB)
  → 一次性顺序写入磁盘 (CoW 新块)
  → 更新 DPHash 索引指针
  → 写入环形 WAL (COW_PTR 记录)
  → 旧块标记延迟回收

Step 4: Ping 刷盘完成
  → Ping 清空，等待下次切换
```

### 4.3 为什么消灭碎片

| 传统 CoW               | XXFS Ping-Pong CoW     |
| -------------------- | ---------------------- |
| 每次修改写 1 个 4KB 块      | 256 个修改合并成 1MB 连续块     |
| 1000 次小写 = 1000 个零散块 | 1000 次小写 = 4 个连续 1MB 块 |
| 读取时 1000 次随机 IO      | 读取时 4 次顺序 IO           |
| 严重碎片                 | **零碎片**                |

## 5. 磁盘布局

```
┌──────────────────────────────────────────────┐
│ Block 0: Superblock (4KB, COW root)          │
├──────────────────────────────────────────────┤
│ Block 1: WAL Ring Header (4KB)               │
├──────────────────────────────────────────────┤
│ Block 2 ~ 2+N: WAL Ring Area (4~16MB)        │
│   固定大小，循环复用，纯顺序写                 │
├──────────────────────────────────────────────┤
│ Block N+1: BG Descriptor Table               │
├──────────────────────────────────────────────┤
│ Block N+2 ~ : Block Bitmaps (per BG)         │
├──────────────────────────────────────────────┤
│ Block M+1: LRC Group Table                   │
├──────────────────────────────────────────────┤
│ Block M+2 ~ : LRC Parity Blocks              │
├──────────────────────────────────────────────┤
│ ┌─ COW Gen 0 ──────────────────────────────┐ │
│ │  Dir Index (DPHash Paged, 连续 Extent)    │ │
│ │  Inode Index (DPHash Paged, 连续 Extent)  │ │
│ │  Inode Data (连续 Extent)                 │ │
│ │  File Data (连续 Extent)                  │ │
│ └───────────────────────────────────────────┘ │
│ ┌─ COW Gen 1 (修改后) ────────────────────┐  │
│ │  新 Dir Index (连续 Extent, Ping-Pong合并)│  │
│ │  新 Inode Index (连续 Extent)            │  │
│ │  新 Inode Data (连续 Extent)             │  │
│ │  新 File Data (连续 Extent)              │  │
│ └───────────────────────────────────────────┘ │
│ ... (旧 Gen 延迟回收)                        │
└──────────────────────────────────────────────┘
```

### 5.1 Superblock

```c
struct xxfs_super {
    u32 s_magic;              // 0xXX5F5346
    u32 s_version;            // 3
    u32 s_block_size;         // 4096
    u32 s_block_size_bits;    // 12
    u64 s_block_count;        // 总块数
    u64 s_free_blocks;        // 空闲块数
    u64 s_inodes_count;       // 总 inode 数
    u64 s_free_inodes;        // 空闲 inode 数
    u64 s_root_ino;           // 根目录 inode 号
    // COW 根指针
    u64 s_cow_generation;     // 当前 COW 代数
    u64 s_dir_idx_root;       // 目录索引根块号
    u64 s_ino_idx_root;       // Inode 索引根块号
    // WAL Ring
    u64 s_wal_off;            // WAL 起始块号
    u32 s_wal_blocks;         // WAL 总块数
    u64 s_wal_seq;            // 最新 WAL 序列号
    // LRC
    u64 s_lrc_table_off;      // LRC 组表偏移
    u32 s_lrc_groups;         // LRC 组数
    // BG
    u32 s_bg_count;           // BG 数量
    u32 s_bg_blocks;          // 每 BG 块数 (8192)
    // Ping-Pong 配置
    u32 s_pp_buf_blocks;      // PP 缓冲区块数 (256=1MB)
    u32 s_pp_flush_threshold; // 刷盘阈值
    // 特性
    u32 s_feature_compat;
    u32 s_feature_ro_compat;
    u32 s_feature_incompat;
    u32 s_checksum;           // CRC32C
    u8  s_reserved[3824];
};
```

### 5.2 Inode (128 字节)

```c
struct xxfs_inode {
    u16 i_mode;
    u16 i_uid;
    u16 i_gid;
    u32 i_nlinks;
    u64 i_size;
    u64 i_blocks;
    u64 i_atime;
    u64 i_mtime;
    u64 i_ctime;
    u64 i_flags;
    u64 i_generation;         // COW 代数
    union {
        u8  i_inline[48];     // 内联数据 (≤48B)
        struct {
            u64 i_extent_off; // 单 Extent
            u32 i_extent_len;
            u32 i_reserved1;
        };
        struct {
            u64 i_extents_off;// 间接 Extent 表
            u32 i_extents_count;
            u16 i_reserved2;
            u8  i_symlink[22];// 短符号链接
        };
    };
    u32 i_checksum;           // CRC32C
    u8  i_reserved[12];
};
```

### 5.3 Extent (16 字节)

```c
struct xxfs_extent {
    u64 e_off;                // 起始块号
    u32 e_len;                // 长度(块数)
    u32 e_checksum;           // CRC32C (可选数据校验)
};
```

### 5.4 块头 (防半写)

```c
struct xxfs_block_header {
    u32 bh_magic;             // 块魔数
    u32 bh_checksum;          // CRC32C (数据校验)
    u64 bh_block_id;          // 逻辑块 ID
    u64 bh_seq;               // 写入序列号 (COW 版本)
    u16 bh_type;              // 块类型
    u16 bh_lrc_group;         // LRC 组 ID
    u8  bh_lrc_index;         // LRC 组内索引
    u8  bh_flags;             // 标志 (HAS_CHECKSUM, IS_LRC_PROTECTED, ...)
    u8  bh_reserved[32];
};
// 64 字节头 + 4032 字节数据 = 4096 字节块
```

## 6. 完整写入流程

```
用户调用 xxfs_write(inode, offset, data):

1. [Ping-Pong] 数据写入 Ping 缓冲
   - 如果 Ping 脏块 < 阈值: 直接返回 (内存操作, ~50ns)
   - 如果 Ping 脏块 >= 阈值: 触发切换

2. [切换] Ping ↔ Pong 角色互换
   - Pong 变前台 (接收新写入)
   - Ping 变后台 (准备刷盘)

3. [WAL] 后台线程:
   a. 将 Ping 缓冲脏块排序 (按逻辑块号)
   b. 写入环形 WAL:
      - TX_BEGIN (seq=N)
      - COW_PTR (old_block, new_block) × 脏块数
      - TX_COMMIT (seq=N)

4. [CoW 刷盘] 后台线程:
   a. 分配连续 Extent (1MB = 256 块)
   b. 将 Ping 缓冲数据顺序写入 Extent
   c. 每块写入 xxfs_block_header (含 CRC32C)
   d. 更新 DPHash 索引指针 (old → new)
   e. 旧块加入延迟回收链表

5. [LRC] 如果修改的是索引/Inode 块:
   a. 更新局部校验块 (XOR, 1 次 IO)
   b. 全局校验延迟批量更新

6. [完成] Ping 缓冲清空，等待下次切换
```

## 7. 读取流程

```
用户调用 xxfs_read(inode, offset, len):

1. [Ping-Pong] 查缓存
   - Ping 缓冲命中 → 直接返回 (~50ns)
   - Pong 缓冲命中 → 直接返回 (~50ns)

2. [DPHash] 缓存未命中
   a. 查 Inode 索引 → 获取 inode 数据
   b. 从 inode 获取 Extent 信息
   c. 从 Extent 连续读取数据块

3. [CRC 校验] 读取时验证
   - CRC 通过 → 返回数据
   - CRC 失败 → 进入 LRC 修复

4. [LRC 修复] (仅索引/Inode 块)
   a. 读取同组其他数据块 + 局部校验
   b. XOR 恢复损坏块
   c. 验证恢复后 CRC
   d. 写回修复块 (CoW 新位置)

5. 因为无碎片，读取全程顺序 IO
```

## 8. 碎片整理 (极致整理)

### 8.1 在线整理器

```c
struct xxfs_defrag {
    u64 df_src_inode;         // 待整理 inode
    u64 df_progress;          // 整理进度 (块偏移)
    u32 df_extents_before;    // 整理前 Extent 数
    u32 df_extents_after;     // 整理后 Extent 数
    u8  df_running;           // 是否运行中
};
```

### 8.2 整理策略

**文件级整理**:

1. 读取文件所有 Extent
2. 分配新的连续大 Extent
3. 将所有数据拷贝到新 Extent (顺序读写)
4. 原子更新 inode 指针 (CoW)
5. 旧 Extent 延迟回收

**全局整理** (极致):

1. 扫描所有 inode，收集所有 Extent
2. 按 inode 顺序重新排列
3. 分配全新的连续区域
4. 批量拷贝所有数据
5. 重建 DPHash 索引 (完全连续)
6. 原子切换 superblock 根指针
7. 旧区域整体回收

**索引整理**:

1. 读取 DPHash 所有目录页
2. 按哈希桶顺序重排
3. 写入新的连续 Extent
4. 重建目录索引 (完全连续)
5. 原子切换根指针

### 8.3 整理触发

| 触发条件           | 整理级别        |
| -------------- | ----------- |
| 单文件 Extent > 8 | 文件级         |
| 全局碎片率 > 15%    | 全局整理        |
| 空闲空间 < 10%     | 索引整理 + 全局整理 |
| 手动触发           | 可选级别        |

## 9. CRC + LRC 数据保护

### 9.1 分层校验策略

| 区域         | 校验方式                  | 理由        |
| ---------- | --------------------- | --------- |
| WAL 日志块    | CRC32C + footer magic | 防半写，快速检错  |
| 目录索引页      | CRC32C + LRC(6+2+2)   | 核心元数据，需纠错 |
| Inode 页    | CRC32C + LRC(6+2+2)   | 核心元数据，需纠错 |
| 文件数据块      | CRC32C (可选)           | 用户可选，默认关闭 |
| Extent 表   | CRC32C                | 指针数据，需检错  |
| Superblock | CRC32C                | 核心元数据     |

### 9.2 LRC 组 (6+2+2)

```
局部组 A: D0 D1 D2 PA    PA = D0⊕D1⊕D2
局部组 B: D3 D4 D5 PB    PB = D3⊕D4⊕D5
全局校验: GX GY          RS(6,2)

1块损坏: 读4块修复 (局部XOR)
2块损坏: 读7块修复 (RS解码)
写1块: 只更新1个局部校验 (50%写放大降低)
```

## 10. 实现步骤

### Phase 1: 用户态原型 (libxxfs)

```
lib/
├── xxfs.h            # 公共接口 + 所有磁盘数据结构
├── xxfs_super.c      # superblock 读写, COW 根指针管理
├── xxfs_inode.c      # inode 操作 (COW 写入)
├── xxfs_dir.c        # 目录操作 (DPHash + COW)
├── xxfs_alloc.c      # 块/inode 分配器 (BG 位图)
├── xxfs_data.c       # 数据读写 (Extent/Inline)
├── xxfs_path.c       # 路径解析
├── xxfs_cow.c        # COW 管理 (代数, 延迟回收)
├── xxfs_wal.c        # 环形 WAL (Ring Buffer)
├── xxfs_pp.c         # Ping-Pong Cache
├── xxfs_lrc.c        # LRC 编解码 + 修复
├── xxfs_crc.c        # CRC32C + 防半写
├── xxfs_defrag.c     # 碎片整理器
└── Makefile
```

1. **xxfs.h** - 所有数据结构定义
2. **xxfs\_crc.c** - CRC32C + 防半写校验
3. **xxfs\_wal.c** - 环形 WAL 实现
4. **xxfs\_cow\.c** - COW 块管理
5. **xxfs\_pp.c** - Ping-Pong Cache
6. **xxfs\_alloc.c** - 块分配器
7. **xxfs\_inode.c** - Inode 操作
8. **xxfs\_dir.c** - 目录操作 (DPHash)
9. **xxfs\_data.c** - 数据读写
10. **xxfs\_lrc.c** - LRC 编解码
11. **xxfs\_defrag.c** - 碎片整理
12. **xxfs\_path.c** - 路径解析
13. **mkfs.c** - 格式化工具
14. **test/** - 正确性 + 性能测试

### Phase 2: FUSE

1. **xxfs\_fuse.c** - FUSE 用户态文件系统

### Phase 3: 内核模块

1. **kernel/** - Linux VFS 模块

## 11. 性能预期

| 操作           | 预期延迟         | 原因                     |
| ------------ | ------------ | ---------------------- |
| 小文件写入        | \~50ns       | Ping-Pong 内存操作         |
| 大文件写入        | \~1μs/MB     | 顺序刷盘                   |
| 文件读取 (缓存命中)  | \~50ns       | Ping-Pong 缓存           |
| 文件读取 (缓存未命中) | \~1μs/Extent | 顺序 IO                  |
| 路径解析         | \~200ns      | 3×DPHash O(1)          |
| 目录创建         | \~100ns      | DPHash put + Ping-Pong |
| 崩溃恢复         | <1s          | 环形 WAL 重放              |
| LRC 修复 (1块)  | \~4μs        | 4 块 IO                 |
| 碎片整理         | \~1ms/GB     | 顺序拷贝                   |

### 写放大分析

| 操作       | 写放大   | 原因                       |
| -------- | ----- | ------------------------ |
| 小文件写入    | 1.0x  | Ping-Pong 合并，无额外写        |
| 元数据修改    | 1.25x | WAL + CoW (1次WAL + 1次数据) |
| LRC 索引更新 | 1.42x | 额外 1 局部校验更新              |
| 全局整理     | 1.0x  | 纯拷贝，无额外写                 |

## 12. 对比

| 特性    | XXFS              | Btrfs  | ZFS   | ext4     |
| ----- | ----------------- | ------ | ----- | -------- |
| 磁盘碎片  | **0**             | 严重     | 中等    | 少        |
| 读取模式  | **纯顺序**           | 随机     | 随机    | 顺序       |
| 写放大   | **\~1.25x**       | 3\~10x | 2\~5x | \~1.1x   |
| 元数据写  | **环形顺序**          | 离散随机   | 离散随机  | 日志顺序     |
| 小文件合并 | **Ping-Pong**     | 不合并    | 不合并   | 不合并      |
| 崩溃恢复  | **秒级**            | 慢      | 慢     | 快        |
| 数据校验  | CRC+LRC           | CRC    | CRC   | 无        |
| 防半写   | **header+footer** | 无      | 无     | journal  |
| 碎片整理  | **极致整理**          | 在线     | 无     | e4defrag |
| 快照    | **COW 天然**        | COW    | COW   | 无        |

