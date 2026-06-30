/**
 * f2fs_metadata.cpp — F2FS metadata serialisation
 *
 * Three output files:
 *   fs_config.txt      — path uid gid mode caps (AOSP fs_config format)
 *   file_contexts.txt  — path label (SELinux, only entries with a label)
 *   f2fs_special.txt   — F2FS-specific per-file attributes
 *
 * f2fs_special.txt format (one line per entry that has at least one attribute):
 *
 *   <path>  <attr> [<attr> ...]
 *
 * Attributes:
 *   compress:algo=<lzo|lz4|zstd|lzorle>,log_cluster=<n>,saved_blocks=<n>[,released]
 *   encrypt
 *   inline_data
 *   inline_dents
 *   pinned
 */

#include "f2fs_metadata.h"
#include "../include/f2fs_fs.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include "win_pread.h"

namespace fs = std::filesystem;

// ════════════════════════════════════════════════════════════════════════════
// xattr name-index → string prefix
// ════════════════════════════════════════════════════════════════════════════

static const char* xattr_prefix(uint8_t index)
{
    switch (index) {
    case XATTR_INDEX_USER:                    return "user.";
    case XATTR_INDEX_POSIX_ACL_ACCESS:        return "system.posix_acl_access";
    case XATTR_INDEX_POSIX_ACL_DEFAULT:       return "system.posix_acl_default";
    case XATTR_INDEX_TRUSTED:                 return "trusted.";
    case XATTR_INDEX_SECURITY:                return "security.";
    case XATTR_INDEX_SYSTEM:                  return "system.";
    default:                                  return "";
    }
}

// ════════════════════════════════════════════════════════════════════════════
// parseXAttrBlock
// ════════════════════════════════════════════════════════════════════════════

XAttrMap parseXAttrBlock(const uint8_t* data, size_t size)
{
    XAttrMap out;
    if (!data || size < sizeof(f2fs_xattr_header)) return out;

    const auto* hdr = reinterpret_cast<const f2fs_xattr_header*>(data);
    if (hdr->h_magic != F2FS_XATTR_MAGIC) return out;

    const uint8_t* ptr = data + sizeof(f2fs_xattr_header);
    const uint8_t* end = data + size;

    while (ptr + sizeof(f2fs_xattr_entry) <= end) {
        const auto* e = reinterpret_cast<const f2fs_xattr_entry*>(ptr);
        if (e->e_name_len == 0 && e->e_value_size == 0) break;

        const uint8_t* name_ptr  = ptr + sizeof(f2fs_xattr_entry);
        const uint8_t* value_ptr = name_ptr + e->e_name_len;
        const uint8_t* next_ptr  = reinterpret_cast<const uint8_t*>(xattr_next_entry(e));

        if (value_ptr > end || value_ptr + e->e_value_size > end) break;
        if (next_ptr  > end + 4) break;

        std::string name = xattr_prefix(e->e_name_index);
        if (e->e_name_index != XATTR_INDEX_POSIX_ACL_ACCESS &&
            e->e_name_index != XATTR_INDEX_POSIX_ACL_DEFAULT) {
            name.append(reinterpret_cast<const char*>(name_ptr), e->e_name_len);
        }
        out.emplace(name, std::vector<uint8_t>(value_ptr, value_ptr + e->e_value_size));
        ptr = next_ptr;
    }
    return out;
}

// ════════════════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════════════════

std::string MetadataWriter::octalMode(uint16_t mode)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%04o", mode & 07777u);
    return buf;
}

std::string MetadataWriter::typeChar(uint16_t mode)
{
    if      (S_ISREG(mode))  return "file";
    else if (S_ISDIR(mode))  return "dir";
    else if (S_ISLNK(mode))  return "symlink";
    else if (S_ISBLK(mode))  return "block";
    else if (S_ISCHR(mode))  return "char";
    else if (S_ISFIFO(mode)) return "fifo";
    else if (S_ISSOCK(mode)) return "socket";
    return "unknown";
}

