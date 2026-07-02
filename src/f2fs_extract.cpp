/**
 * f2fs_extract.cpp
 *
 * F2FS image extractor implementation.
 *
 * Key design notes
 * ────────────────
 * • NAT (Node Address Table) is loaded entirely into a hash-map at open()
 *   time for O(1) NID → block-address lookup.
 *
 * • The on-disk NAT area is split into two equal ping-pong sets (A and B).
 *   The active set for each NAT block index is determined by a bit in
 *   nat_ver_bitmap from the chosen checkpoint.  If the bit is 0 → set A;
 *   if 1 → set B.
 *
 * • Block-address lookup for a file follows the F2FS indirect-block tree:
 *     direct slots  → i_addr[extra..extra+addrs-1]
 *     single-indir  → i_nid[0..1]  → direct_node.addr[]
 *     double-indir  → i_nid[2..3]  → indirect_node.nid[] → direct_node.addr[]
 *     triple-indir  → i_nid[4]     → indirect → indirect → direct
 *
 * • Inline data / inline dentries are handled before the block-tree path.
 *
 * • Encrypted inodes are detected via i_flags (FSCRYPT bit); a placeholder
 *   directory / file is created and a warning is emitted.
 */

#include "f2fs_extract.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32) || defined(__MINGW32__)
#  include <sys/utime.h>
#endif
#include "win_pread.h"  // POSIX pread() + stat-macro shims on Windows; passthrough on POSIX
#include "f2fs_compress.h"
#include "f2fs_metadata.h"

namespace fs = std::filesystem;

// ────────────────────────────────────────────────────────────────────────────
// inode flag bits (from include/linux/f2fs_fs.h)
// ────────────────────────────────────────────────────────────────────────────
static constexpr u32 F2FS_ENCRYPT_FL = 0x00000800; // directory / file is encrypted

// Maximum recursion depth for directory traversal
static constexpr int MAX_DEPTH = 512;

// ════════════════════════════════════════════════════════════════════════════
// Construction / destruction
// ════════════════════════════════════════════════════════════════════════════

F2FSExtractor::F2FSExtractor(LogFn log)
    : log_(std::move(log))
{}

F2FSExtractor::~F2FSExtractor()
{
    if (fd_ >= 0) ::close(fd_);
}

// ════════════════════════════════════════════════════════════════════════════
// Logging
// ════════════════════════════════════════════════════════════════════════════

