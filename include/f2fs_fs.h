#pragma once
/**
 * f2fs_fs.h — F2FS on-disk data structures
 *
 * Adapted from:
 *   AOSP external/f2fs-tools/include/f2fs_fs.h
 *   Linux kernel include/linux/f2fs_fs.h
 *
 * All multi-byte fields are little-endian.
 * All structures use __attribute__((packed)) to match the on-disk layout exactly.
 */

#include <cstdint>

// ────────────────────────────────────────────────────────────────────────────
// Basic types (mirrors Linux kernel nomenclature)
// ────────────────────────────────────────────────────────────────────────────
using u8   = uint8_t;
using u16  = uint16_t;
using u32  = uint32_t;
using u64  = uint64_t;
using le16 = uint16_t;  // stored little-endian (on LE host: same as u16)
using le32 = uint32_t;
using le64 = uint64_t;

// ────────────────────────────────────────────────────────────────────────────
// Global constants
// ────────────────────────────────────────────────────────────────────────────
static constexpr u32 F2FS_SUPER_MAGIC          = 0xF2F52010;
static constexpr u32 F2FS_SUPER_OFFSET         = 1024;   // bytes from device start
static constexpr u32 F2FS_BLKSIZE              = 4096;

static constexpr u32 MAX_DEVICES               = 8;
static constexpr u32 F2FS_MAX_EXTENSION        = 64;
static constexpr u32 F2FS_MAX_QUOTAS           = 3;

// inode block-address array
static constexpr u32 DEF_ADDRS_PER_INODE       = 923;    // addr slots in f2fs_inode
static constexpr u32 DEF_NIDS_PER_INODE        = 5;      // NID slots  (dn×2, in×2, din×1)
static constexpr u32 DEF_INLINE_RESERVED_SIZE  = 1;      // skipped slots before inline data

// direct / indirect node block-address arrays
static constexpr u32 ADDRS_PER_BLOCK           = 1018;   // = (4096-24)/4  (node minus footer)
static constexpr u32 NIDS_PER_BLOCK            = 1018;

// NAT
static constexpr u32 NAT_ENTRY_SIZE            = 9;      // sizeof(f2fs_nat_entry) packed
static constexpr u32 NAT_ENTRY_PER_BLOCK       = F2FS_BLKSIZE / NAT_ENTRY_SIZE; // 455

// directory
static constexpr u32 F2FS_NAME_LEN             = 255;
static constexpr u32 F2FS_SLOT_LEN             = 8;
static constexpr u32 NR_DENTRY_IN_BLOCK        = 214;
static constexpr u32 SIZE_OF_DIR_ENTRY         = 11;     // sizeof(f2fs_dir_entry) packed
static constexpr u32 SIZE_OF_DENTRY_BITMAP     = (NR_DENTRY_IN_BLOCK + 7) / 8; // 27
// padding between bitmap and dentry array inside f2fs_dentry_block:
static constexpr u32 SIZE_OF_RESERVED          =
    F2FS_BLKSIZE - ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * NR_DENTRY_IN_BLOCK
                    + SIZE_OF_DENTRY_BITMAP);             // 3

// special block address sentinels
static constexpr u32 NULL_ADDR = 0U;
static constexpr u32 NEW_ADDR  = 0xFFFFFFFFU;

// ────────────────────────────────────────────────────────────────────────────
// i_inline flags  (linux/f2fs_fs.h, verified against kernel 4.19 + 6.x)
// ────────────────────────────────────────────────────────────────────────────
static constexpr u8 F2FS_INLINE_XATTR  = 0x01; // xattr stored inline
static constexpr u8 F2FS_INLINE_DATA   = 0x02; // small files: data inside inode
static constexpr u8 F2FS_INLINE_DENTS  = 0x04; // small dirs: dentries inside inode
static constexpr u8 F2FS_DATA_EXIST    = 0x08; // inline data actually present
static constexpr u8 F2FS_INLINE_DOTS   = 0x10; // implicit "." and ".." dentries
static constexpr u8 F2FS_EXTRA_ATTR    = 0x20; // inode has extra attribute region