// ════════════════════════════════════════════════════════════════════════════
// writeFsConfig
//
// Canonical AOSP "canned_fs_config" format (system/core/libcutils/
// canned_fs_config.cpp), as consumed by build_image.py / mkuserimg_*.sh /
// an AOSP-tree-built sload.f2fs -C:
//
//   <path> <uid> <gid> <mode> [capabilities=0x...]
//
// Path is relative to the mount point with NO leading slash (root itself
// is the single exception, written as a bare "/"). Directories get a
// trailing "/". Capabilities is an OPTIONAL trailing key=value token,
// completely omitted when there are none — not a permanent "0" column.
//
// Verified against a real sample produced by an independent third-party
// unpacker on the same cache.f2fs image: identical conventions confirmed
// byte-for-byte (no leading slash, trailing slash on directories, no
// capabilities token when zero).
// ════════════════════════════════════════════════════════════════════════════

// Strip the leading slash for the AOSP canned_fs_config path convention.
// Root ("/") is kept as-is; everything else loses its leading "/".
static std::string aospRelPath(const std::string& path)
{
    if (path.empty() || path == "/") return "/";
    return (path[0] == '/') ? path.substr(1) : path;
}

bool MetadataWriter::writeFsConfig(const std::string& path) const
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[ERR] Cannot open %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }

    fputs("# fs_config — generated by f2fs_extract\n"
          "# format: path uid gid mode [capabilities=0x...]\n"
          "# AOSP canned_fs_config convention: no leading slash (except root),\n"
          "# directories end in '/', capabilities omitted when zero.\n", f);

    std::vector<const FileMetadata*> sorted;
    sorted.reserve(entries_.size());
    for (const auto& m : entries_) sorted.push_back(&m);
    std::sort(sorted.begin(), sorted.end(),
              [](const FileMetadata* a, const FileMetadata* b){
                  return a->path < b->path;
              });

    for (const auto* m : sorted) {
        std::string p = aospRelPath(m->path);
        if (p != "/" && S_ISDIR(m->mode) && (p.empty() || p.back() != '/'))
            p += '/';

        fprintf(f, "%s %u %u %s",
                p.c_str(), m->uid, m->gid, octalMode(m->mode).c_str());

        if (m->has_caps && m->capabilities)
            fprintf(f, " capabilities=0x%016llx",
                    (unsigned long long)m->capabilities);

        fputc('\n', f);
    }

    fclose(f);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// writeFileContexts
//
// SELinux file_contexts format consumed by libselinux's selabel_open()
// (PCRE-backed): "<path_regex>\t<label>" per line, path anchored absolute
// (leading slash kept, unlike fs_config.txt's relative convention — these
// are two distinct AOSP formats with different path conventions).
//
// Only entries with an actual security.selinux xattr are emitted — this
// tool extracts real labels found in the image, it does not fabricate
// regex-wildcard skeleton entries for paths that carry no label data.
// (A reference third-party tool run on the same image emitted unlabelled
// path-pattern entries for every file as a fill-in-the-blanks template;
// that's a different design goal — useful as a starting point for manual
// policy authoring — and is easy to derive from fs_config.txt's path list
// separately if that workflow is wanted.)
//
// Regex metacharacters in the path (most commonly '.') are escaped so the
// pattern matches the literal filename rather than "any character" --
// matches the convention observed in a real AOSP-style file_contexts
// sample: "\.version" not ".version".
// ════════════════════════════════════════════════════════════════════════════

static std::string escapeRegex(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '.': case '+': case '*': case '?': case '(': case ')':
        case '[': case ']': case '{': case '}': case '^': case '$':
        case '|': case '\\':
            out += '\\';
            [[fallthrough]];
        default:
            out += c;
        }
    }
    return out;
}

