#!/bin/bash
set -e

NFILES=${1:-10000}
IMG_SIZE=256
IMG_DIR=/tmp/xxfs_bench_compare
XXFS_ROOT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS_DIR=/tmp/xxfs_bench_results

mkdir -p "$IMG_DIR" "$RESULTS_DIR"

XXFS_BENCH="$XXFS_ROOT/build/linux/x86_64/release/xxfs-bench"
POSIX_BENCH="$XXFS_ROOT/build/linux/x86_64/release/posix-bench"
MKFS_XXFS="$XXFS_ROOT/build/linux/x86_64/release/mkfs.xxfs"

if [ ! -x "$XXFS_BENCH" ]; then
    echo "Building xxfs first..."
    cd "$XXFS_ROOT" && xmake f -c --readline=y && xmake -j4
fi

echo "============================================================"
echo "  XXFS vs Multiple Filesystems  —  Benchmark Comparison"
echo "  files: $NFILES   image: ${IMG_SIZE}MB"
echo ""
echo "  NOTE: XXFS runs in userspace (direct img I/O, no VFS)."
echo "        Others run in kernel (VFS + syscall overhead)."
echo "        This is NOT an apples-to-apples comparison."
echo "        XXFS advantage: no syscall/VFS per operation."
echo "        Others advantage: kernel page cache, scheduler."
echo "============================================================"
echo ""

FS_LIST=""

# ---- XXFS ----
echo ">>> [1/N] XXFS Benchmark (userspace, direct img) <<<"
echo ""
$XXFS_BENCH -n $NFILES -o "$IMG_DIR/xxfs.img" -s $IMG_SIZE 2>&1 | tee "$RESULTS_DIR/xxfs.txt"
FS_LIST="$FS_LIST xxfs"
echo ""

# ---- ext4 ----
echo ">>> [2/N] ext4 Benchmark (kernel, VFS) <<<"
echo ""

EXT4_IMG="$IMG_DIR/ext4.img"
EXT4_MNT="$IMG_DIR/mnt_ext4"

if command -v mkfs.ext4 >/dev/null 2>&1; then
    dd if=/dev/zero of="$EXT4_IMG" bs=1M count=$IMG_SIZE 2>/dev/null
    mkfs.ext4 -F -q "$EXT4_IMG" 2>/dev/null

    mkdir -p "$EXT4_MNT"
    sudo mount -o loop "$EXT4_IMG" "$EXT4_MNT"
    sudo chown $(id -u):$(id -g) "$EXT4_MNT"

    $POSIX_BENCH "$EXT4_MNT" -n $NFILES 2>&1 | tee "$RESULTS_DIR/ext4.txt"
    FS_LIST="$FS_LIST ext4"

    sudo umount "$EXT4_MNT"
    rmdir "$EXT4_MNT"
else
    echo "mkfs.ext4 not available, skipping"
fi
echo ""

# ---- btrfs ----
echo ">>> [3/N] btrfs Benchmark (kernel, VFS, CoW) <<<"
echo ""

BTRFS_IMG="$IMG_DIR/btrfs.img"
BTRFS_MNT="$IMG_DIR/mnt_btrfs"

if command -v mkfs.btrfs >/dev/null 2>&1; then
    dd if=/dev/zero of="$BTRFS_IMG" bs=1M count=$IMG_SIZE 2>/dev/null
    mkfs.btrfs -f -q "$BTRFS_IMG" 2>/dev/null

    mkdir -p "$BTRFS_MNT"
    sudo mount -o loop "$BTRFS_IMG" "$BTRFS_MNT"
    sudo chown $(id -u):$(id -g) "$BTRFS_MNT"

    $POSIX_BENCH "$BTRFS_MNT" -n $NFILES 2>&1 | tee "$RESULTS_DIR/btrfs.txt"
    FS_LIST="$FS_LIST btrfs"

    sudo umount "$BTRFS_MNT"
    rmdir "$BTRFS_MNT"
else
    echo "mkfs.btrfs not available, skipping"
fi
echo ""

# ---- xfs ----
echo ">>> [4/N] XFS Benchmark (kernel, VFS, high performance) <<<"
echo ""

XFS_IMG="$IMG_DIR/xfs.img"
XFS_MNT="$IMG_DIR/mnt_xfs"

if command -v mkfs.xfs >/dev/null 2>&1; then
    dd if=/dev/zero of="$XFS_IMG" bs=1M count=$IMG_SIZE 2>/dev/null
    mkfs.xfs -f -q "$XFS_IMG" 2>/dev/null

    mkdir -p "$XFS_MNT"
    sudo mount -o loop "$XFS_IMG" "$XFS_MNT"
    sudo chown $(id -u):$(id -g) "$XFS_MNT"

    $POSIX_BENCH "$XFS_MNT" -n $NFILES 2>&1 | tee "$RESULTS_DIR/xfs.txt"
    FS_LIST="$FS_LIST xfs"

    sudo umount "$XFS_MNT"
    rmdir "$XFS_MNT"
else
    echo "mkfs.xfs not available, skipping"
fi
echo ""

# ---- fat32 ----
echo ">>> [5/N] FAT32 Benchmark (kernel, VFS, simple FS) <<<"
echo ""

FAT32_IMG="$IMG_DIR/fat32.img"
FAT32_MNT="$IMG_DIR/mnt_fat32"

if command -v mkfs.vfat >/dev/null 2>&1; then
    dd if=/dev/zero of="$FAT32_IMG" bs=1M count=$IMG_SIZE 2>/dev/null
    mkfs.vfat -F 32 "$FAT32_IMG" 2>/dev/null

    mkdir -p "$FAT32_MNT"
    sudo mount -o loop,uid=$(id -u),gid=$(id -g) "$FAT32_IMG" "$FAT32_MNT"

    $POSIX_BENCH "$FAT32_MNT" -n $NFILES 2>&1 | tee "$RESULTS_DIR/fat32.txt"
    FS_LIST="$FS_LIST fat32"

    sudo umount "$FAT32_MNT"
    rmdir "$FAT32_MNT"
