/**
 * f2fs_pack_fsconfig.c
 *
 * Reads our own fs_config.txt (AOSP canned_fs_config text format, written by
 * f2fs_extract's MetadataWriter::writeFsConfig) and exposes a function
 * matching sload.c's `fs_config_f` typedef:
 *
 *   typedef void (*fs_config_f)(const char *path, int dir,
 *                               const char *target_out_path,
 *                               unsigned *uid, unsigned *gid,
 *                               unsigned *mode, uint64_t *capabilities);
 *
 * This replaces AOSP's own canned_fs_config() (system/core/libcutils/
 * canned_fs_config.cpp), which requires <private/android_filesystem_config.h>
 * and <private/canned_fs_config.h> — real AOSP-toolchain-only headers we
 * deliberately don't vendor (see CMakeLists.txt comment on HAVE_SELINUX_ANDROID_H
 * for the parallel reasoning on the SELinux side).
 *
 * File format, one entry per line (see f2fs_metadata.cpp's writeFsConfig):
 *   <path> <uid> <gid> <mode-octal> [capabilities=0x<hex>]
 * Path has no leading slash except literal root ("/"); directories have a
 * trailing slash. Lines starting with '#' are comments.
 *
 * Also provides f2fs_pack_set_capabilities() (AOSP's own sload.c reads
 * de->capabilities via fs_config_func but never actually writes it as a
 * security.capability xattr anywhere in this codebase — see that function's
 * own comment below) and f2fs_pack_load_symlinks() /
 * f2fs_pack_find_missing_symlinks() (recovers symlinks the extractor
 * recorded but couldn't physically create on FUSE/exFAT filesystems).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fsck.h"

typedef struct {
    char     *path;
    unsigned  uid;
    unsigned  gid;
    unsigned  mode;
    uint64_t  capabilities;
} F2fsPackFsConfigEntry;

static F2fsPackFsConfigEntry *g_fsc_entries   = NULL;
static size_t                 g_fsc_count     = 0;
static size_t                 g_fsc_capacity  = 0;

/* Returns 0 on success (including "file has zero usable entries", which is
 * valid — an empty/comment-only fs_config.txt just means every dentry falls
 * through to sload's built-in stat()-derived defaults), -1 if the file
 * couldn't be opened at all. */
int f2fs_pack_load_fs_config(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f)
        return -1;

    g_fsc_capacity = 256;
    g_fsc_entries  = (F2fsPackFsConfigEntry *)
        malloc(g_fsc_capacity * sizeof(F2fsPackFsConfigEntry));
    g_fsc_count = 0;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        char path[2048];
        unsigned uid, gid, mode;
        char rest[512] = {0};
        /* rest captures anything after the mode column, e.g.
         * "capabilities=0x1000" — sscanf's %511[^\n] grabs the remainder
         * of the line whether or not a capabilities token is present. */
        int n = sscanf(p, "%2047s %u %u %o %511[^\r\n]",
                        path, &uid, &gid, &mode, rest);
        if (n < 4)
            continue;   /* malformed line — skip rather than abort the load */

        uint64_t caps = 0;
        const char *capstr = strstr(rest, "capabilities=");
        if (capstr)
            caps = strtoull(capstr + strlen("capabilities="), NULL, 0);

        if (g_fsc_count >= g_fsc_capacity) {
            g_fsc_capacity *= 2;
            g_fsc_entries = (F2fsPackFsConfigEntry *)
                realloc(g_fsc_entries, g_fsc_capacity * sizeof(F2fsPackFsConfigEntry));
        }

        g_fsc_entries[g_fsc_count].path         = strdup(path);
        g_fsc_entries[g_fsc_count].uid          = uid;
        g_fsc_entries[g_fsc_count].gid          = gid;
        g_fsc_entries[g_fsc_count].mode         = mode;
        g_fsc_entries[g_fsc_count].capabilities = caps;
        ++g_fsc_count;
    }

    fclose(f);
    fprintf(stderr, "[f2fs_pack] loaded %zu fs_config entries\n", g_fsc_count);
    return 0;
}

static const F2fsPackFsConfigEntry *fsc_find(const char *path, int dir)
{
    size_t len = strlen(path);
    const F2fsPackFsConfigEntry *slash_match = NULL;

    for (size_t i = 0; i < g_fsc_count; ++i) {
        const char *epath = g_fsc_entries[i].path;
        if (strcmp(epath, path) == 0)
            return &g_fsc_entries[i];   /* exact match wins immediately */

        /* Directory entries in our file always carry a trailing slash
         * (aospRelPath() in f2fs_metadata.cpp). sload's incoming `path`
         * for a directory dentry may or may not include one depending on
         * how the caller built it — try the with-slash form too so a
         * convention mismatch doesn't silently drop every directory's
         * recorded uid/gid/mode/capabilities. */
        if (dir && !slash_match) {
            size_t elen = strlen(epath);
            if (elen == len + 1 && epath[elen - 1] == '/' &&
                strncmp(epath, path, len) == 0)
                slash_match = &g_fsc_entries[i];
        }
    }
    return slash_match;
}

