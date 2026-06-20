/**
 * f2fs_metadata.cpp
 *
 * XAttr parsing and metadata serialisation for f2fs_extract.
 *
 * Output formats
 * ──────────────
 * metadata.json
 *   One JSON object per entry. xattr values that are printable strings are
 *   stored as strings; binary values are base64-encoded.
 *
 * fs_config.txt
 *   Android AOSP "fs_config" format used by mkfs.ext4, simg2img, etc.:
 *       <path> <uid> <gid> <octal_mode> <capabilities_hex>
 *   Capabilities are the 64-bit permitted-capability bitmask in hex.
 *
 * file_contexts.txt
 *   SELinux labelling table consumed by restorecon / make_ext4fs:
 *       <path>  <label>
 *   Only entries with a non-empty security.selinux xattr are included.
 */

#include "f2fs_metadata.h"
#include "../include/f2fs_fs.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include "win_pread.h"  // POSIX stat-macro shims (S_ISLNK etc.) on Windows; passthrough on POSIX

namespace fs = std::filesystem;

// ════════════════════════════════════════════════════════════════════════════
// xattr name-index → string prefix
// ════════════════════════════════════════════════════════════════════════════

static const char* xattr_prefix(uint8_t index)
{
    switch (index) {
    case XATTR_INDEX_USER:              return "user.";
    case XATTR_INDEX_POSIX_ACL_ACCESS:  return "system.posix_acl_access";
    case XATTR_INDEX_POSIX_ACL_DEFAULT: return "system.posix_acl_default";
    case XATTR_INDEX_TRUSTED:           return "trusted.";
    case XATTR_INDEX_SECURITY:          return "security.";
    case XATTR_INDEX_SYSTEM:            return "system.";
    default:                            return "";
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

        // End marker: both lengths zero
        if (e->e_name_len == 0 && e->e_value_size == 0) break;

        const uint8_t* name_ptr  = ptr + sizeof(f2fs_xattr_entry);
        const uint8_t* value_ptr = name_ptr + e->e_name_len;
        const uint8_t* next_ptr  = reinterpret_cast<const uint8_t*>(
            xattr_next_entry(e));

        // Bounds check
        if (value_ptr > end || value_ptr + e->e_value_size > end) break;
        if (next_ptr  > end + 4) break;   // tolerate 1-entry overread

        // Build full attribute name
        std::string name = xattr_prefix(e->e_name_index);
        // For index types where prefix IS the full name (ACL), append nothing;
        // otherwise append the stored name suffix.
        if (e->e_name_index != XATTR_INDEX_POSIX_ACL_ACCESS &&
            e->e_name_index != XATTR_INDEX_POSIX_ACL_DEFAULT) {
            name.append(reinterpret_cast<const char*>(name_ptr), e->e_name_len);
        }

        out.emplace(name,
            std::vector<uint8_t>(value_ptr, value_ptr + e->e_value_size));

        ptr = next_ptr;
    }
    return out;
}

// ════════════════════════════════════════════════════════════════════════════
// MetadataWriter helpers
// ════════════════════════════════════════════════════════════════════════════