else
    echo "mkfs.vfat not available, skipping"
fi
echo ""

# ---- exfat ----
echo ">>> [6/N] exFAT Benchmark (kernel, VFS, optimized for flash) <<<"
echo ""

EXFAT_IMG="$IMG_DIR/exfat.img"
EXFAT_MNT="$IMG_DIR/mnt_exfat"

if command -v mkfs.exfat >/dev/null 2>&1; then
    dd if=/dev/zero of="$EXFAT_IMG" bs=1M count=$IMG_SIZE 2>/dev/null
    mkfs.exfat -n "$EXFAT_IMG" 2>/dev/null || mkexfatfs -n "$EXFAT_IMG" 2>/dev/null

    mkdir -p "$EXFAT_MNT"
    sudo mount -o loop,uid=$(id -u),gid=$(id -g) "$EXFAT_IMG" "$EXFAT_MNT"

    $POSIX_BENCH "$EXFAT_MNT" -n $NFILES 2>&1 | tee "$RESULTS_DIR/exfat.txt"
    FS_LIST="$FS_LIST exfat"

    sudo umount "$EXFAT_MNT"
    rmdir "$EXFAT_MNT"
else
    echo "mkfs.exfat not available, skipping"
fi
echo ""

# ---- zfs ----
echo ">>> [7/N] ZFS Benchmark (kernel, VFS, advanced features) <<<"
echo ""

ZFS_IMG="$IMG_DIR/zfs.img"
ZFS_MNT="$IMG_DIR/mnt_zfs"
ZFS_POOL="xxfsbench_$$"

if command -v zpool >/dev/null 2>&1 && command -v zfs >/dev/null 2>&1; then
    dd if=/dev/zero of="$ZFS_IMG" bs=1M count=$IMG_SIZE 2>/dev/null

    sudo zpool create -f "$ZFS_POOL" "$ZFS_IMG" 2>/dev/null || { echo "ZFS pool creation failed, skipping"; ZFS_SKIP=1; }

    if [ -z "$ZFS_SKIP" ]; then
        sudo zfs create "$ZFS_POOL/data" 2>/dev/null
        sudo zfs set mountpoint="$ZFS_MNT" "$ZFS_POOL/data" 2>/dev/null
        sudo chown $(id -u):$(id -g) "$ZFS_MNT"

        $POSIX_BENCH "$ZFS_MNT" -n $NFILES 2>&1 | tee "$RESULTS_DIR/zfs.txt"
        FS_LIST="$FS_LIST zfs"

        sudo zfs destroy "$ZFS_POOL/data" 2>/dev/null || true
        sudo zpool destroy "$ZFS_POOL" 2>/dev/null || true
    fi
else
    echo "ZFS tools not available, skipping"
fi
echo ""

# ---- Summary ----
echo "============================================================"
echo "  Comparison Summary  —  Latency (us/op, lower is better)"
echo "============================================================"
echo ""

extract_field() {
    local file="$1"
    local pattern="$2"
    local field="$3"
    grep "$pattern" "$file" 2>/dev/null | head -1 | awk "{print \$$field}"
}

# Print header
printf "%-30s" "Test"
for fs in $FS_LIST; do
    printf " %12s" "$fs"
done
echo ""

printf "%-30s" "---"
for fs in $FS_LIST; do
    printf " %12s" "---"
done
echo ""

# Print data
for display_name in "create (sequential)" "stat (sequential)" "stat (random)" \
    "write small" "read small (sequential)" "read small (random)" \
    "write 4K blocks" "mkdir (sequential)" "readdir" "rmdir (sequential)" "unlink (sequential)"; do

    printf "%-30s" "$display_name"

    for fs in $FS_LIST; do
        lat=$(extract_field "$RESULTS_DIR/${fs}.txt" "$display_name" 7)
        if [ -z "$lat" ]; then
            lat=$(extract_field "$RESULTS_DIR/${fs}.txt" "$display_name" 6)
        fi
        printf " %12s" "${lat:---}"
    done
    echo ""
done

echo ""
echo "============================================================"
echo "  Comparison Summary  —  Throughput (ops/s, higher is better)"
echo "============================================================"
echo ""

# Print header
printf "%-30s" "Test"
for fs in $FS_LIST; do
    printf " %12s" "$fs"
done
echo ""

printf "%-30s" "---"
for fs in $FS_LIST; do
    printf " %12s" "---"
done
echo ""

# Print data
for display_name in "create (sequential)" "stat (sequential)" "stat (random)" \
    "write small" "read small (sequential)" "read small (random)" \
    "write 4K blocks" "mkdir (sequential)" "readdir" "rmdir (sequential)" "unlink (sequential)"; do

    printf "%-30s" "$display_name"

    for fs in $FS_LIST; do
        ops=$(extract_field "$RESULTS_DIR/${fs}.txt" "$display_name" 5)
        if [ -z "$ops" ]; then
            ops=$(extract_field "$RESULTS_DIR/${fs}.txt" "$display_name" 4)
        fi
        printf " %12s" "${ops:---}"
    done
    echo ""
done

echo ""
echo "Results saved to: $RESULTS_DIR/"
echo ""
echo "NOTE: XXFS operates in userspace with direct file I/O."
echo "      Others operate through kernel VFS with syscall overhead."
echo "      A FUSE-based XXFS would add ~5-20us per operation for kernel<->user switches."
