#pragma once
/**
 * f2fs_metadata.h
 *
 * Collected per-entry metadata and the writer that serialises it into
 * three files:
 *
 *   fs_config.txt       — AOSP "fs_config" format:  path uid gid mode caps
 *   file_contexts.txt   — SELinux labelling:         path  label
 *   f2fs_special.txt    — F2FS-specific per-file attributes not covered
 *                         by the two files above:
 *                         compression, encryption, inline data, pinned, etc.
 *
 * Timestamps are NOT stored here — they are applied directly to extracted
 * files via utimensat() during extraction.
 */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// ────────────────────────────────────────────────────────────────────────────
// XAttrMap — raw extended attributes (full "prefix.name" → bytes)
// ────────────────────────────────────────────────────────────────────────────
using XAttrMap = std::map<std::string, std::vector<uint8_t>>;

// ────────────────────────────────────────────────────────────────────────────
// FileMetadata — one inode's worth of metadata
// ────────────────────────────────────────────────────────────────────────────
struct FileMetadata {
    // ── Path ────────────────────────────────────────────────────────────────
    std::string path;               // root-relative, e.g. "/system/bin/sh"

    // ── Core inode fields (→ fs_config.txt) ─────────────────────────────────
    uint16_t    mode        = 0;    // full st_mode (type bits + permissions)
    uint32_t    uid         = 0;
    uint32_t    gid         = 0;
    uint64_t    size        = 0;

    // ── Symlink target ───────────────────────────────────────────────────────
    std::string symlink_target;

    // ── SELinux (→ file_contexts.txt) ────────────────────────────────────────
    std::string selinux_label;      // security.selinux, null-terminator stripped

    // ── Linux capabilities (→ fs_config.txt caps column) ────────────────────
    uint64_t    capabilities = 0;
    bool        has_caps     = false;

    // ── All raw xattrs ───────────────────────────────────────────────────────
    XAttrMap    xattrs;

    // ── F2FS-specific attributes (→ f2fs_special.txt) ────────────────────────

    // Transparent compression (F2FS_COMPR_FL in i_flags)
    bool     f2fs_compressed     = false;
    uint8_t  compress_algo       = 0;   // 0=lzo 1=lz4 2=zstd 3=lzorle
    uint8_t  log_cluster_size    = 0;   // cluster = (1 << log_cluster_size) pages
    uint64_t compr_blocks        = 0;   // i_compr_blocks: saved blocks count
    bool     compress_released   = false; // F2FS_COMPRESS_RELEASED: blocks freed

    // Encryption (F2FS_ENCRYPT_FL in i_flags)
    bool     f2fs_encrypted      = false;

    // Inline storage
    bool     f2fs_inline_data    = false; // file content stored inside inode
    bool     f2fs_inline_dents   = false; // dir dentries stored inside inode

    // Pinned (GC won't move this file)
    bool     f2fs_pinned         = false;
};

// ────────────────────────────────────────────────────────────────────────────
// MetadataWriter
// ────────────────────────────────────────────────────────────────────────────
class MetadataWriter {
public:
    void add(FileMetadata md) { entries_.push_back(std::move(md)); }
    size_t size() const { return entries_.size(); }

    // Write all three output files into `dir`.
    bool writeAll(const std::string& dir) const;

    bool writeFsConfig    (const std::string& path) const;
    bool writeFileContexts(const std::string& path) const;
    bool writeF2FSSpecial (const std::string& path) const;

private:
    std::vector<FileMetadata> entries_;

    static std::string octalMode(uint16_t mode);
    static std::string typeChar (uint16_t mode);
};

// ────────────────────────────────────────────────────────────────────────────
// parseXAttrBlock
// ────────────────────────────────────────────────────────────────────────────
XAttrMap parseXAttrBlock(const uint8_t* data, size_t size);