// ────────────────────────────────────────────────────────────────────────────
// File types used in directory entries
// ────────────────────────────────────────────────────────────────────────────
static constexpr u8 F2FS_FT_UNKNOWN   = 0;
static constexpr u8 F2FS_FT_REG_FILE  = 1;
static constexpr u8 F2FS_FT_DIR       = 2;
static constexpr u8 F2FS_FT_CHRDEV    = 3;
static constexpr u8 F2FS_FT_BLKDEV    = 4;
static constexpr u8 F2FS_FT_FIFO      = 5;
static constexpr u8 F2FS_FT_SOCK      = 6;
static constexpr u8 F2FS_FT_SYMLINK   = 7;

// ────────────────────────────────────────────────────────────────────────────
// Superblock (one copy at byte 1024, second at byte 1024 + 4096)
// sizeof must fit within 4096 bytes.
// ────────────────────────────────────────────────────────────────────────────
struct f2fs_device {
    u8   path[64];
    le32 total_segments;
} __attribute__((packed));

struct f2fs_super_block {
    le32 magic;
    le16 major_ver;
    le16 minor_ver;
    le32 log_sectorsize;
    le32 log_sectors_per_block;
    le32 log_blocksize;          // log2(block_size); usually 12
    le32 log_blocks_per_seg;     // log2(blocks_per_segment); usually 9
    le32 segs_per_sec;
    le32 secs_per_zone;
    le32 checksum_offset;
    le64 block_count;
    le32 section_count;
    le32 segment_count;
    le32 segment_count_ckpt;
    le32 segment_count_sit;
    le32 segment_count_nat;
    le32 segment_count_ssa;
    le32 segment_count_main;
    le32 segment0_blkaddr;
    le32 cp_blkaddr;
    le32 sit_blkaddr;
    le32 nat_blkaddr;
    le32 ssa_blkaddr;
    le32 main_blkaddr;
    le32 root_ino;
    le32 node_ino;
    le32 meta_ino;
    u8   uuid[16];
    le16 volume_name[512];
    le32 extension_count;
    u8   extension_list[F2FS_MAX_EXTENSION][8];
    le32 cp_payload;
    u8   version[256];
    u8   init_version[256];
    le32 feature;
    u8   encryption_level;
    u8   encrypt_pw_salt[16];
    f2fs_device devs[MAX_DEVICES];
    le32 qf_ino[F2FS_MAX_QUOTAS];
    u8   hot_ext_count;
    le16 s_encoding;
    le16 s_encoding_flags;
    u8   reserved[306];
    le32 crc;
} __attribute__((packed));

// ────────────────────────────────────────────────────────────────────────────
// Checkpoint  (first block of the CP pack; occupies segment_count_ckpt segs)
// Two CP packs alternate (ping-pong); the one with higher checkpoint_ver wins.
// The version bitmaps for SIT/NAT follow immediately after the fixed header.
//
// Field offsets verified by hex-dump against mkfs.f2fs 1.16.0 / kernel 6.x:
//   @  0  checkpoint_ver
//   @  8  user_block_count
//   @ 16  valid_block_count
//   @ 24  rsvd_segment_count
//   @ 28  overprov_segment_count
//   @ 32  free_segment_count
//   @ 36  cur_node_segno[8]     (32 bytes; slots [3..7] = 0xFFFFFFFF if unused)
//   @ 68  cur_node_blkoff[8]    (16 bytes)
//   @ 84  cur_data_segno[8]     (32 bytes)
//   @116  cur_data_blkoff[8]    (16 bytes)
//   @132  ckpt_flags
//   @136  cp_pack_total_block_count
//   @140  cp_pack_start_sum
//   @144  valid_node_count
//   @148  valid_inode_count
//   @152  next_free_nid
//   @156  sit_ver_bitmap_bytesize   ← KEY FIELD
//   @160  nat_ver_bitmap_bytesize   ← KEY FIELD
//   @164  checksum_offset
//   @168  elapsed_time
//   @176  alloc_type[6]
//   @182  sit_ver_bitmap[sit_ver_bitmap_bytesize]
//         nat_ver_bitmap[nat_ver_bitmap_bytesize]
// ────────────────────────────────────────────────────────────────────────────
struct f2fs_checkpoint {
    le64 checkpoint_ver;               // @0
    le64 user_block_count;             // @8
    le64 valid_block_count;            // @16
    le32 rsvd_segment_count;           // @24
    le32 overprov_segment_count;       // @28
    le32 free_segment_count;           // @32
    le32 cur_node_segno[8];            // @36  (32 bytes)
    le16 cur_node_blkoff[8];           // @68  (16 bytes)
    le32 cur_data_segno[8];            // @84  (32 bytes)
    le16 cur_data_blkoff[8];           // @116 (16 bytes)
    le32 ckpt_flags;                   // @132
    le32 cp_pack_total_block_count;    // @136
    le32 cp_pack_start_sum;            // @140
    le32 valid_node_count;             // @144
    le32 valid_inode_count;            // @148
    le32 next_free_nid;                // @152
    le32 sit_ver_bitmap_bytesize;      // @156
    le32 nat_ver_bitmap_bytesize;      // @160
    le32 checksum_offset;              // @164
    le64 elapsed_time;                 // @168
    u8   alloc_type[6];                // @176
    // @182: sit_ver_bitmap + nat_ver_bitmap follow here
} __attribute__((packed));             // fixed header sizeof == 182

