/**
 * main.cpp — f2fs_extract CLI
 *
 * Usage:
 *   f2fs_extract [options] <image> <output_dir>
 *
 * Options:
 *   -v          verbose (INFO + WARN + ERR)
 *   -q          quiet   (ERR only)
 *   -m          save metadata alongside extracted files
 *   -M <dir>    save metadata into <dir>  (default: <output_dir>/_metadata)
 */

#include "f2fs_extract.h"

#include <cstdio>
#include <cstring>
#include <string>

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [-v|-q] [-m|-M <meta_dir>] <f2fs_image> <output_dir>\n"
        "\n"
        "  -v            verbose output  (INFO + WARN + ERR)\n"
        "  -q            quiet output    (ERR only)\n"
        "  -m            save metadata to <output_dir>/_metadata/\n"
        "  -M <dir>      save metadata to <dir>\n"
        "\n"
        "Metadata files written:\n"
        "  metadata.json      mode/uid/gid/xattrs (atime/mtime are applied\n"
        "                     directly to extracted files, not recorded here)\n"
        "  fs_config.txt      AOSP fs_config format\n"
        "  file_contexts.txt  SELinux labels\n"
        "\n"
        "Example:\n"
        "  %s -v -m cache.img out/cache\n",
        prog, prog);
}

int main(int argc, char* argv[])
{
    bool        verbose   = false;
    bool        quiet     = false;
    bool        do_meta   = false;
    std::string meta_dir;

    int optind = 1;
    while (optind < argc && argv[optind][0] == '-') {
        std::string opt = argv[optind];
        if (opt == "-v") {
            verbose = true;
        } else if (opt == "-q") {
            quiet = true;
        } else if (opt == "-m") {
            do_meta = true;
        } else if (opt == "-M") {
            do_meta = true;
            if (optind + 1 >= argc) {
                fprintf(stderr, "-M requires an argument\n");
                usage(argv[0]);
                return 1;
            }
            meta_dir = argv[++optind];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[optind]);
            usage(argv[0]);
            return 1;
        }
        ++optind;
    }

    if (argc - optind < 2) { usage(argv[0]); return 1; }

    const char* image_path = argv[optind];
    const char* out_dir    = argv[optind + 1];

    // Default metadata directory
    if (do_meta && meta_dir.empty())
        meta_dir = std::string(out_dir) + "/_metadata";

    // ── Logger ────────────────────────────────────────────────────────────
    auto logger = [&](LogLevel lvl, const char* msg) {
        if (quiet   && lvl != LogLevel::ERR)  return;
        if (!verbose && lvl == LogLevel::INFO) return;
        FILE* out  = (lvl == LogLevel::ERR) ? stderr : stdout;
        const char* pfx =
            (lvl == LogLevel::ERR)  ? "\033[31m[ERR]\033[0m " :
            (lvl == LogLevel::WARN) ? "\033[33m[WRN]\033[0m " : "[INF] ";
        fputs(pfx, out);
        fputs(msg, out);
        fputc('\n', out);
    };

    // ── Open & extract ────────────────────────────────────────────────────
    F2FSExtractor extractor(logger);

    fprintf(stdout, "Opening %s ...\n", image_path);
    if (!extractor.open(image_path)) {
        fprintf(stderr, "Failed to open/parse F2FS image.\n");
        return 2;
    }

    if (verbose) extractor.printInfo();
    if (do_meta) extractor.enableMetadata();

    fprintf(stdout, "Extracting to %s ...\n", out_dir);
    if (!extractor.extractAll(out_dir)) {
        fprintf(stderr, "Extraction failed.\n");
        return 3;
    }

    if (do_meta) {
        fprintf(stdout, "Saving metadata to %s/ ...\n", meta_dir.c_str());
        if (!extractor.saveMetadata(meta_dir)) {
            fprintf(stderr, "Metadata save failed.\n");
            return 4;
        }
    }

    fprintf(stdout, "Done.\n");
    return 0;
}