void f2fs_pack_fs_config(const char *path, int dir, const char *target_out_path,
                         unsigned *uid, unsigned *gid, unsigned *mode,
                         uint64_t *capabilities)
{
    (void)target_out_path;

    /* Match AOSP's own canned_fs_config() convention: incoming paths carry
     * a leading '/', but our stored entries don't (except literal root). */
    if (path != NULL && path[0] == '/')
        ++path;

    /* Our writer stores root as the literal string "/" (aospRelPath()'s
     * special case) — after stripping the leading slash above, root's
     * incoming path becomes "", so look up "/" explicitly for that case
     * rather than searching for an empty string that will never match. */
    const char *lookup = (path == NULL || path[0] == '\0') ? "/" : path;

    const F2fsPackFsConfigEntry *e = fsc_find(lookup, dir);
    if (e) {
        *uid          = e->uid;
        *gid          = e->gid;
        *mode         = e->mode;
        *capabilities = e->capabilities;
        return;
    }

    /* No recorded entry for this path — fall back to plain, sane defaults
     * (matches what sload's caller would already have from lstat() before
     * calling this function, so this is a safety net, not a real answer;
     * having comprehensive fs_config.txt coverage from f2fs_extract is
     * the actual correctness guarantee here). */
    *uid          = 0;
    *gid          = 0;
    *mode         = dir ? 0755 : 0644;
    *capabilities = 0;
}

/* ────────────────────────────────────────────────────────────────────────
 * f2fs_pack_set_capabilities
 *
 * AOSP's own sload.c reads de->capabilities (via fs_config_func above) but
 * NEVER actually writes it as a security.capability xattr anywhere in this
 * codebase — confirmed by grepping every use of de->capabilities across
 * dir.c/segment.c/sload.c: both non-fsconfig hits are debug-print
 * statements only. Real capabilities enforcement happens at execve() time
 * via this xattr (unlike e.g. the binary fs_config_files blob some OEM
 * images also embed, which is purely informational for userspace tools —
 * not kernel-enforced), so without this our repacked binaries would
 * silently lose their elevated privileges (CAP_NET_ADMIN, etc.) even
 * though fs_config.txt correctly recorded them.
 *
 * Layout matches struct vfs_cap_data (linux/capability.h, 20 bytes) —
 * identical to what f2fs_extract's own buildMetadata() parses on the
 * extraction side: magic_etc (VFS_CAP_REVISION_2) + two
 * {permitted,inheritable} u32 pairs holding the low/high 32 bits of the
 * 64-bit capability mask.
 * ──────────────────────────────────────────────────────────────────────── */
#define F2FS_PACK_VFS_CAP_REVISION_2 0x02000000u
#define F2FS_PACK_XATTR_INDEX_SECURITY 6

/* f2fs_setxattr() is implemented in xattr.c but has no prototype in any
 * header in this codebase — declared here to silence the implicit-
 * declaration warning (the symbol itself resolves fine at link time). */
int f2fs_setxattr(struct f2fs_sb_info *sbi, nid_t ino, int index,
                  const char *name, const void *value, size_t size, int flags);

int f2fs_pack_set_capabilities(struct f2fs_sb_info *sbi, unsigned int ino,
                               uint64_t capabilities)
{
    if (capabilities == 0)
        return 0;   /* nothing to write — matches AOSP's own "0 = no caps" convention */

    uint32_t cap_blob[5];
    cap_blob[0] = F2FS_PACK_VFS_CAP_REVISION_2;       /* magic_etc */
    cap_blob[1] = (uint32_t)(capabilities & 0xFFFFFFFFu); /* data[0].permitted */
    cap_blob[2] = 0;                                   /* data[0].inheritable */
    cap_blob[3] = (uint32_t)(capabilities >> 32);       /* data[1].permitted */
    cap_blob[4] = 0;                                   /* data[1].inheritable */

    return f2fs_setxattr(sbi, ino, F2FS_PACK_XATTR_INDEX_SECURITY,
                         "capability", cap_blob, sizeof(cap_blob), 1);
}