bool MetadataWriter::writeFileContexts(const std::string& path) const
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[ERR] Cannot open %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }

    fputs("# file_contexts — generated by f2fs_extract\n"
          "# format: path_regex  selinux_label\n"
          "# Only entries with an actual security.selinux xattr are listed.\n", f);

    std::vector<const FileMetadata*> sorted;
    for (const auto& m : entries_)
        if (!m.selinux_label.empty()) sorted.push_back(&m);
    std::sort(sorted.begin(), sorted.end(),
              [](const FileMetadata* a, const FileMetadata* b){
                  return a->path < b->path;
              });

    for (const auto* m : sorted)
        fprintf(f, "%s\t%s\n", escapeRegex(m->path).c_str(), m->selinux_label.c_str());

    fclose(f);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// writeF2FSSpecial  — F2FS-specific per-file attributes
// ════════════════════════════════════════════════════════════════════════════

bool MetadataWriter::writeF2FSSpecial(const std::string& path) const
{
    static const char* algo_name[] = {"lzo", "lz4", "zstd", "lzorle"};

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[ERR] Cannot open %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }

    fputs("# f2fs_special — F2FS-specific per-file attributes\n"
          "# Generated by f2fs_extract\n"
          "# format: path  attribute  [attribute ...]\n"
          "#\n"
          "# Attributes:\n"
          "#   symlink:target=<path>  — symlink destination (critical on FUSE/exFAT where\n"
          "#                            symlinks cannot be created during extraction)\n"
          "#   compress:algo=<lzo|lz4|zstd|lzorle>,log_cluster=<n>,saved_blocks=<n>[,released]\n"
          "#     log_cluster: cluster = (1 << n) pages = (1 << n) x 4096 bytes\n"
          "#     saved_blocks: number of 4K blocks saved by compression\n"
          "#     released: compressed blocks were freed (COMPRESS_RELEASED flag)\n"
          "#   encrypt      — file content is encrypted (FBE)\n"
          "#   inline_data  — regular file content stored inside the inode block\n"
          "#   inline_dents — directory entries stored inside the inode block\n"
          "#   pinned       — file pinned in place, GC will not move it\n"
          "#\n"
          "# Only entries with at least one attribute are listed.\n",
          f);

    std::vector<const FileMetadata*> sorted;
    for (const auto& m : entries_) {
        const bool is_symlink = !m.symlink_target.empty();
        if (is_symlink || m.f2fs_compressed || m.f2fs_encrypted ||
            m.f2fs_inline_data || m.f2fs_inline_dents || m.f2fs_pinned)
            sorted.push_back(&m);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const FileMetadata* a, const FileMetadata* b){
                  return a->path < b->path;
              });

    for (const auto* m : sorted) {
        std::string p = m->path.empty() ? "/" : m->path;
        fprintf(f, "%s", p.c_str());

        // Symlink target — first so it's easy to grep/parse
        if (!m->symlink_target.empty())
            fprintf(f, "\tsymlink:target=%s", m->symlink_target.c_str());

        if (m->f2fs_compressed) {
            const char* aname = (m->compress_algo < 4)
                                ? algo_name[m->compress_algo] : "unknown";
            fprintf(f, "\tcompress:algo=%s,log_cluster=%u,saved_blocks=%llu%s",
                    aname,
                    (unsigned)m->log_cluster_size,
                    (unsigned long long)m->compr_blocks,
                    m->compress_released ? ",released" : "");
        }
        if (m->f2fs_encrypted)    fputs("\tencrypt",      f);
        if (m->f2fs_inline_data)  fputs("\tinline_data",  f);
        if (m->f2fs_inline_dents) fputs("\tinline_dents", f);
        if (m->f2fs_pinned)       fputs("\tpinned",       f);

        fputc('\n', f);
    }

    fclose(f);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// writeAll
// ════════════════════════════════════════════════════════════════════════════

bool MetadataWriter::writeAll(const std::string& dir) const
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        fprintf(stderr, "[ERR] Cannot create metadata dir %s: %s\n",
                dir.c_str(), ec.message().c_str());
        return false;
    }

    bool ok = true;
    ok &= writeFsConfig    (dir + "/fs_config.txt");
    ok &= writeFileContexts(dir + "/file_contexts.txt");
    ok &= writeF2FSSpecial (dir + "/f2fs_special.txt");

    if (ok)
        fprintf(stdout, "Metadata written to %s/\n", dir.c_str());
    return ok;
}