static constexpr u32 F2FS_CHECKPOINT_FIXED_SIZE = 182; // offset of version bitmaps

// ────────────────────────────────────────────────────────────────────────────
// NAT (Node Address Table)
// Maps NID → (ino, block_addr).  Two NAT sets alternate on-disk.
// ────────────────────────────────────────────────────────────────────────────
struct f2fs_nat_entry {
    u8   version;
    le32 ino;
    le32 block_addr;
} __attribute__((packed));          // sizeof == 9  → 4096/9 = 455 per block

struct f2fs_nat_block {
    f2fs_nat_entry entries[NAT_ENTRY_PER_BLOCK];
} __attribute__((packed));

// ────────────────────────────────────────────────────────────────────────────
// Extent  (cached largest contiguous extent inside inode)
// ────────────────────────────────────────────────────────────────────────────
struct f2fs_extent {
    le32 fofs;      // file block offset
    le32 blk_addr;  // start block address
    le32 len;       // length in blocks
} __attribute__((packed));

// ────────────────────────────────────────────────────────────────────────────
// Inode
//
// Physical layout inside a 4096-byte node block (before node_footer):
//   bytes [0..359]           : fixed header fields
//   bytes [360..360+923*4-1] : union{ extra_attrs struct | i_addr[923] }
//   bytes [360+923*4..]      : i_nid[5]
//
// 360 + 923*4 + 5*4 = 360 + 3692 + 20 = 4072  →  4072 + 24 (footer) = 4096 ✓
//
// When F2FS_EXTRA_ATTR is set in i_inline:
//   i_addr[0 .. i_extra_isize/4 - 1]  : extra attribute fields (see union below)
//   i_addr[i_extra_isize/4 .. end]     : data block addresses (or inline data)
// ────────────────────────────────────────────────────────────────────────────
struct f2fs_inode {
    le16 i_mode;
    u8   i_advise;
    u8   i_inline;          // F2FS_INLINE_* flags
    le32 i_uid;
    le32 i_gid;
    le32 i_links;
    le64 i_size;
    le64 i_blocks;
    // NOTE: kernel groups all THREE 64-bit timestamps together, THEN all
    // three 32-bit nsec fields — NOT interleaved as atime/atime_nsec/ctime/...
    // (verified by hex-dump against a real Android image; an earlier
    // interleaved version of this struct silently scrambled all three
    // timestamps while leaving every field after i_mtime_nsec unaffected,
    // since the total byte span [32..68) is identical in both orderings).
    le64 i_atime;
    le64 i_ctime;
    le64 i_mtime;
    le32 i_atime_nsec;
    le32 i_ctime_nsec;
    le32 i_mtime_nsec;
    le32 i_generation;
    le32 i_current_depth;   // directory hash-tree depth (union: i_gc_failures for pinned)
    le32 i_xattr_nid;
    le32 i_flags;
    le32 i_pino;            // parent inode number
    le32 i_namelen;
    u8   i_name[F2FS_NAME_LEN];
    u8   i_dir_level;
    f2fs_extent i_ext;