/* ────────────────────────────────────────────────────────────────────────
 * f2fs_pack_load_symlinks / f2fs_pack_find_missing_symlink
 *
 * f2fs_extract's extractor silently skips creating a symlink when the
 * target filesystem can't hold one (Android FUSE / exFAT return ENOSYS —
 * see extractSymlink()'s symlinks_skipped_ handling on the extraction
 * side) and instead records the target in f2fs_special.txt as
 * "symlink:target=<target>". A source tree that was extracted this way
 * has NO entry at all for that path — scandir() will never find it,
 * because there's genuinely nothing on disk there to find. This means
 * the fix can't be "make readlink() fall back to metadata" (that only
 * helps if a broken/wrong symlink exists to call readlink() ON in the
 * first place) — build_directory() needs to know to synthesize a dentry
 * for these paths that were never physically created.
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    char *path;    /* full recorded path, with leading slash, e.g. "/bin/cat" */
    char *target;  /* symlink target string */
} F2fsPackSymlinkEntry;

static F2fsPackSymlinkEntry *g_sym_entries  = NULL;
static size_t                g_sym_count    = 0;
static size_t                g_sym_capacity = 0;

int f2fs_pack_load_symlinks(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f)
        return -1;

    g_sym_capacity = 64;
    g_sym_entries  = (F2fsPackSymlinkEntry *)
        malloc(g_sym_capacity * sizeof(F2fsPackSymlinkEntry));
    g_sym_count = 0;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        /* First column (up to the first tab) is the path. */
        char *tab = strchr(p, '\t');
        if (!tab)
            continue;   /* no attributes at all on this line */
        size_t path_len = (size_t)(tab - p);
        if (path_len == 0 || path_len >= sizeof(line))
            continue;

        char path[2048];
        if (path_len >= sizeof(path))
            continue;
        memcpy(path, p, path_len);
        path[path_len] = '\0';

        const char *marker = "symlink:target=";
        char *symtag = strstr(tab, marker);
        if (!symtag)
            continue;   /* this entry has other attrs, but not a symlink target */
        symtag += strlen(marker);

        /* Target runs until the next tab, CR, or LF. */
        char target[2048];
        size_t tlen = strcspn(symtag, "\t\r\n");
        if (tlen >= sizeof(target))
            tlen = sizeof(target) - 1;
        memcpy(target, symtag, tlen);
        target[tlen] = '\0';

        if (g_sym_count >= g_sym_capacity) {
            g_sym_capacity *= 2;
            g_sym_entries = (F2fsPackSymlinkEntry *)
                realloc(g_sym_entries, g_sym_capacity * sizeof(F2fsPackSymlinkEntry));
        }
        g_sym_entries[g_sym_count].path   = strdup(path);
        g_sym_entries[g_sym_count].target = strdup(target);
        ++g_sym_count;
    }

    fclose(f);
    fprintf(stderr, "[f2fs_pack] loaded %zu recorded symlink target(s)\n", g_sym_count);
    return 0;
}

/* Returns the number of missing symlinks found under `dir_path` (an
 * f2fs-relative directory path with leading AND trailing slash, e.g. "/bin/"
 * or "/" for root) that are not present in `existing_names` (a NULL-
 * terminated array of plain filenames already found by scandir() in that
 * directory). On return, *out_names / *out_targets are malloc'd
 * NULL-terminated arrays (caller frees each string and the arrays
 * themselves) of the missing entries' bare filenames and targets. */
int f2fs_pack_find_missing_symlinks(const char *dir_path,
                                    char **existing_names, int existing_count,
                                    char ***out_names, char ***out_targets)
{
    size_t dir_len = strlen(dir_path);
    char **names   = NULL;
    char **targets = NULL;
    int    count   = 0;
    int    cap     = 0;

    for (size_t i = 0; i < g_sym_count; ++i) {
        const char *path = g_sym_entries[i].path;
        size_t plen = strlen(path);

        /* Must live directly under dir_path (one path component below —
         * no further '/' after the dir_path prefix), and must actually
         * start with dir_path. */
        if (plen <= dir_len || strncmp(path, dir_path, dir_len) != 0)
            continue;
        const char *rest = path + dir_len;
        if (strchr(rest, '/') != NULL)
            continue;   /* lives in a deeper subdirectory, not this one */

        /* Already present on disk (real symlink survived extraction) —
         * nothing to synthesize. */
        int found = 0;
        for (int j = 0; j < existing_count; ++j) {
            if (strcmp(existing_names[j], rest) == 0) { found = 1; break; }
        }
        if (found)
            continue;

        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            names   = (char **)realloc(names,   (size_t)cap * sizeof(char *));
            targets = (char **)realloc(targets, (size_t)cap * sizeof(char *));
        }
        names[count]   = strdup(rest);
        targets[count] = strdup(g_sym_entries[i].target);
        ++count;
    }

    if (count > 0) {
        names   = (char **)realloc(names,   (size_t)(count + 1) * sizeof(char *));
        targets = (char **)realloc(targets, (size_t)(count + 1) * sizeof(char *));
        names[count]   = NULL;
        targets[count] = NULL;
    }
    *out_names   = names;
    *out_targets = targets;
    return count;
}