void F2FSExtractor::logf(LogLevel lvl, const char* fmt, ...) const
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (log_) {
        log_(lvl, buf);
    } else {
        FILE* out = (lvl == LogLevel::ERR) ? stderr : stdout;
        const char* prefix = (lvl == LogLevel::ERR)  ? "[ERR] "
                           : (lvl == LogLevel::WARN) ? "[WRN] "
                                                     : "[INF] ";
        fputs(prefix, out);
        fputs(buf, out);
        fputc('\n', out);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Low-level I/O
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::readAt(off_t offset, void* buf, size_t size) const
{
    ssize_t r = ::pread(fd_, buf, size, offset);
    if (r == (ssize_t)size) return true;
    if (r < 0)
        logf(LogLevel::ERR, "pread(%llu @ %lld): %s", (unsigned long long)size, (long long)offset, strerror(errno));
    else
        logf(LogLevel::ERR, "pread(%llu @ %lld): short read %lld", (unsigned long long)size, (long long)offset, (long long)r);
    return false;
}

bool F2FSExtractor::readBlock(u32 blkno, void* buf) const
{
    // Detect stale block addresses beyond the image boundary.
    // Samsung builds some partitions by creating a full-size F2FS image, filling
    // it with data, then shrinking it (truncating / punching holes). After
    // shrinking, inode block pointers that used to be valid now point beyond
    // the image's EOF. Rather than failing the whole file, we return a zero
    // block (matching what the kernel does for punched-out or unallocated blocks)
    // and count the occurrences so we can report them as a summary.
    if (total_block_count_ > 0 && (u64)blkno >= total_block_count_) {
        ++stale_blocks_read_;
        memset(buf, 0, blksize_);
        return true;
    }
    return readAt((off_t)blkno * blksize_, buf, blksize_);
}

// ════════════════════════════════════════════════════════════════════════════
// open()  —  entry point
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::open(const std::string& path)
{
    fd_ = ::open(path.c_str(), O_RDONLY | O_LARGEFILE);
    if (fd_ < 0) {
        logf(LogLevel::ERR, "open(%s): %s", path.c_str(), strerror(errno));
        return false;
    }

    if (!readSuperblock()) return false;

    std::vector<u8> nat_ver_bitmap;
    if (!readCheckpoint(nat_ver_bitmap)) return false;

    if (!loadNAT(nat_ver_bitmap)) return false;

    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Superblock
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::readSuperblock()
{
    // Two identical copies: at byte 1024 and byte 1024+4096.
    for (u32 copy = 0; copy < 2; ++copy) {
        off_t off = F2FS_SUPER_OFFSET + (off_t)copy * F2FS_BLKSIZE;
        if (!readAt(off, &sb_, sizeof(sb_))) continue;
        if (sb_.magic == F2FS_SUPER_MAGIC) {
            if (copy == 1) logf(LogLevel::WARN, "Primary superblock bad; using backup copy");
            goto found;
        }
    }
    logf(LogLevel::ERR, "No valid F2FS superblock found (magic mismatch)");
    return false;

found:
    blksize_            = 1u << sb_.log_blocksize;
    blocks_per_seg_     = 1u << sb_.log_blocks_per_seg;
    total_block_count_  = sb_.block_count;

    if (blksize_ != F2FS_BLKSIZE) {
        logf(LogLevel::ERR, "Unsupported block size %u (only 4096 supported)", blksize_);
        return false;
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Checkpoint
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::readCheckpoint(std::vector<u8>& nat_ver_bitmap_out)
{
    // CP area is split into two packs of equal size.
    // Each pack starts with one f2fs_checkpoint block; we compare checkpoint_ver
    // and take the pack with the higher value.
    const u32 cp_segs     = sb_.segment_count_ckpt;
    const u32 pack_blocks = (cp_segs / 2) * blocks_per_seg_; // blocks per pack
    const u32 cp0_blk     = sb_.cp_blkaddr;
    const u32 cp1_blk     = sb_.cp_blkaddr + pack_blocks;

    std::vector<u8> blk0(blksize_), blk1(blksize_);

    bool ok0 = readBlock(cp0_blk, blk0.data());
    bool ok1 = readBlock(cp1_blk, blk1.data());

    if (!ok0 && !ok1) {
        logf(LogLevel::ERR, "Cannot read either checkpoint pack");
        return false;
    }

    const f2fs_checkpoint* cp0 = ok0 ? reinterpret_cast<const f2fs_checkpoint*>(blk0.data()) : nullptr;
    const f2fs_checkpoint* cp1 = ok1 ? reinterpret_cast<const f2fs_checkpoint*>(blk1.data()) : nullptr;

    const f2fs_checkpoint* cp  = nullptr;
    const u8*              cp_blk_data = nullptr;

    if (!cp1 || (cp0 && cp0->checkpoint_ver >= cp1->checkpoint_ver)) {
        cp           = cp0;
        cp_blk_data  = blk0.data();
        logf(LogLevel::INFO, "Using CP pack 0  (ver=0x%llX)", (unsigned long long)cp0->checkpoint_ver);
    } else {
        cp           = cp1;
        cp_blk_data  = blk1.data();
        logf(LogLevel::INFO, "Using CP pack 1  (ver=0x%llX)", (unsigned long long)cp1->checkpoint_ver);
    }

    // nat_ver_bitmap immediately follows the fixed CP fields, after sit_ver_bitmap.
    const u32 sit_bm_sz = cp->sit_ver_bitmap_bytesize;
    const u32 nat_bm_sz = cp->nat_ver_bitmap_bytesize;

    const u8* nat_bm_ptr = cp_blk_data + F2FS_CHECKPOINT_FIXED_SIZE + sit_bm_sz;

    // Sanity: bitmap must fit inside one 4096-byte block.
    if (F2FS_CHECKPOINT_FIXED_SIZE + sit_bm_sz + nat_bm_sz > blksize_) {
        logf(LogLevel::WARN, "NAT ver bitmap exceeds one block — using empty bitmap");
        nat_ver_bitmap_out.assign(nat_bm_sz, 0);
    } else {
        nat_ver_bitmap_out.assign(nat_bm_ptr, nat_bm_ptr + nat_bm_sz);
    }

    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// NAT loading
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::loadNAT(const std::vector<u8>& nat_ver_bitmap)
{
    // NAT area occupies segment_count_nat segments starting at nat_blkaddr.
    // It is split into two equal-sized ping-pong sets:
    //   Set A: nat_blkaddr + [0 .. half_blocks-1]
    //   Set B: nat_blkaddr + [half_blocks .. nat_blocks-1]
    //
    // For each NAT block index i:
    //   bit i of nat_ver_bitmap == 0  →  use Set A block i
    //   bit i of nat_ver_bitmap == 1  →  use Set B block i

    const u32 nat_segs        = sb_.segment_count_nat;
    const u32 nat_total_blks  = nat_segs * blocks_per_seg_;
    const u32 half_blocks     = nat_total_blks / 2; // blocks per set

    logf(LogLevel::INFO, "NAT: %u segs, %u blocks total, %u per set, nat_blkaddr=0x%X",
         nat_segs, nat_total_blks, half_blocks, (unsigned)sb_.nat_blkaddr);

    std::vector<u8> blkbuf(blksize_);
    const auto* nb = reinterpret_cast<const f2fs_nat_block*>(blkbuf.data());

    for (u32 i = 0; i < half_blocks; ++i) {
        // Choose set A or B according to bitmap
        bool use_set_b = false;
        if (i / 8 < (u32)nat_ver_bitmap.size()) {
            use_set_b = (nat_ver_bitmap[i / 8] >> (i % 8)) & 1u;
        }

        const u32 phys_blk = sb_.nat_blkaddr + i + (use_set_b ? half_blocks : 0);

        if (!readBlock(phys_blk, blkbuf.data())) {
            logf(LogLevel::WARN, "Cannot read NAT block %u (phys %u), skipping", i, phys_blk);
            continue;
        }

        for (u32 j = 0; j < NAT_ENTRY_PER_BLOCK; ++j) {
            const auto& e   = nb->entries[j];
            const u32   nid = i * NAT_ENTRY_PER_BLOCK + j;
            const u32   blk = e.block_addr;
            if (blk != NULL_ADDR && blk != NEW_ADDR) {
                nat_[nid] = blk;
            }
        }
    }

    logf(LogLevel::INFO, "NAT loaded: %llu valid entries", (unsigned long long)nat_.size());
    return !nat_.empty();
}

// ────────────────────────────────────────────────────────────────────────────
u32 F2FSExtractor::nidToBlock(u32 nid) const
{
    auto it = nat_.find(nid);
    return (it != nat_.end()) ? it->second : NULL_ADDR;
}

bool F2FSExtractor::readNode(u32 nid, f2fs_node& out) const
{
    const u32 blkno = nidToBlock(nid);
    if (blkno == NULL_ADDR) {
        logf(LogLevel::WARN, "NID %u not in NAT", nid);
        return false;
    }
    return readBlock(blkno, &out);
}

// ════════════════════════════════════════════════════════════════════════════
// Inode helpers
// ════════════════════════════════════════════════════════════════════════════

/*static*/ int F2FSExtractor::extraSlots(const f2fs_inode& inode)
{
    if (!(inode.i_inline & F2FS_EXTRA_ATTR)) return 0;
    return (int)(inode.i_extra_isize / sizeof(le32));
}

/*static*/ int F2FSExtractor::xattrSlots(const f2fs_inode& inode)
{
    if (!(inode.i_inline & F2FS_INLINE_XATTR)) return 0;
    return (int)inode.i_inline_xattr_size;
}

/*static*/ int F2FSExtractor::addrsPerInode(const f2fs_inode& inode)
{
    return (int)DEF_ADDRS_PER_INODE - extraSlots(inode) - xattrSlots(inode);
}

/*static*/ const le32* F2FSExtractor::inodeAddrs(const f2fs_inode& inode)
{
    return inode.i_addr + extraSlots(inode);
}

/*static*/ const u8* F2FSExtractor::inlineDataPtr(const f2fs_inode& inode)
{
    // Skip extra-attr region + 1 reserved slot
    int skip = extraSlots(inode) + DEF_INLINE_RESERVED_SIZE;
    return reinterpret_cast<const u8*>(inode.i_addr + skip);
}

/*static*/ u64 F2FSExtractor::inlineDataBytes(const f2fs_inode& inode)
{
    int avail = (int)DEF_ADDRS_PER_INODE
              - extraSlots(inode)
              - xattrSlots(inode)
              - (int)DEF_INLINE_RESERVED_SIZE;
    if (avail <= 0) return 0;
    return (u64)avail * sizeof(le32);
}

// ════════════════════════════════════════════════════════════════════════════
// Block-address lookup  (fileBlockAddr)
//
// F2FS indirect-tree:
//   i_addr[0..addrs-1]                  → direct blocks
//   i_nid[0..1]                         → direct_node  → ADDRS_PER_BLOCK addr
//   i_nid[2..3]                         → indirect_node → NIDS_PER_BLOCK dn
//                                         each dn → ADDRS_PER_BLOCK addr
//   i_nid[4]                            → indirect_node → NIDS_PER_BLOCK in
//                                         each in → NIDS_PER_BLOCK dn
//                                         each dn → ADDRS_PER_BLOCK addr
// ════════════════════════════════════════════════════════════════════════════

u32 F2FSExtractor::fileBlockAddr(const f2fs_inode& inode, u64 bidx) const
{
    const le32* daddrs  = inodeAddrs(inode);
    const int   n_dir   = addrsPerInode(inode);

    // ── Direct ────────────────────────────────────────────────────────────
    if (bidx < (u64)n_dir) {
        return daddrs[bidx];
    }
    bidx -= (u64)n_dir;

    // ── Single-indirect: i_nid[0], i_nid[1] ──────────────────────────────
    for (int k = 0; k < 2; ++k) {
        if (bidx < ADDRS_PER_BLOCK) {
            u32 dn_nid = inode.i_nid[k];
            if (dn_nid == 0) return NULL_ADDR;
            f2fs_node dn;
            if (!readNode(dn_nid, dn)) return NULL_ADDR;
            return dn.dn.addr[bidx];
        }
        bidx -= ADDRS_PER_BLOCK;
    }

    // ── Double-indirect: i_nid[2], i_nid[3] ──────────────────────────────
    static constexpr u64 DIND_RANGE = (u64)NIDS_PER_BLOCK * ADDRS_PER_BLOCK;
    for (int k = 2; k < 4; ++k) {
        if (bidx < DIND_RANGE) {
            u32 in_nid = inode.i_nid[k];
            if (in_nid == 0) return NULL_ADDR;
            f2fs_node in_node;
            if (!readNode(in_nid, in_node)) return NULL_ADDR;

            const u32 inner_idx = (u32)(bidx / ADDRS_PER_BLOCK);
            const u32 inner_off = (u32)(bidx % ADDRS_PER_BLOCK);
            const u32 dn_nid    = in_node.in.nid[inner_idx];
            if (dn_nid == 0) return NULL_ADDR;

            f2fs_node dn;
            if (!readNode(dn_nid, dn)) return NULL_ADDR;
            return dn.dn.addr[inner_off];
        }
        bidx -= DIND_RANGE;
    }

    // ── Triple-indirect: i_nid[4] ─────────────────────────────────────────
    {
        u32 tin_nid = inode.i_nid[4];
        if (tin_nid == 0) return NULL_ADDR;
        f2fs_node tin;
        if (!readNode(tin_nid, tin)) return NULL_ADDR;

        static constexpr u64 PER_DIND  = (u64)NIDS_PER_BLOCK * ADDRS_PER_BLOCK;
        const u32 l2_idx = (u32)(bidx / PER_DIND);
        const u64 l2_off = bidx % PER_DIND;
        if (l2_idx >= NIDS_PER_BLOCK) return NULL_ADDR;

        const u32 in_nid = tin.in.nid[l2_idx];
        if (in_nid == 0) return NULL_ADDR;
        f2fs_node in_node;
        if (!readNode(in_nid, in_node)) return NULL_ADDR;

        const u32 inner_idx = (u32)(l2_off / ADDRS_PER_BLOCK);
        const u32 inner_off = (u32)(l2_off % ADDRS_PER_BLOCK);
        const u32 dn_nid    = in_node.in.nid[inner_idx];
        if (dn_nid == 0) return NULL_ADDR;

        f2fs_node dn;
        if (!readNode(dn_nid, dn)) return NULL_ADDR;
        return dn.dn.addr[inner_off];
    }
}

// ════════════════════════════════════════════════════════════════════════════
// extractAll()
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::extractAll(const std::string& outdir)
{
    fs::create_directories(outdir);
    outdir_base_ = outdir;   // remember for relative-path metadata
    const u32 root_ino = sb_.root_ino;
    logf(LogLevel::INFO, "Extracting from root inode %u → %s", root_ino, outdir.c_str());

    bool ok = extractDir(root_ino, outdir, 0);

    // extractDir() doesn't know the root's own inode (it's read internally),
    // so re-read it here to stamp the output directory's timestamps —
    // every other entry gets this via extractEntry(), but the root bypasses
    // that dispatcher.
    f2fs_node root_node;
    if (readNode(root_ino, root_node)) {
        if (collect_metadata_) collectMeta("/", root_node.i);
        applyTimestamps(outdir, root_node.i);
    }

    if (symlinks_skipped_ > 0) {
        logf(LogLevel::WARN,
             "Skipped %u symlink(s): target filesystem doesn't support them "
             "(e.g. Android FUSE / exFAT). "
             "Symlink targets are preserved in _metadata/f2fs_special.txt.",
             symlinks_skipped_);
    }

    if (stale_blocks_read_ > 0) {
        logf(LogLevel::WARN,
             "Read %llu block(s) with addresses beyond image EOF (zeroed). "
             "This is normal for Samsung images that were shrunk after creation "
             "(fallocate/punch_hole). Affected files contain zeros where the "
             "original data was.",
             (unsigned long long)stale_blocks_read_);
    }

    return ok;
}

// ════════════════════════════════════════════════════════════════════════════
// extractEntry — dispatcher
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::extractEntry(u32 ino, u8 ftype,
                                 const std::string& outpath, int depth)
{
    if (depth > MAX_DEPTH) {
        logf(LogLevel::WARN, "Max depth exceeded at %s", outpath.c_str());
        return false;
    }

    f2fs_node node;
    if (!readNode(ino, node)) return false;

    // Cross-check footer
    if (node.footer.ino != ino) {
        logf(LogLevel::WARN, "NID %u: footer.ino=%u mismatch — possible corruption",
             ino, (u32)node.footer.ino);
        // Continue anyway; footer check is advisory only.
    }

    const f2fs_inode& inode = node.i;

    // Collect metadata before dispatching by type
    if (collect_metadata_) {
        // Build FS-relative path: strip outdir_base_ prefix, prepend "/"
        std::string fspath = "/";
        if (outpath.size() > outdir_base_.size())
            fspath += outpath.substr(outdir_base_.size() +
                (outdir_base_.back() == '/' ? 0 : 1));
        collectMeta(fspath, inode);
    }

    const u16         mode  = inode.i_mode;

    // Encrypted inode: create placeholder, don't try to parse data blocks.
    if (inode.i_flags & F2FS_ENCRYPT_FL) {
        logf(LogLevel::WARN, "Encrypted inode %u (%s) — skipping content", ino, outpath.c_str());
        if (ftype == F2FS_FT_DIR || S_ISDIR(mode)) {
            fs::create_directories(outpath);
        } else {
            // Write zero-byte placeholder so the path exists.
            std::ofstream(outpath, std::ios::binary);
        }
        applyTimestamps(outpath, inode);
        return true;
    }

    // Resolve type from dentry hint or inode mode.
    // Each branch stamps atime/mtime on the just-written entry before returning.
    if (ftype == F2FS_FT_DIR || S_ISDIR(mode)) {
        bool ok = extractDir(ino, outpath, depth);
        applyTimestamps(outpath, inode);
        return ok;
    }
    if (ftype == F2FS_FT_SYMLINK || S_ISLNK(mode)) {
        bool ok = extractSymlink(inode, outpath);
        if (ok) applyTimestamps(outpath, inode);
        return ok;
    }
    if (ftype == F2FS_FT_REG_FILE || S_ISREG(mode)) {
        bool ok = extractRegularFile(inode, outpath);
        if (ok) applyTimestamps(outpath, inode);
        return ok;
    }

    // Special files (devices, pipes, sockets) — create empty placeholder.
    logf(LogLevel::INFO, "Special file (type=%u mode=0%o) at %s — placeholder only",
         ftype, mode, outpath.c_str());
    std::ofstream(outpath, std::ios::binary);
    applyTimestamps(outpath, inode);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// extractRegularFile
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::extractRegularFile(const f2fs_inode& inode, const std::string& outpath)
{
    const u64 file_size = inode.i_size;

    // ── Compressed file (F2FS transparent compression) ────────────────────
    if (inode.i_flags & F2FS_COMPR_FL) {
        return extractCompressedFile(inode, outpath);
    }

    FILE* f = ::fopen(outpath.c_str(), "wb");
    if (!f) {
        logf(LogLevel::ERR, "fopen(%s): %s", outpath.c_str(), strerror(errno));
        return false;
    }

    // ── Inline data (small files whose content lives inside the inode) ─────
    if (inode.i_inline & F2FS_INLINE_DATA) {
        const u8* ptr   = inlineDataPtr(inode);
        const u64 avail = inlineDataBytes(inode);
        const u64 wsz   = std::min(file_size, avail);
        ::fwrite(ptr, 1, (size_t)wsz, f);
        ::fclose(f);
        return true;
    }

    // ── Regular block-tree data ───────────────────────────────────────────
    std::vector<u8> blkbuf(blksize_);
    u64 remaining = file_size;
    u64 bidx      = 0;

    while (remaining > 0) {
        const u64 chunk = std::min(remaining, (u64)blksize_);
        const u32 blkno = fileBlockAddr(inode, bidx);

        if (blkno != NULL_ADDR && blkno != NEW_ADDR) {
            if (!readBlock(blkno, blkbuf.data())) {
                ::fclose(f);
                return false;
            }
        } else {
            // Sparse hole: write zeroes.
            std::fill(blkbuf.begin(), blkbuf.end(), u8{0});
        }

        if (::fwrite(blkbuf.data(), 1, (size_t)chunk, f) != (size_t)chunk) {
            logf(LogLevel::ERR, "fwrite(%s): %s", outpath.c_str(), strerror(errno));
            ::fclose(f);
            return false;
        }

        remaining -= chunk;
        ++bidx;
    }

    ::fclose(f);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// extractCompressedFile
//
// F2FS transparent compression — cluster-based layout:
//
//   Each cluster is (1 << i_log_cluster_size) consecutive logical blocks.
//   A cluster is either:
//     (a) COMPRESSED — i_addr[base] == COMPRESS_ADDR (sentinel)
//             Physical blocks: i_addr[base+1] .. i_addr[base+N-1]
//             Block base+1 holds f2fs_compress_header (24 B) + payload start.
//             NULL_ADDR slots → this block was saved (not allocated).
//     (b) PLAIN — i_addr[base] is a normal block address
//             Read cluster_size blocks as-is (identical to non-compressed files).
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::extractCompressedFile(const f2fs_inode& inode,
                                          const std::string& outpath)
{
    const u64 file_size   = inode.i_size;
    const u32 cluster_sz  = 1u << inode.i_log_cluster_size;  // pages per cluster
    const u8  algo        = inode.i_compress_algorithm;

    // ── Compress-released files (Samsung et al.) ──────────────────────────────
    // Some OEM image builders (notably Samsung) compress files and then call
    // F2FS_IOC_RELEASE_COMPRESS_BLOCKS to free the compressed blocks, shrinking
    // the final image.  After this operation:
    //   - F2FS_COMPRESS_RELEASED (i_inline bit 0x80) is set
    //   - Physical block addresses in i_addr may be NULL_ADDR or stale addresses
    //     that now point BEYOND the shrunken image's EOF
    // The kernel returns zeros when reading such a file at runtime (the blocks
    // are simply gone), so we do the same: write i_size zero bytes and skip all
    // block reads.
    if (inode.i_inline & F2FS_COMPRESS_RELEASED) {
        logf(LogLevel::INFO,
             "Compressed-released (blocks freed by OEM): writing %llu zero bytes  %s",
             (unsigned long long)file_size, outpath.c_str());
        FILE* zf = ::fopen(outpath.c_str(), "wb");
        if (!zf) {
            logf(LogLevel::ERR, "fopen(%s): %s", outpath.c_str(), strerror(errno));
            return false;
        }
        const std::vector<u8> zeros(std::min(file_size, (u64)blksize_), 0);
        for (u64 rem = file_size; rem > 0; ) {
            const size_t chunk = (size_t)std::min(rem, (u64)zeros.size());
            if (::fwrite(zeros.data(), 1, chunk, zf) != chunk) {
                logf(LogLevel::ERR, "fwrite(%s): %s", outpath.c_str(), strerror(errno));
                ::fclose(zf);
                return false;
            }
            rem -= chunk;
        }
        ::fclose(zf);
        return true;
    }

    logf(LogLevel::INFO, "Compressed file: algo=%u cluster_pages=%u size=%llu  %s",
         (u32)algo, cluster_sz, (unsigned long long)file_size, outpath.c_str());

    FILE* out = ::fopen(outpath.c_str(), "wb");
    if (!out) {
        logf(LogLevel::ERR, "fopen(%s): %s", outpath.c_str(), strerror(errno));
        return false;
    }

    std::vector<u8> blkbuf(blksize_);
    u64 written = 0;
    u64 bidx    = 0;   // logical block index into the file

    while (written < file_size) {
        const u32 addr0 = fileBlockAddr(inode, bidx);

        // ── (a) Compressed cluster ────────────────────────────────────────
        if (addr0 == COMPRESS_ADDR) {

            // Read the first physical block — contains the compress header.
            const u32 phys1 = fileBlockAddr(inode, bidx + 1);
            if (phys1 == NULL_ADDR || phys1 == NEW_ADDR) {
                logf(LogLevel::ERR, "Compressed cluster at bidx=%llu: missing first block",
                     (unsigned long long)bidx);
                ::fclose(out);
                return false;
            }
            if (!readBlock(phys1, blkbuf.data())) { ::fclose(out); return false; }

            // Parse the compress header (24 bytes at start of first block).
            // No magic/version field on disk — see f2fs_compress_header comment
            // in f2fs_fs.h for why an earlier version of this code was wrong.
            const auto* hdr =
                reinterpret_cast<const f2fs_compress_header*>(blkbuf.data());

            const u32  clen = hdr->clen;   // compressed payload length, bytes

            // Sanity check: a corrupt/garbage clen would otherwise cause us
            // to read far past the cluster's allocated blocks.
            const u64 max_plausible = (u64)cluster_sz * blksize_;
            if (clen == 0 || clen > max_plausible) {
                logf(LogLevel::ERR,
                     "bidx=%llu: implausible compressed length %u (cluster=%llu bytes)",
                     (unsigned long long)bidx, clen, (unsigned long long)max_plausible);
                ::fclose(out);
                return false;
            }

            // Gather compressed payload bytes from this and subsequent blocks.
            //   First block contributes: blksize - 24 bytes (after the header).
            //   Each additional block:   blksize bytes (no header).
            std::vector<u8> cdata;
            cdata.reserve(clen);

            {
                const u8* after_hdr = blkbuf.data() + F2FS_COMPRESS_HEADER_SIZE;
                const size_t avail  = blksize_ - F2FS_COMPRESS_HEADER_SIZE;
                const size_t take   = std::min((size_t)clen, avail);
                cdata.insert(cdata.end(), after_hdr, after_hdr + take);
            }

            for (u32 k = 2; k < cluster_sz && cdata.size() < clen; ++k) {
                const u32 phys_k = fileBlockAddr(inode, bidx + k);
                if (phys_k == NULL_ADDR || phys_k == NEW_ADDR) break;
                if (!readBlock(phys_k, blkbuf.data())) { ::fclose(out); return false; }
                const size_t remaining = clen - cdata.size();
                const size_t take      = std::min(remaining, (size_t)blksize_);
                cdata.insert(cdata.end(), blkbuf.begin(), blkbuf.begin() + (ptrdiff_t)take);
            }

            if (cdata.size() < clen) {
                logf(LogLevel::ERR,
                     "bidx=%llu: only gathered %llu/%u compressed bytes",
                     (unsigned long long)bidx, (unsigned long long)cdata.size(), clen);
                ::fclose(out);
                return false;
            }

            // Decompress into a full-cluster output buffer.
            const u64 plain_sz = (u64)cluster_sz * blksize_;
            std::vector<u8> plain(plain_sz);

            if (!f2fs_decompress_cluster(algo, cdata.data(), clen,
                                         plain.data(), plain_sz)) {
                logf(LogLevel::ERR, "Decompression failed at bidx=%llu",
                     (unsigned long long)bidx);
                ::fclose(out);
                return false;
            }

            const u64 chunk = std::min(file_size - written, plain_sz);
            if (::fwrite(plain.data(), 1, (size_t)chunk, out) != (size_t)chunk) {
                logf(LogLevel::ERR, "fwrite: %s", strerror(errno));
                ::fclose(out);
                return false;
            }
            written += chunk;

        // ── (b) Plain (uncompressed) cluster ─────────────────────────────
        } else {
            for (u32 k = 0; k < cluster_sz && written < file_size; ++k) {
                const u32 phys = (k == 0) ? addr0
                                           : fileBlockAddr(inode, bidx + k);
                if (phys != NULL_ADDR && phys != NEW_ADDR) {
                    if (!readBlock(phys, blkbuf.data())) {
                        ::fclose(out); return false;
                    }
                } else {
                    std::fill(blkbuf.begin(), blkbuf.end(), u8{0});
                }
                const u64 chunk = std::min(file_size - written, (u64)blksize_);
                if (::fwrite(blkbuf.data(), 1, (size_t)chunk, out) != (size_t)chunk) {
                    logf(LogLevel::ERR, "fwrite: %s", strerror(errno));
                    ::fclose(out); return false;
                }
                written += chunk;
            }
        }

        bidx += cluster_sz;   // advance to next cluster
    }

    ::fclose(out);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// extractSymlink
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::extractSymlink(const f2fs_inode& inode, const std::string& outpath)
{
    const u64 sz = inode.i_size;
    std::string target(sz, '\0');

    if (inode.i_inline & F2FS_INLINE_DATA) {
        const u8* ptr = inlineDataPtr(inode);
        std::memcpy(target.data(), ptr, (size_t)sz);
    } else {
        const u32 blkno = fileBlockAddr(inode, 0);
        if (blkno == NULL_ADDR) return false;
        std::vector<u8> blkbuf(blksize_);
        if (!readBlock(blkno, blkbuf.data())) return false;
        std::memcpy(target.data(), blkbuf.data(), (size_t)sz);
    }

    // Remove existing node at the destination path if any.
    std::error_code ec;
    fs::remove(outpath, ec);

    fs::create_symlink(target, outpath, ec);
    if (ec) {
        // ENOSYS / EPERM / ENOTSUP: the target filesystem (e.g. Android FUSE /
        // sdcard / Windows) doesn't support symlinks at all. Expected when
        // extracting to /storage/emulated/0/ on Android. Don't spam per-link
        // warnings — increment a counter and report a single summary at the end.
        // The symlink target is preserved in the metadata files.
        const int e = ec.value();
        if (e == ENOSYS || e == EPERM || e == ENOTSUP) {
            ++symlinks_skipped_;
            return true;   // not an extraction failure
        }
        logf(LogLevel::WARN, "symlink(%s → %s): %s",
             outpath.c_str(), target.c_str(), ec.message().c_str());
        return false;
    }

    // Sanity-check: some Android FUSE implementations return 0 from symlink()
    // but silently discard the link (no ENOSYS, no error — the call just does
    // nothing). Detect this by verifying the path actually exists afterward.
    // Use symlink_status() (= lstat semantics, doesn't follow the link) via
    // std::filesystem so no platform-specific lstat() call is needed here.
    {
        std::error_code ec2;
        (void)fs::symlink_status(outpath, ec2);
        if (ec2) {
            ++symlinks_skipped_;
            return true;
        }
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// extractDir
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::extractDir(u32 ino, const std::string& outpath, int depth)
{
    f2fs_node node;
    if (!readNode(ino, node)) return false;

    fs::create_directories(outpath);

    const f2fs_inode& inode = node.i;

    // ── Inline dentries ───────────────────────────────────────────────────
    if (inode.i_inline & F2FS_INLINE_DENTS) {
        return processInlineDents(inode, outpath, depth);
    }

    // ── Regular directory: iterate data blocks ────────────────────────────
    const u64 dir_size   = inode.i_size;
    const u64 dir_blocks = (dir_size + blksize_ - 1) / blksize_;

    std::vector<u8> blkbuf(blksize_);

    for (u64 bi = 0; bi < dir_blocks; ++bi) {
        const u32 blkno = fileBlockAddr(inode, bi);
        if (blkno == NULL_ADDR || blkno == NEW_ADDR) continue;
        if (!readBlock(blkno, blkbuf.data())) continue;

        const auto& db = *reinterpret_cast<const f2fs_dentry_block*>(blkbuf.data());
        processDentryBlock(db, outpath, depth);
    }

    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// processDentryBlock — regular (4096-byte) directory data block
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::processDentryBlock(const f2fs_dentry_block& db,
                                       const std::string& outdir, int depth)
{
    return processDentryArray(db.dentry_bitmap, NR_DENTRY_IN_BLOCK,
                              db.dentry,
                              reinterpret_cast<const u8*>(db.filename),
                              F2FS_SLOT_LEN,
                              outdir, depth);
}

// ════════════════════════════════════════════════════════════════════════════
// processInlineDents — dentries stored inside the inode itself
//
// Layout (starting at inlineDataPtr):
//   [0 .. bitmap_size-1]                              bitmap
//   [bitmap_size .. bitmap_size+reserved_size-1]      reserved padding
//   [bitmap_size+reserved_size .. +nr*SIZE_DIR_ENTRY] dentry array
//   [... .. +nr*F2FS_SLOT_LEN]                        filename slots
//
// reserved_size = max_inline_data - bitmap_size - nr*(SIZE_DIR_ENTRY+SLOT_LEN)
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::processInlineDents(const f2fs_inode& inode,
                                       const std::string& outdir, int depth)
{
    const u64 max_data   = inlineDataBytes(inode);
    if (max_data == 0) return true;

    // NR_INLINE_DENTRY = floor(max_data * 8 / ((SIZE_DIR_ENTRY + SLOT_LEN) * 8 + 1))
    const u32 nr = (u32)(max_data * 8 /
                         ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * 8 + 1));
    if (nr == 0) return true;

    const u32 bitmap_sz   = (nr + 7) / 8;
    const u32 dentry_sz   = nr * SIZE_OF_DIR_ENTRY;
    const u32 filename_sz = nr * F2FS_SLOT_LEN;
    const u32 reserved_sz = (u32)max_data - bitmap_sz - dentry_sz - filename_sz;

    const u8* base      = inlineDataPtr(inode);
    const u8* bitmap    = base;
    const u8* dent_raw  = base + bitmap_sz + reserved_sz;
    const u8* fname_raw = dent_raw + dentry_sz;

    return processDentryArray(bitmap, nr,
                              reinterpret_cast<const f2fs_dir_entry*>(dent_raw),
                              fname_raw, F2FS_SLOT_LEN,
                              outdir, depth);
}

// ════════════════════════════════════════════════════════════════════════════
// processDentryArray — shared code for block and inline dentries
// ════════════════════════════════════════════════════════════════════════════

bool F2FSExtractor::processDentryArray(const u8*              bitmap,
                                       u32                    nr,
                                       const f2fs_dir_entry*  dentries,
                                       const u8*              filenames,
                                       u32                    slot_len,
                                       const std::string&     outdir,
                                       int                    depth)
{
    for (u32 i = 0; i < nr; ) {
        // Check validity bit.
        const u32 byte = i / 8, bit = i % 8;
        if (!(bitmap[byte] & (1u << bit))) {
            ++i;
            continue;
        }

        const f2fs_dir_entry& de = dentries[i];
        const u32  ino      = de.ino;
        const u16  name_len = de.name_len;
        const u8   ftype    = de.file_type;

        if (ino == 0 || name_len == 0 || name_len > F2FS_NAME_LEN) {
            ++i;
            continue;
        }

        // A long name spans multiple consecutive slots.
        const u32 slots = (name_len + slot_len - 1) / slot_len;

        // Collect name from filename slots.
        std::string name;
        name.reserve(name_len);
        for (u32 s = 0; s < slots && (i + s) < nr; ++s) {
            const u32 sidx = i + s;
            const u32 rem  = name_len - (u32)name.size();
            const u32 take = std::min(rem, slot_len);
            name.append(reinterpret_cast<const char*>(filenames + sidx * slot_len), take);
        }

        if (name != "." && name != "..") {
            const std::string dst = outdir + "/" + name;
            if (!extractEntry(ino, ftype, dst, depth + 1)) {
                logf(LogLevel::WARN, "  Failed: %s", dst.c_str());
            }
        }

        i += slots;
    }

    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// XAttr reading
// ════════════════════════════════════════════════════════════════════════════

XAttrMap F2FSExtractor::parseXAttrBuf(const uint8_t* data, size_t size) const
{
    return parseXAttrBlock(data, size);
}

XAttrMap F2FSExtractor::readXAttrs(const f2fs_inode& inode) const
{
    XAttrMap result;

    // ── Inline xattrs ────────────────────────────────────────────────────────
    // Stored in the LAST xattr_slots elements of i_addr[]:
    //   i_addr[DEF_ADDRS_PER_INODE - xattr_slots .. DEF_ADDRS_PER_INODE - 1]
    if ((inode.i_inline & F2FS_INLINE_XATTR) && xattrSlots(inode) > 0) {
        const int slots = xattrSlots(inode);
        // Byte offset of inline xattr region inside node block
        const auto* xattr_ptr = reinterpret_cast<const uint8_t*>(
            inode.i_addr + DEF_ADDRS_PER_INODE - slots);
        const size_t xattr_sz = static_cast<size_t>(slots) * sizeof(le32);

        auto xm = parseXAttrBuf(xattr_ptr, xattr_sz);
        result.insert(xm.begin(), xm.end());
    }

    // ── External xattr node ──────────────────────────────────────────────────
    if (inode.i_xattr_nid != 0) {
        f2fs_node xn;
        if (readNode(inode.i_xattr_nid, xn)) {
            // The full 4096-byte node block is the xattr payload.
            auto xm = parseXAttrBuf(reinterpret_cast<const uint8_t*>(&xn),
                                    sizeof(xn));
            result.insert(xm.begin(), xm.end());
        }
    }

    return result;
}

// ════════════════════════════════════════════════════════════════════════════
// buildMetadata
// ════════════════════════════════════════════════════════════════════════════

FileMetadata F2FSExtractor::buildMetadata(const std::string& relpath,
                                           const f2fs_inode&  inode,
                                           const XAttrMap&    xm) const
{
    FileMetadata m;
    m.path   = relpath;
    m.mode   = inode.i_mode;
    m.uid    = inode.i_uid;
    m.gid    = inode.i_gid;
    m.size   = inode.i_size;
    m.xattrs = xm;

    // ── Parse security.selinux ───────────────────────────────────────────────
    auto it_sel = xm.find("security.selinux");
    if (it_sel != xm.end()) {
        const auto& v = it_sel->second;
        size_t len = v.size();
        while (len > 0 && v[len-1] == '\0') --len;
        m.selinux_label = std::string(
            reinterpret_cast<const char*>(v.data()), len);
    }

    // ── Parse security.capability (vfs_cap_data, 20 bytes) ───────────────────
    auto it_cap = xm.find("security.capability");
    if (it_cap != xm.end()) {
        const auto& v = it_cap->second;
        if (v.size() >= sizeof(vfs_cap_data)) {
            const auto* cap = reinterpret_cast<const vfs_cap_data*>(v.data());
            const uint32_t rev = cap->magic_etc & VFS_CAP_REVISION_MASK;
            if (rev == VFS_CAP_REVISION_2 || rev == VFS_CAP_REVISION_3) {
                m.capabilities = (uint64_t)cap->data[0].permitted
                                | ((uint64_t)cap->data[1].permitted << 32);
                m.has_caps     = true;
            }
        }
    }

    // ── F2FS-specific attributes ─────────────────────────────────────────────

    // Transparent compression
    if (inode.i_flags & F2FS_COMPR_FL) {
        m.f2fs_compressed   = true;
        m.compress_algo     = inode.i_compress_algorithm;
        m.log_cluster_size  = inode.i_log_cluster_size;
        m.compr_blocks      = inode.i_compr_blocks;
        m.compress_released = (inode.i_inline & F2FS_COMPRESS_RELEASED) != 0;
    }

    // Encryption
    m.f2fs_encrypted    = (inode.i_flags & F2FS_ENCRYPT_FL) != 0;

    // Inline data: both INLINE_DATA and DATA_EXIST must be set for data to
    // actually be present inside the inode. Exclude symlinks: their target
    // is trivially always stored "inline" in F2FS for short paths, so the
    // flag carries no useful information and would clutter f2fs_special.txt.
    m.f2fs_inline_data  = S_ISREG(inode.i_mode)
                       && (inode.i_inline & F2FS_INLINE_DATA) != 0
                       && (inode.i_inline & F2FS_DATA_EXIST)  != 0;

    // Inline dentries (small directories whose dentry table lives in the inode)
    m.f2fs_inline_dents = (inode.i_inline & F2FS_INLINE_DENTS) != 0;

    // Pinned: GC will not relocate this file's blocks
    m.f2fs_pinned       = (inode.i_inline & F2FS_PIN_FILE) != 0;

    return m;
}

// ════════════════════════════════════════════════════════════════════════════
// collectMeta — called from extractEntry after extraction
// ════════════════════════════════════════════════════════════════════════════

void F2FSExtractor::collectMeta(const std::string& relpath,
                                const f2fs_inode& inode)
{
    auto xm = readXAttrs(inode);
    auto md = buildMetadata(relpath, inode, xm);

    // Capture symlink target if applicable
    if (S_ISLNK(inode.i_mode)) {
        // Reuse the inline-data reading path to get the target string
        if (inode.i_inline & F2FS_INLINE_DATA) {
            const u8* ptr  = inlineDataPtr(inode);
            const u64 sz   = std::min(inode.i_size, inlineDataBytes(inode));
            md.symlink_target = std::string(
                reinterpret_cast<const char*>(ptr), sz);
        } else {
            const u32 blkno = fileBlockAddr(inode, 0);
            if (blkno != NULL_ADDR) {
                std::vector<u8> buf(blksize_);
                if (readBlock(blkno, buf.data())) {
                    md.symlink_target = std::string(
                        reinterpret_cast<const char*>(buf.data()),
                        std::min(inode.i_size, (u64)blksize_));
                }
            }
        }
    }

    meta_.add(std::move(md));
}

// ════════════════════════════════════════════════════════════════════════════
// applyTimestamps
//
// Stamps the just-extracted file/dir/symlink with the original inode's
// atime/mtime, read directly from the F2FS image. This runs unconditionally
// during extraction (independent of -m / collect_metadata_), since it
// modifies the extracted filesystem entry itself rather than a metadata
// sidecar file.
//
// ctime is intentionally NOT set: it is not user-settable on POSIX (the
// kernel always stamps it with the time of the most recent metadata change,
// which here is "now" — the moment we call utimensat/symlink/etc.).
//
// Nanosecond fields are sanitised: some images carry garbage > 999999999
// in these slots, which would make utimensat() fail outright.
// ════════════════════════════════════════════════════════════════════════════

void F2FSExtractor::applyTimestamps(const std::string& outpath,
                                    const f2fs_inode&  inode) const
{
#if defined(_WIN32) || defined(__MINGW32__)
    // Windows/MinGW: second-resolution via _utime64(). Symlinks are rare on
    // this platform and _wutime64 has no NOFOLLOW equivalent, so we accept
    // following the link target here (matches typical Windows tooling).
    // Use the system-provided __utimbuf64 directly — no local redeclaration,
    // no casting, no risk of a single/double-underscore typo mismatch.
    struct __utimbuf64 times;
    times.actime  = (__time64_t)(int64_t)inode.i_atime;
    times.modtime = (__time64_t)(int64_t)inode.i_mtime;
    if (_utime64(outpath.c_str(), &times) != 0) {
        logf(LogLevel::WARN, "_utime64(%s): %s", outpath.c_str(), strerror(errno));
    }
#else
    auto sane_ns = [](u32 ns) -> long { return (ns <= 999999999u) ? (long)ns : 0L; };

    struct timespec times[2];
    times[0].tv_sec  = (time_t)(int64_t)inode.i_atime;        // access time
    times[0].tv_nsec = sane_ns(inode.i_atime_nsec);
    times[1].tv_sec  = (time_t)(int64_t)inode.i_mtime;        // modification time
    times[1].tv_nsec = sane_ns(inode.i_mtime_nsec);

    // AT_SYMLINK_NOFOLLOW: for symlinks, stamp the link itself, not its target.
    if (::utimensat(AT_FDCWD, outpath.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
        // ENOENT: the path doesn't exist — this happens when a symlink was
        // silently discarded by FUSE (symlink() returned 0 but didn't create
        // the link). Detected and counted in extractSymlink(); no need to warn
        // again here since the summary message covers it.
        if (errno != ENOENT)
            logf(LogLevel::WARN, "utimensat(%s): %s", outpath.c_str(), strerror(errno));
    }
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// printInfo
// ════════════════════════════════════════════════════════════════════════════

void F2FSExtractor::printInfo() const
{
    logf(LogLevel::INFO, "─── F2FS superblock ───────────────────────────");
    logf(LogLevel::INFO, "  Version      : %u.%u",
         (u32)sb_.major_ver, (u32)sb_.minor_ver);
    logf(LogLevel::INFO, "  Block size   : %u", blksize_);
    logf(LogLevel::INFO, "  Blocks/seg   : %u", blocks_per_seg_);
    logf(LogLevel::INFO, "  Total blocks : %llu", (unsigned long long)sb_.block_count);
    logf(LogLevel::INFO, "  root_ino     : %u", (u32)sb_.root_ino);
    logf(LogLevel::INFO, "  nat_blkaddr  : 0x%X", (u32)sb_.nat_blkaddr);
    logf(LogLevel::INFO, "  main_blkaddr : 0x%X", (u32)sb_.main_blkaddr);
    logf(LogLevel::INFO, "  NAT entries  : %llu", (unsigned long long)nat_.size());
    logf(LogLevel::INFO, "───────────────────────────────────────────────");
}