    // Union: when F2FS_EXTRA_ATTR is set, the first (i_extra_isize/4) slots of
    // i_addr[] are occupied by the packed extra-attribute struct below.
    // Otherwise the full i_addr[923] array is available for block addresses.
    union {
        struct {
            le16 i_extra_isize;         // size of extra attr region in bytes
            le16 i_inline_xattr_size;   // inline xattr size in 4-byte units
            le32 i_projid;
            le32 i_inode_checksum;
            le64 i_crtime;
            le32 i_crtime_nsec;
            le64 i_compr_blocks;
            u8   i_compress_algorithm;
            u8   i_log_cluster_size;
            le16 i_compress_flag;
        } __attribute__((packed));      // 36 bytes → 9 extra slots
        le32 i_addr[DEF_ADDRS_PER_INODE];
    };
    le32 i_nid[DEF_NIDS_PER_INODE];    // dn[0], dn[1], in[0], in[1], din[0]
} __attribute__((packed));

// ────────────────────────────────────────────────────────────────────────────
// Direct / Indirect nodes
// ────────────────────────────────────────────────────────────────────────────
struct direct_node {
    le32 addr[ADDRS_PER_BLOCK];        // 1018 block addresses
} __attribute__((packed));

struct indirect_node {
    le32 nid[NIDS_PER_BLOCK];          // 1018 child NIDs
} __attribute__((packed));

// ────────────────────────────────────────────────────────────────────────────
// Node footer (last 24 bytes of every 4096-byte node block)
// ────────────────────────────────────────────────────────────────────────────
struct node_footer {
    le32 nid;
    le32 ino;
    le32 flag;          // cold / fsync / dentry marks + blk-offset
    le64 cp_ver;
    le32 next_blkaddr;
} __attribute__((packed));             // sizeof == 24

// ────────────────────────────────────────────────────────────────────────────
// Full 4096-byte node block
// ────────────────────────────────────────────────────────────────────────────
struct f2fs_node {
    union {
        f2fs_inode   i;
        direct_node  dn;
        indirect_node in;
    };
    node_footer footer;
} __attribute__((packed));             // sizeof must == 4096

static_assert(sizeof(f2fs_node) == F2FS_BLKSIZE,
    "f2fs_node size mismatch – check padding in f2fs_inode");

// ────────────────────────────────────────────────────────────────────────────
// Directory entry
// ────────────────────────────────────────────────────────────────────────────
struct f2fs_dir_entry {
    le32 hash_code;
    le32 ino;
    le16 name_len;
    u8   file_type;
} __attribute__((packed));             // sizeof == 11  == SIZE_OF_DIR_ENTRY

// Full 4096-byte directory data block
struct f2fs_dentry_block {
    u8              dentry_bitmap[SIZE_OF_DENTRY_BITMAP]; // 27
    u8              reserved[SIZE_OF_RESERVED];            // 3
    f2fs_dir_entry  dentry[NR_DENTRY_IN_BLOCK];           // 214 × 11 = 2354
    u8              filename[NR_DENTRY_IN_BLOCK][F2FS_SLOT_LEN]; // 214 × 8 = 1712
} __attribute__((packed));             // 27+3+2354+1712 = 4096

static_assert(sizeof(f2fs_dentry_block) == F2FS_BLKSIZE,
    "f2fs_dentry_block size mismatch");

// ────────────────────────────────────────────────────────────────────────────
// Compression support  (linux/f2fs_fs.h + fs/f2fs/f2fs.h, kernel 5.6+)
// ────────────────────────────────────────────────────────────────────────────

// i_flags bit: file uses transparent compression
static constexpr u32 F2FS_COMPR_FL       = 0x00000004;

// Sentinel value in i_addr[] that marks the start of a compressed cluster
static constexpr u32 COMPRESS_ADDR       = 0xFFFFFFFEU;

// Compression algorithm IDs stored in inode.i_compress_algorithm
enum class F2FSCompressAlgo : u8 {
    LZO    = 0,
    LZ4    = 1,
    ZSTD   = 2,
    LZORLE = 3,
};