std::string MetadataWriter::jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string MetadataWriter::base64Encode(const uint8_t* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i+1] << 8;
        if (i + 2 < len) b |= data[i+2];
        out += B64[(b >> 18) & 0x3F];
        out += B64[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? B64[(b >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64[ b       & 0x3F] : '=';
    }
    return out;
}

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

// Test whether a byte slice is valid UTF-8 and printable (no control chars).
static bool isPrintableUtf8(const uint8_t* data, size_t len)
{
    if (len == 0) return true;
    // Null-terminated strings from SELinux end with \0 — allow that.
    size_t check_len = (len > 0 && data[len-1] == 0) ? len - 1 : len;
    for (size_t i = 0; i < check_len; ++i) {
        uint8_t c = data[i];
        if (c < 0x20 && c != '\t') return false;
        if (c == 0x7F) return false;
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// writeJSON
// ════════════════════════════════════════════════════════════════════════════

bool MetadataWriter::writeJSON(const std::string& path) const
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[ERR] Cannot open %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }

    fputs("{\n  \"format\": \"f2fs_extract_metadata\",\n  \"version\": 1,\n", f);
    fprintf(f, "  \"entry_count\": %llu,\n", (unsigned long long)entries_.size());
    fputs("  \"entries\": [\n", f);

    for (size_t idx = 0; idx < entries_.size(); ++idx) {
        const auto& m = entries_[idx];
        const bool last = (idx + 1 == entries_.size());

        fputs("    {\n", f);
        fprintf(f, "      \"path\":  \"%s\",\n",   jsonEscape(m.path).c_str());
        fprintf(f, "      \"type\":  \"%s\",\n",   typeChar(m.mode).c_str());
        fprintf(f, "      \"mode\":  \"%s\",\n",   octalMode(m.mode).c_str());
        fprintf(f, "      \"uid\":   %u,\n",       m.uid);
        fprintf(f, "      \"gid\":   %u,\n",       m.gid);
        fprintf(f, "      \"size\":  %llu,\n",     (unsigned long long)m.size);

        if (!m.symlink_target.empty())
            fprintf(f, "      \"symlink_target\": \"%s\",\n",
                    jsonEscape(m.symlink_target).c_str());

        if (!m.selinux_label.empty())
            fprintf(f, "      \"selinux\": \"%s\",\n",
                    jsonEscape(m.selinux_label).c_str());

        if (m.has_caps)
            fprintf(f, "      \"capabilities\": \"0x%016llx\",\n",
                    (unsigned long long)m.capabilities);

        // xattrs object
        fputs("      \"xattrs\": {", f);
        if (!m.xattrs.empty()) {
            fputs("\n", f);
            size_t xi = 0;
            for (const auto& [name, val] : m.xattrs) {
                const bool xlast = (++xi == m.xattrs.size());
                // Print as string if printable, else base64
                if (isPrintableUtf8(val.data(), val.size())) {
                    std::string sv(reinterpret_cast<const char*>(val.data()),
                                   val.size());
                    // strip trailing null
                    while (!sv.empty() && sv.back() == '\0') sv.pop_back();
                    fprintf(f, "        \"%s\": \"%s\"%s\n",
                            jsonEscape(name).c_str(),
                            jsonEscape(sv).c_str(),
                            xlast ? "" : ",");
                } else {
                    fprintf(f, "        \"%s\": {\"b64\": \"%s\"}%s\n",
                            jsonEscape(name).c_str(),
                            base64Encode(val.data(), val.size()).c_str(),
                            xlast ? "" : ",");
                }
            }
            fputs("      }", f);
        } else {
            fputs("}", f);
        }
        fprintf(f, "\n    }%s\n", last ? "" : ",");
    }

    fputs("  ]\n}\n", f);
    fclose(f);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// writeFsConfig
//
// Format (one line per entry, sorted alphabetically by path):
//   <path> <uid> <gid> <octal_mode> <capabilities_hex>
//
// The root directory is represented as "/".
// Capabilities of zero are written as "0".
// ════════════════════════════════════════════════════════════════════════════

bool MetadataWriter::writeFsConfig(const std::string& path) const
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[ERR] Cannot open %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }

    // Sort entries by path for deterministic output.
    std::vector<const FileMetadata*> sorted;
    sorted.reserve(entries_.size());
    for (const auto& m : entries_) sorted.push_back(&m);
    std::sort(sorted.begin(), sorted.end(),
              [](const FileMetadata* a, const FileMetadata* b) {
                  return a->path < b->path;
              });

    fputs("# fs_config — generated by f2fs_extract\n", f);
    fputs("# format: path uid gid mode capabilities\n", f);

    for (const auto* m : sorted) {
        // fs_config uses the path without leading slash except for "/"
        std::string p = m->path;
        if (p.empty()) p = "/";

        char caps_str[32];
        if (m->has_caps && m->capabilities)
            snprintf(caps_str, sizeof(caps_str), "0x%016llx",
                     (unsigned long long)m->capabilities);
        else
            strcpy(caps_str, "0");

        fprintf(f, "%s %u %u %s %s\n",
                p.c_str(), m->uid, m->gid,
                octalMode(m->mode).c_str(),
                caps_str);
    }

    fclose(f);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// writeFileContexts
//
// Format (entries with a security.selinux label only):
//   <path>    <label>
// ════════════════════════════════════════════════════════════════════════════

bool MetadataWriter::writeFileContexts(const std::string& path) const
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[ERR] Cannot open %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }

    fputs("# file_contexts — generated by f2fs_extract\n", f);
    fputs("# format: path  selinux_label\n", f);

    std::vector<const FileMetadata*> sorted;
    for (const auto& m : entries_)
        if (!m.selinux_label.empty()) sorted.push_back(&m);

    std::sort(sorted.begin(), sorted.end(),
              [](const FileMetadata* a, const FileMetadata* b) {
                  return a->path < b->path;
              });

    for (const auto* m : sorted)
        fprintf(f, "%s\t%s\n", m->path.c_str(), m->selinux_label.c_str());

    fclose(f);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// writeAll — write all three files into the given directory
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
    ok &= writeJSON        (dir + "/metadata.json");
    ok &= writeFsConfig    (dir + "/fs_config.txt");
    ok &= writeFileContexts(dir + "/file_contexts.txt");

    if (ok)
        fprintf(stdout, "Metadata written to %s/\n", dir.c_str());
    return ok;
}
