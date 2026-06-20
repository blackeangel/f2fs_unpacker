/**
 * f2fs_compress.cpp — F2FS cluster decompression
 *
 * Supported algorithms (matching kernel enum compress_algorithm_type):
 *   0 = LZO      (lzo1x_decompress_safe)
 *   1 = LZ4      (LZ4_decompress_safe)       ← most common in Android
 *   2 = ZSTD     (ZSTD_decompress)
 *   3 = LZO-RLE  (lzo1x_decompress_safe — IDENTICAL to plain LZO; verified
 *                 against fs/f2fs/compress.c: f2fs_lzorle_ops.decompress_pages
 *                 == lzo_decompress_pages. RLE only affects how the kernel's
 *                 *compressor* runs LZO, not the decompression path.)
 */

#include "f2fs_compress.h"

#include <cstdio>
#include <cstring>

// ────────────────────────────────────────────────────────────────────────────
// Optional library availability — set by CMake via compile definitions:
//   F2FS_HAVE_LZ4   — liblz4 available
//   F2FS_HAVE_ZSTD  — libzstd available
//   F2FS_HAVE_LZO   — liblzo2 available
// ────────────────────────────────────────────────────────────────────────────

#ifdef F2FS_HAVE_LZ4
#  include <lz4.h>
#endif

#ifdef F2FS_HAVE_ZSTD
#  include <zstd.h>
#endif

#ifdef F2FS_HAVE_LZO
#  include <lzo/lzo1x.h>
   // liblzo2 requires a one-time init; we use a static guard.
   static struct LZOInit {
       LZOInit() { lzo_init(); }
   } _lzo_init;
#endif

// ────────────────────────────────────────────────────────────────────────────
// f2fs_decompress_cluster  (public API)
// ────────────────────────────────────────────────────────────────────────────

bool f2fs_decompress_cluster(uint8_t        algo,
                             const uint8_t* src,
                             size_t         clen,
                             uint8_t*       dst,
                             size_t         dstlen)
{
    switch (algo) {

    // ── LZ4 ──────────────────────────────────────────────────────────────────
    case 1: {
#ifdef F2FS_HAVE_LZ4
        int r = LZ4_decompress_safe(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(dst),
            static_cast<int>(clen),
            static_cast<int>(dstlen)
        );
        if (r < 0) {
            fprintf(stderr, "[ERR] LZ4_decompress_safe rc=%d\n", r);
            return false;
        }
        return true;
#else
        fprintf(stderr, "[ERR] LZ4 support not compiled in\n");
        return false;
#endif
    }

    // ── ZSTD ─────────────────────────────────────────────────────────────────
    case 2: {
#ifdef F2FS_HAVE_ZSTD
        size_t r = ZSTD_decompress(dst, dstlen, src, clen);
        if (ZSTD_isError(r)) {
            fprintf(stderr, "[ERR] ZSTD_decompress: %s\n", ZSTD_getErrorName(r));
            return false;
        }
        return true;
#else
        fprintf(stderr, "[ERR] ZSTD support not compiled in\n");
        return false;
#endif
    }

    // ── LZO and LZO-RLE (identical decompression path) ─────────────────────
    case 0:   // LZO
    case 3: { // LZO-RLE — same decompressor, see file header comment
#ifdef F2FS_HAVE_LZO
        lzo_uint out_len = (lzo_uint)dstlen;
        int r = lzo1x_decompress_safe(src, (lzo_uint)clen,
                                       dst, &out_len, nullptr);
        if (r != LZO_E_OK) {
            fprintf(stderr, "[ERR] lzo1x_decompress_safe rc=%d (algo=%u)\n", r, algo);
            return false;
        }
        return true;
#else
        fprintf(stderr, "[ERR] LZO support not compiled in\n");
        return false;
#endif
    }

    default:
        fprintf(stderr, "[ERR] Unknown F2FS compression algorithm %u\n", algo);
        return false;
    }
}
