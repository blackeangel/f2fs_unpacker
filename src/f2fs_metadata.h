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
    std::string selinux_label;         // security.selinux, null-terminator stripped
    bool        selinux_from_xattr = false; // true only when sourced from actual xattr

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

    // Encryption (FADVISE_ENCRYPT_BIT in i_advise — see f2fs_fs.h for why
    // this is i_advise and not i_flags, contrary to the ext4-style naming)
    bool     f2fs_encrypted      = false;

    // Decoded fscrypt_context xattr (index 9, name "c"), if present and
    // recognized. Format: "v2,contents=aes-256-xts,names=aes-256-cts,
    // key_id=<32 hex chars>,nonce=<32 hex chars>". This is metadata
    // describing HOW the file is encrypted, NOT the encryption key itself
    // — the key is wrapped by hardware Keymaster/KeyMint and cannot be
    // recovered from a static image. Empty if f2fs_encrypted is false, or
    // if the context xattr is missing/unrecognized.
    std::string encryption_context;

    // fsverity: file content is integrity-protected and read-only enforced
    // by the kernel (FADVISE_VERITY_BIT in i_advise)
    bool     f2fs_verity         = false;

    // Case-insensitive directory lookups (F2FS_CASEFOLD_FL in i_flags,
    // directories only)
    bool     f2fs_casefold       = false;

    // Project quota ID (only meaningful when F2FS_EXTRA_ATTR is set and
    // the value is non-default; 0 = not set / default project)
    uint32_t project_id          = 0;

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

    // Post-extraction enrichment: merge data from Android runtime config files
    // that are embedded inside the extracted partition tree.
    //
    // enrichFromFsConfigBinary() reads a compiled AOSP fs_config binary
    // (etc/fs_config_files or etc/fs_config_dirs) and merges uid/gid/mode/
    // capabilities into entries that were read from the F2FS image itself.
    // Capabilities are the primary target — Samsung Android 14 stores them
    // here rather than as security.capability xattrs.
    //
    // enrichFromFileContextsText() reads a text-format SELinux file_contexts
    // file (e.g. etc/selinux/plat_file_contexts) and fills in selinux_label
    // for entries that have no label yet (= not stored as xattr in the image).
    //
    // Both are no-ops (return false silently) if the file doesn't exist.
    bool enrichFromFsConfigBinary    (const std::string& bin_path);
    bool enrichFromFileContextsText  (const std::string& txt_path);

private:
    std::vector<FileMetadata> entries_;
    // Paths of embedded SELinux text context files found inside the image.
    // When file_contexts.txt is written and there are no xattr-based labels,
    // the first found file is copied verbatim (preserving all regex patterns
    // that sload.f2fs -s needs).
    std::vector<std::string>  embedded_ctx_files_;

    static std::string octalMode(uint16_t mode);
    static std::string typeChar (uint16_t mode);
};

// ────────────────────────────────────────────────────────────────────────────
// parseXAttrBlock
// ────────────────────────────────────────────────────────────────────────────
XAttrMap parseXAttrBlock(const uint8_t* data, size_t size);

// ────────────────────────────────────────────────────────────────────────────
// fscryptContextToString
//
// Decode a raw fscrypt_context_v1/v2 xattr value (as found under the
// "encryption.c" key in an entry's XAttrMap) into a human-readable summary.
// Returns "" if the input is empty, truncated, or an unrecognized version.
// ────────────────────────────────────────────────────────────────────────────
std::string fscryptContextToString(const std::vector<uint8_t>& raw);