// Header occupying the first 24 bytes of the first physical block in a
// compressed cluster.  Compressed payload (cdata[]) immediately follows.
//
// Verified against fs/f2fs/f2fs.h (struct compress_data) and a real device
// image — there is NO magic number in this on-disk header. An earlier
// version of this struct invented a magic/version/size(u16) layout that
// does not exist on disk; 0xF5F2C000 (F2FS_COMPRESSED_PAGE_MAGIC in the
// kernel) is an in-memory page_private() marker, never written to media.
//
// chksum is only meaningful when the inode's COMPRESS_CHKSUM flag bit is
// set (i_compress_flag & 1); otherwise it is written as zero and unused.
//
// LZO-RLE (algo=3) uses the exact same on-disk format and the exact same
// decompressor as plain LZO (algo=0) — confirmed in fs/f2fs/compress.c:
// f2fs_lzorle_ops.decompress_pages == lzo_decompress_pages. "RLE" only
// affects how the *compressor* runs LZO (lzorle1x_1_compress vs
// lzo1x_1_compress); there is no separate zero-page bitmask or any other
// cluster-level special-casing for it. reserved[] is genuinely unused.
struct f2fs_compress_header {
    le32 clen;          // compressed payload length in bytes (cdata[])
    le32 chksum;         // CRC32 of cdata[], valid only if COMPRESS_CHKSUM flag set
    le32 reserved[4];    // always zero on current kernels; no defined meaning
} __attribute__((packed)); // sizeof == 24

static constexpr u32 F2FS_COMPRESS_HEADER_SIZE = 24; // sizeof(f2fs_compress_header)


// ────────────────────────────────────────────────────────────────────────────
// Extended attributes (xattr)  — linux/f2fs_fs.h + fs/f2fs/xattr.h
// ────────────────────────────────────────────────────────────────────────────

static constexpr u32 F2FS_XATTR_MAGIC = 0xF2F52011;

// Name-prefix index → string prefix:
//   1  "user."
//   2  "system.posix_acl_access"
//   3  "system.posix_acl_default"
//   4  "trusted."
//   6  "security."
//   7  "system."
static constexpr u8 XATTR_INDEX_USER      = 1;
static constexpr u8 XATTR_INDEX_POSIX_ACL_ACCESS  = 2;
static constexpr u8 XATTR_INDEX_POSIX_ACL_DEFAULT = 3;
static constexpr u8 XATTR_INDEX_TRUSTED   = 4;
static constexpr u8 XATTR_INDEX_SECURITY  = 6;
static constexpr u8 XATTR_INDEX_SYSTEM    = 7;

// Header at the start of every xattr block (inline or external)
struct f2fs_xattr_header {
    le32 h_magic;      // F2FS_XATTR_MAGIC
    le32 h_refcount;
} __attribute__((packed));  // sizeof == 8

// One attribute entry.  Immediately follows the header and previous entries.
// Layout:   [e_name_index(1)][e_name_len(1)][e_value_size(2)]
//           [name: e_name_len bytes][value: e_value_size bytes]
//           [padding: 0-3 bytes to reach next 4-byte boundary]
struct f2fs_xattr_entry {
    u8   e_name_index;
    u8   e_name_len;
    le16 e_value_size;
    // char e_name[];  uint8_t e_value[];  uint8_t pad[];
} __attribute__((packed));  // sizeof == 4

// Total unpadded size of one entry (header + name + value)
inline size_t xattr_entry_raw_size(const f2fs_xattr_entry* e) {
    return sizeof(f2fs_xattr_entry) + e->e_name_len + e->e_value_size;
}
// Advance to the next entry (4-byte aligned)
inline const f2fs_xattr_entry* xattr_next_entry(const f2fs_xattr_entry* e) {
    size_t sz = (xattr_entry_raw_size(e) + 3u) & ~3u;
    return reinterpret_cast<const f2fs_xattr_entry*>(
        reinterpret_cast<const uint8_t*>(e) + sz);
}

// Linux VFS capability blob (security.capability xattr value)
// Defined in linux/capability.h as struct vfs_cap_data (20 bytes)
static constexpr uint32_t VFS_CAP_REVISION_MASK = 0xFF000000u;
static constexpr uint32_t VFS_CAP_REVISION_2    = 0x02000000u;
static constexpr uint32_t VFS_CAP_REVISION_3    = 0x03000000u;

struct vfs_cap_data {
    le32 magic_etc;              // VFS_CAP_REVISION_x | flags
    struct { le32 permitted, inheritable; } data[2]; // [0]=lo32 [1]=hi32
} __attribute__((packed));      // sizeof == 20
