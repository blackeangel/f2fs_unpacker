#pragma once
/**
 * f2fs_metadata.h
 *
 * Collected per-entry metadata and the writer that serialises it into
 * three formats used by Android firmware toolchains:
 *
 *   metadata.json      — mode, uid/gid, all xattrs
 *   fs_config.txt      — AOSP "fs_config" format:  path uid gid mode caps
 *   file_contexts.txt  — SELinux labelling:         path  label
 *
 * Timestamps are NOT recorded here — they are applied directly to the
 * extracted files' atime/mtime during extraction (see applyTimestamps()
 * in F2FSExtractor), so the filesystem itself carries that information.
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

    // ── Core inode fields ───────────────────────────────────────────────────
    uint16_t    mode        = 0;    // full st_mode (type bits + permissions)
    uint32_t    uid         = 0;
    uint32_t    gid         = 0;
    uint64_t    size        = 0;

    // ── Symlink ─────────────────────────────────────────────────────────────
    std::string symlink_target;     // non-empty only for symbolic links

    // ── Parsed security attributes ───────────────────────────────────────────
    std::string selinux_label;      // security.selinux (null-term stripped)
    uint64_t    capabilities = 0;   // Linux caps bitmask from vfs_cap_data
    bool        has_caps     = false;

    // ── All raw xattrs ───────────────────────────────────────────────────────
    XAttrMap    xattrs;             // full "prefix.name" → raw bytes
};

// ────────────────────────────────────────────────────────────────────────────
// MetadataWriter
// ────────────────────────────────────────────────────────────────────────────
class MetadataWriter {
public:
    void add(FileMetadata md) { entries_.push_back(std::move(md)); }
    size_t size() const { return entries_.size(); }

    // Write all three output files into `dir`.
    // Returns false if any write fails.
    bool writeAll(const std::string& dir) const;

    // Individual writers (path = full file path, not directory)
    bool writeJSON        (const std::string& path) const;
    bool writeFsConfig    (const std::string& path) const;
    bool writeFileContexts(const std::string& path) const;

private:
    std::vector<FileMetadata> entries_;

    // Helpers
    static std::string jsonEscape(const std::string& s);
    static std::string base64Encode(const uint8_t* data, size_t len);
    static std::string octalMode(uint16_t mode);   // e.g. "0755"
    static std::string typeChar(uint16_t mode);    // "file"/"dir"/"symlink"/…
};

// ────────────────────────────────────────────────────────────────────────────
// parseXAttrBlock — parse a raw F2FS xattr block into an XAttrMap
//
// @data  pointer to the start of the block (f2fs_xattr_header at offset 0)
// @size  number of bytes available
// ────────────────────────────────────────────────────────────────────────────
XAttrMap parseXAttrBlock(const uint8_t* data, size_t size);
