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
echo "  XXFS vs ext4 vs btrfs  —  Benchmark Comparison"
echo "  files: $NFILES   image: ${IMG_SIZE}MB"
echo ""
echo "  NOTE: XXFS runs in userspace (direct img I/O, no VFS)."
echo "        ext4/btrfs run in kernel (VFS + syscall overhead)."
echo "        This is NOT an apples-to-apples comparison."
echo "        XXFS advantage: no syscall/VFS per operation."
echo "        ext4/btrfs advantage: kernel page cache, scheduler."
echo "============================================================"
echo ""

# ---- XXFS ----
echo ">>> [1/3] XXFS Benchmark (userspace, direct img) <<<"
echo ""
$XXFS_BENCH -n $NFILES -o "$IMG_DIR/xxfs.img" -s $IMG_SIZE 2>&1 | tee "$RESULTS_DIR/xxfs.txt"
echo ""

# ---- ext4 ----
echo ">>> [2/3] ext4 Benchmark (kernel, VFS) <<<"
echo ""

EXT4_IMG="$IMG_DIR/ext4.img"
EXT4_MNT="$IMG_DIR/mnt_ext4"

dd if=/dev/zero of="$EXT4_IMG" bs=1M count=$IMG_SIZE 2>/dev/null
mkfs.ext4 -F -q "$EXT4_IMG" 2>/dev/null

mkdir -p "$EXT4_MNT"
sudo mount -o loop "$EXT4_IMG" "$EXT4_MNT"
sudo chown $(id -u):$(id -g) "$EXT4_MNT"

$POSIX_BENCH "$EXT4_MNT" -n $NFILES 2>&1 | tee "$RESULTS_DIR/ext4.txt"

sudo umount "$EXT4_MNT"
rmdir "$EXT4_MNT"
echo ""

# ---- btrfs ----
echo ">>> [3/3] btrfs Benchmark (kernel, VFS, CoW) <<<"
echo ""

BTRFS_IMG="$IMG_DIR/btrfs.img"
BTRFS_MNT="$IMG_DIR/mnt_btrfs"

dd if=/dev/zero of="$BTRFS_IMG" bs=1M count=$IMG_SIZE 2>/dev/null
mkfs.btrfs -f -q "$BTRFS_IMG" 2>/dev/null || { echo "mkfs.btrfs not available, skipping"; BTRFS_SKIP=1; }

if [ -z "$BTRFS_SKIP" ]; then
    mkdir -p "$BTRFS_MNT"
    sudo mount -o loop "$BTRFS_IMG" "$BTRFS_MNT"
    sudo chown $(id -u):$(id -g) "$BTRFS_MNT"

    $POSIX_BENCH "$BTRFS_MNT" -n $NFILES 2>&1 | tee "$RESULTS_DIR/btrfs.txt"

    sudo umount "$BTRFS_MNT"
    rmdir "$BTRFS_MNT"
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

printf "%-30s %12s %12s" "Test" "XXFS" "ext4"
if [ -z "$BTRFS_SKIP" ]; then
    printf " %12s" "btrfs"
fi
echo ""
printf "%-30s %12s %12s" "---" "---" "---"
if [ -z "$BTRFS_SKIP" ]; then
    printf " %12s" "---"
fi
echo ""

for display_name in "create (sequential)" "stat (sequential)" "stat (random)" \
    "write small" "read small (sequential)" "read small (random)" \
    "write 4K blocks" "mkdir (sequential)" "readdir" "rmdir (sequential)" "unlink (sequential)"; do

    xxfs_lat=$(extract_field "$RESULTS_DIR/xxfs.txt" "$display_name" 7)
    ext4_lat=$(extract_field "$RESULTS_DIR/ext4.txt" "$display_name" 7)

    if [ -z "$xxfs_lat" ]; then
        xxfs_lat=$(extract_field "$RESULTS_DIR/xxfs.txt" "$display_name" 6)
    fi
    if [ -z "$ext4_lat" ]; then
        ext4_lat=$(extract_field "$RESULTS_DIR/ext4.txt" "$display_name" 6)
    fi

    printf "%-30s %12s %12s" "$display_name" "${xxfs_lat:---}" "${ext4_lat:---}"

    if [ -z "$BTRFS_SKIP" ]; then
        btrfs_lat=$(extract_field "$RESULTS_DIR/btrfs.txt" "$display_name" 7)
        if [ -z "$btrfs_lat" ]; then
            btrfs_lat=$(extract_field "$RESULTS_DIR/btrfs.txt" "$display_name" 6)
        fi
        printf " %12s" "${btrfs_lat:---}"
    fi
    echo ""
done

echo ""
echo "============================================================"
echo "  Comparison Summary  —  Throughput (ops/s, higher is better)"
echo "============================================================"
echo ""

printf "%-30s %12s %12s" "Test" "XXFS" "ext4"
if [ -z "$BTRFS_SKIP" ]; then
    printf " %12s" "btrfs"
fi
echo ""
printf "%-30s %12s %12s" "---" "---" "---"
if [ -z "$BTRFS_SKIP" ]; then
    printf " %12s" "---"
fi
echo ""

for display_name in "create (sequential)" "stat (sequential)" "stat (random)" \
    "write small" "read small (sequential)" "read small (random)" \
    "write 4K blocks" "mkdir (sequential)" "readdir" "rmdir (sequential)" "unlink (sequential)"; do

    xxfs_ops=$(extract_field "$RESULTS_DIR/xxfs.txt" "$display_name" 5)
    ext4_ops=$(extract_field "$RESULTS_DIR/ext4.txt" "$display_name" 5)

    if [ -z "$xxfs_ops" ]; then
        xxfs_ops=$(extract_field "$RESULTS_DIR/xxfs.txt" "$display_name" 4)
    fi
    if [ -z "$ext4_ops" ]; then
        ext4_ops=$(extract_field "$RESULTS_DIR/ext4.txt" "$display_name" 4)
    fi

    printf "%-30s %12s %12s" "$display_name" "${xxfs_ops:---}" "${ext4_ops:---}"

    if [ -z "$BTRFS_SKIP" ]; then
        btrfs_ops=$(extract_field "$RESULTS_DIR/btrfs.txt" "$display_name" 5)
        if [ -z "$btrfs_ops" ]; then
            btrfs_ops=$(extract_field "$RESULTS_DIR/btrfs.txt" "$display_name" 4)
        fi
        printf " %12s" "${btrfs_ops:---}"
    fi
    echo ""
done

echo ""
echo "Results saved to: $RESULTS_DIR/"
echo ""
echo "NOTE: XXFS operates in userspace with direct file I/O."
echo "      ext4/btrfs operate through kernel VFS with syscall overhead."
echo "      A FUSE-based XXFS would add ~5-20us per operation for kernel<->user switches."
