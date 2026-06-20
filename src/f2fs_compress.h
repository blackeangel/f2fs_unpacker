#pragma once
/**
 * f2fs_compress.h — F2FS transparent compression: cluster decompression
 *
 * F2FS compresses data in units called "clusters" — groups of
 * (1 << i_log_cluster_size) consecutive 4096-byte pages.
 *
 * On-disk layout of a compressed cluster (N = cluster_size blocks):
 *
 *   i_addr[cluster_base + 0] = COMPRESS_ADDR (0xFFFFFFFE)  ← sentinel
 *   i_addr[cluster_base + 1] = phys_blk_0   ← f2fs_compress_header (24 B)
 *                                              + first part of cdata
 *   i_addr[cluster_base + 2] = phys_blk_1   ← continuation of cdata
 *   ...
 *   i_addr[cluster_base + k] = NULL_ADDR     ← saved (sparse) block
 *   ...
 *   i_addr[cluster_base + N-1] = NULL_ADDR
 *
 * This module decompresses the gathered payload bytes back into
 *   cluster_size × 4096 bytes of plain data.
 *
 * LZO-RLE (algo=3) uses the identical on-disk format and the identical
 * decompressor as plain LZO (algo=0) — verified against fs/f2fs/compress.c
 * (f2fs_lzorle_ops.decompress_pages == lzo_decompress_pages). "RLE" only
 * changes how the kernel *compresses* data, not how it's read back.
 */

#include <cstdint>
#include <cstddef>

// ────────────────────────────────────────────────────────────────────────────
// f2fs_decompress_cluster
//
// @algo          : value of inode.i_compress_algorithm
//                  (0=LZO, 1=LZ4, 2=ZSTD, 3=LZO-RLE — decompresses like LZO)
// @src            : compressed payload (after the 24-byte header), length clen
// @clen           : byte length of the compressed payload
// @dst            : output buffer — must be exactly cluster_pages × 4096 bytes
// @dstlen         : cluster_pages × 4096
//
// Returns true on success, false on decompression error.
// ────────────────────────────────────────────────────────────────────────────
bool f2fs_decompress_cluster(uint8_t        algo,
                             const uint8_t* src,
                             size_t         clen,
                             uint8_t*       dst,
                             size_t         dstlen);
