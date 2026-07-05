#pragma once
/**
 * f2fs_extract.h — high-level F2FS image extractor
 */

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

#include "../include/f2fs_fs.h"
#include "f2fs_metadata.h"

// ────────────────────────────────────────────────────────────────────────────
// Progress / logging callback
// ────────────────────────────────────────────────────────────────────────────
enum class LogLevel { INFO, WARN, ERR };
using LogFn = std::function<void(LogLevel, const char* /*msg*/)>;

// ────────────────────────────────────────────────────────────────────────────
// F2FSExtractor
// ────────────────────────────────────────────────────────────────────────────
class F2FSExtractor {
public:
    explicit F2FSExtractor(LogFn log = nullptr);
    ~F2FSExtractor();

    // Open the image file, validate superblock, load NAT.
    // Returns false on any fatal error.
    bool open(const std::string& image_path);

    // Extract the entire filesystem rooted at root_ino into outdir.
    bool extractAll(const std::string& outdir);

    // Dump key superblock / NAT statistics to log (INFO).
    void printInfo() const;

    // Enable metadata collection.
    // After extractAll(), call saveMetadata(dir) to write the three files.
    void enableMetadata() { collect_metadata_ = true; }

    // Enrich already-collected metadata from Android runtime config files
    // embedded inside the extracted output tree:
    //   - etc/fs_config_files, etc/fs_config_dirs  (binary, AOSP format)
    //     → capabilities that weren't stored as xattrs (Samsung Android 14)
    //   - etc/selinux/plat_file_contexts etc.       (text, regex patterns)
    //     → SELinux labels for images without security.selinux xattrs
    // Call between extractAll() and saveMetadata().
    void enrichMetadata(const std::string& out_dir);

    bool saveMetadata(const std::string& dir) const {
        return meta_.writeAll(dir);
    }

private:
    // ── I/O ─────────────────────────────────────────────────────────────────
    int  fd_  {-1};
    bool readAt(off_t offset, void* buf, size_t size) const;
    bool readBlock(u32 blkno, void* buf) const;           // reads blksize_ bytes

    // ── Superblock ───────────────────────────────────────────────────────────
    f2fs_super_block sb_ {};
    u32 blksize_           {F2FS_BLKSIZE};
    u64 total_block_count_ {0};            // from superblock — used to detect stale addresses
    mutable u64 stale_blocks_read_ {0};    // blocks beyond image EOF (silently zeroed)
    u32 blocks_per_seg_{512};

    bool readSuperblock();

    // ── Checkpoint ───────────────────────────────────────────────────────────
    // Reads both CP packs, picks the one with higher checkpoint_ver,
    // returns nat_ver_bitmap for NAT set selection.
    bool readCheckpoint(std::vector<u8>& nat_ver_bitmap_out);

    // ── NAT ─────────────────────────────────────────────────────────────────
    // NID → physical block address
    std::unordered_map<u32, u32> nat_;

    bool loadNAT(const std::vector<u8>& nat_ver_bitmap);

    // NID → block address (NULL_ADDR if not found)
    u32  nidToBlock(u32 nid) const;

    // Read a full f2fs_node by NID.  Returns false if NID not in NAT.
    bool readNode(u32 nid, f2fs_node& node_out) const;

    // ── Inode helpers ────────────────────────────────────────────────────────
    // Number of leading i_addr[] slots used by extra attributes.
    static int extraSlots(const f2fs_inode& inode);

    // Number of trailing i_addr[] slots used by inline xattr data.
    static int xattrSlots(const f2fs_inode& inode);

    // Total data-address slots actually available in i_addr[]:
    //   DEF_ADDRS_PER_INODE - extraSlots - xattrSlots
    static int addrsPerInode(const f2fs_inode& inode);

    // Pointer to the first data-address slot (skips extra-attr region).
    static const le32* inodeAddrs(const f2fs_inode& inode);

    // Pointer & byte-size of inline data region (for F2FS_INLINE_DATA).
    // Skips the extra-attr region AND the 1 reserved slot.
    static const u8*  inlineDataPtr(const f2fs_inode& inode);
    static u64        inlineDataBytes(const f2fs_inode& inode);

    // ── Block-address lookup ─────────────────────────────────────────────────
    // Returns the physical block address of logical block index `bidx`
    // within the file described by `inode`.  Traverses dn/in/din chains.
    u32 fileBlockAddr(const f2fs_inode& inode, u64 bidx) const;

    // ── Extraction ───────────────────────────────────────────────────────────
    // Dispatch by file type; `depth` guards against infinite recursion.
    bool extractEntry(u32 ino, u8 file_type, const std::string& outpath, int depth);

    bool extractRegularFile   (const f2fs_inode& inode, const std::string& outpath);
    bool extractCompressedFile(const f2fs_inode& inode, const std::string& outpath);
    bool extractSymlink       (const f2fs_inode& inode, const std::string& outpath);
    bool extractDir           (u32 ino, const std::string& outpath, int depth);

    // Set atime/mtime on the just-extracted file/dir/symlink to match the
    // original inode's timestamps. ctime cannot be set by userspace (the
    // kernel always stamps it with the current time), so it is not applied.
    // Symlinks are stamped via the *_NOFOLLOW path so the link itself
    // (not its target) gets the timestamp.
    void applyTimestamps(const std::string& outpath, const f2fs_inode& inode) const;

    // Process one 4096-byte dentry block.
    bool processDentryBlock(const f2fs_dentry_block& db,
                            const std::string& outdir, int depth);

    // Process inline dentries stored directly in the inode.
    bool processInlineDents(const f2fs_inode& inode,
                            const std::string& outdir, int depth);

    // Low-level dentry visitor (used by both block and inline paths).
    bool processDentryArray(const u8* bitmap, u32 nr,
                            const f2fs_dir_entry* dentries,
                            const u8* filenames, u32 slot_len,
                            const std::string& outdir, int depth);

    // ── Logging ──────────────────────────────────────────────────────────────
    LogFn log_;
    void logf(LogLevel lvl, const char* fmt, ...) const
        __attribute__((format(printf, 3, 4)));

    // ── Metadata ─────────────────────────────────────────────────────────────
    bool            collect_metadata_ = false;
    MetadataWriter  meta_;
    std::string     outdir_base_;   // set in extractAll(), used for relative paths
    uint32_t        symlinks_skipped_ = 0; // FUSE/FAT targets that can't hold symlinks

    // Read all xattrs for an inode (inline + external node)
    XAttrMap readXAttrs(const f2fs_inode& inode) const;

    // Parse an xattr block buffer (delegates to parseXAttrBlock)
    XAttrMap parseXAttrBuf(const uint8_t* data, size_t size) const;

    // Build a FileMetadata record from an inode + its xattrs
    FileMetadata buildMetadata(const std::string& relpath,
                               const f2fs_inode&  inode,
                               const XAttrMap&    xm) const;

    // Record metadata for the current entry (called inside extractEntry)
    void collectMeta(const std::string& relpath,
                     const f2fs_inode& inode);
};
