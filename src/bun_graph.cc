// bun_graph.cc - Parse a Bun --compile ELF and print its Standalone Module Graph.
//   part of patchnar - built via autotools (shares ElfFile from patchelf)
//   run:    ./bun_graph <bun-compiled-elf> [--extract DIR]

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <memory>
#include <cerrno>
#include <string_view>
#include <cctype>

#include "elf.h"
#include "patchelf.h"

// Shared source-highlight tokenization for syntax-aware string patching
#include "source_patcher.h"

// ElfFile type aliases for cleaner template dispatch
using ElfFile32 = ElfFile<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Nhdr, Elf32_Addr, Elf32_Off,
    Elf32_Dyn, Elf32_Sym, Elf32_Versym, Elf32_Verdef, Elf32_Verdaux,
    Elf32_Verneed, Elf32_Vernaux, Elf32_Rel, Elf32_Rela, 32>;
using ElfFile64 = ElfFile<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Nhdr, Elf64_Addr, Elf64_Off,
    Elf64_Dyn, Elf64_Sym, Elf64_Versym, Elf64_Verdef, Elf64_Verdaux,
    Elf64_Verneed, Elf64_Vernaux, Elf64_Rel, Elf64_Rela, 64>;

static const char BUN_MAGIC[] = "\n---- Bun! ----\n";
static constexpr size_t BUN_MAGIC_LEN = 16;
static constexpr size_t BLOB_HEADER_SIZE = 8;  // [u64 payload_len] before payload data


struct BunSection {
    size_t file_offset;
    size_t size;
};

// Read file into a FileContents (shared_ptr<vector<unsigned char>>)
static FileContents readFileContents(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return {}; }
    struct stat st{};
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return {}; }
    auto contents = std::make_shared<std::vector<unsigned char>>(st.st_size);
    size_t bytesRead = 0;
    ssize_t n;
    while ((n = read(fd, contents->data() + bytesRead, st.st_size - bytesRead)) > 0)
        bytesRead += n;
    close(fd);
    if (bytesRead != static_cast<size_t>(st.st_size)) {
        fprintf(stderr, "short read on %s\n", path);
        return {};
    }
    return contents;
}

// Check ELF class from raw file contents
static bool isElf32(const FileContents& fc) {
    return fc->size() > EI_CLASS && (*fc)[EI_CLASS] == ELFCLASS32;
}

// Find the .bun section using patchelf's ElfFile (handles endianness + bounds checks)
template<class ElfFileType>
static bool findBunSection(FileContents fc, BunSection& out) {
    try {
        ElfFileType elfFile(fc);
        auto info = elfFile.findSection(".bun");
        if (!info) return false;
        out.file_offset = info->offset;
        out.size = info->size;
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "ELF error: %s\n", e.what());
        return false;
    }
}

// ---- Embedded statically-linked CLI tools (busybox-style argv[0] dispatch) ----
//
// The Anthropic Bun fork links ripgrep / ugrep / bfs and sandbox-runtime's
// apply-seccomp directly into the single `claude` executable and selects one
// by basename(argv[0]) at startup (a strlen(argv0)-keyed classifier accepting
// exactly "rg", "ug", "ugrep", "bfs", "apply-seccomp"; any other argv0 runs
// Claude Code).  Their machine code is merged into .text with no per-program
// boundary, so — unlike the .bun module graph above — they cannot be carved
// out as files.  What we CAN do is confirm which tools are linked in by
// scanning the runtime's .rodata for each tool's unique signature string.
//
// We scan .rodata ONLY, never .bun: the embedded cli.js (in .bun) mentions
// these same tool names, so a whole-file scan would false-match (verified:
// "apply-seccomp" occurs in both sections; the search-tool names occur in
// cli.js too).  .rodata belongs to the statically-linked tools themselves.
struct EmbeddedTool {
    const char* name;
    const char* argv0;                 // display: comma-joined dispatch aliases
    std::vector<const char*> anchors;  // ALL must be present in .rodata
};
static const std::vector<EmbeddedTool> EMBEDDED_TOOLS = {
    {"ripgrep",       "rg",            {"BurntSushi/ripgrep"}},
    {"ugrep",         "ug, ugrep",     {"ugrep user manual"}},
    {"bfs",           "bfs",           {"bfs contributors"}},
    {"apply-seccomp", "apply-seccomp", {"apply-seccomp"}},
};

// Length of a leading dotted-numeric version (>=2 groups, e.g. "7.5.0"), else 0.
static size_t semverLen(std::string_view s) {
    size_t i = 0; int groups = 0;
    while (i < s.size()) {
        size_t d = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) break;
        groups++;
        if (i < s.size() && s[i] == '.') { i++; continue; }
        break;
    }
    return groups >= 2 ? i : 0;
}

// Best-effort version for a detected tool (empty when not stored as a literal;
// ripgrep and bfs construct their banners at runtime, so only ugrep/ripgrep
// expose something recoverable from a static scan).
static std::string toolVersion(std::string_view name, std::string_view ro) {
    if (name == "ugrep") {
        for (size_t p = ro.find("ugrep "); p != std::string_view::npos; p = ro.find("ugrep ", p + 1)) {
            size_t L = semverLen(ro.substr(p + 6));
            if (L) return std::string(ro.substr(p + 6, L));
        }
    } else if (name == "ripgrep") {
        // ripgrep stores "<semver><git-rev>" adjacently (clap version! + rev).
        for (size_t p = 0; p + 8 < ro.size(); ++p) {
            if (ro[p] < '0' || ro[p] > '9') continue;
            size_t L = semverLen(ro.substr(p));
            if (L < 5) continue;  // want "14.1.1"-style, not a bare "1.0"
            if (p > 0 && (std::isalnum((unsigned char)ro[p - 1]) || ro[p - 1] == '.')) continue;
            size_t h = p + L, hn = 0;
            while (h + hn < ro.size() && hn < 12 && std::isxdigit((unsigned char)ro[h + hn])) hn++;
            if (hn >= 7) {
                std::string v(ro.substr(p, L));
                v += " (rev "; v.append(ro.substr(h, 10)); v += ")";
                return v;
            }
        }
    }
    return {};
}

template<class ElfFileType>
static bool findRodataRange(FileContents fc, size_t& off, size_t& size) {
    try {
        ElfFileType elfFile(fc);
        auto info = elfFile.findSection(".rodata");
        if (!info) return false;
        off = info->offset; size = info->size;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Print which embedded tools are statically linked into the executable.
static void scanEmbeddedTools(FileContents fc) {
    printf("\n--- embedded native tools (statically linked, argv[0]-dispatched) ---\n");
    size_t ro_off = 0, ro_sz = 0;
    bool ok = isElf32(fc) ? findRodataRange<ElfFile32>(fc, ro_off, ro_sz)
                          : findRodataRange<ElfFile64>(fc, ro_off, ro_sz);
    if (!ok || ro_off + ro_sz > fc->size()) {
        printf("  (.rodata not found — cannot scan)\n");
        return;
    }
    std::string_view ro(reinterpret_cast<const char*>(fc->data()) + ro_off, ro_sz);
    size_t found = 0;
    for (const auto& t : EMBEDDED_TOOLS) {
        bool present = true;
        for (const char* a : t.anchors)
            if (ro.find(a) == std::string_view::npos) { present = false; break; }
        if (present) {
            std::string ver = toolVersion(t.name, ro);
            printf("  %-14s argv0: %-16s [linked]%s%s\n",
                   t.name, t.argv0, ver.empty() ? "" : "  version: ", ver.c_str());
            ++found;
        } else {
            printf("  %-14s argv0: %-16s [NOT FOUND — upstream may have changed]\n",
                   t.name, t.argv0);
        }
    }
    printf("total: %zu/%zu known embedded tools linked in\n", found, EMBEDDED_TOOLS.size());
    printf("  (signature scan of .rodata; argv[0] keys are this Bun fork's applet classifier)\n");
}

// A resolved entry discovered in the directory region.
struct Entry {
    size_t dir_offset;        // byte offset inside the directory region
    uint32_t stored_name_off; // as stored in the record
    uint32_t name_len;
    uint32_t stored_content_off;
    uint32_t content_len;
    uint32_t stored_bytecode_off;  // JSC bytecode offset (bytes 24-27)
    uint32_t bytecode_len;         // JSC bytecode length (bytes 28-31)
    uint32_t stored_sourcemap_off;
    uint32_t sourcemap_len;
    std::string name;
    std::string module_info;
    std::string bytecode_origin_path;
    bool has_content;
    bool has_bytecode;
    uint8_t encoding;
    uint8_t loader;
    uint8_t module_format;
    uint8_t side;
    const char* kind;         // short label describing detected content type
};

static bool is_bunfs_path(const uint8_t* p, size_t len, size_t max) {
    if (len < 8 || len > 512) return false;
    if (len > max) return false;
    return memcmp(p, "/$bunfs/", 8) == 0;
}

// Create parent directories for a path (mkdir -p, without the final component).
static bool mkparents(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && i > 0) {
            if (mkdir(cur.c_str(), 0755) < 0 && errno != EEXIST) {
                perror(("mkdir " + cur).c_str());
                return false;
            }
        }
        cur.push_back(path[i]);
    }
    return true;
}

// Reject absolute paths and any path containing a ".." component, so extraction
// cannot escape the chosen output directory.
static bool is_safe_relative(const std::string& p) {
    if (p.empty() || p.front() == '/') return false;
    size_t start = 0;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            size_t len = i - start;
            if (len == 2 && p[start] == '.' && p[start + 1] == '.') return false;
            start = i + 1;
        }
    }
    return true;
}

// Map an internal /$bunfs/... path to an output path.
// Replaces the "/$bunfs" prefix with the user-specified output directory.
static std::string output_path(const std::string& bunfs, const std::string& outdir) {
    const char prefix[] = "/$bunfs";
    if (bunfs.size() >= sizeof(prefix) - 1 &&
        memcmp(bunfs.data(), prefix, sizeof(prefix) - 1) == 0) {
        return outdir + bunfs.substr(sizeof(prefix) - 1);
    }
    return bunfs;
}

static bool write_file(const std::string& path, const uint8_t* data, size_t len) {
    if (!mkparents(path)) return false;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(("open " + path).c_str()); return false; }
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) { perror("write"); close(fd); return false; }
        if (n == 0) {
            fprintf(stderr, "write: no progress writing %s\n", path.c_str());
            close(fd);
            return false;
        }
        written += static_cast<size_t>(n);
    }
    close(fd);
    return true;
}

static bool write_file(const std::string& path, const std::string& content) {
    return write_file(path, reinterpret_cast<const uint8_t*>(content.data()), content.size());
}

// Translator that replaces /$bunfs with the output directory inside string literals.
// Uses CharTranslator's regex-based set_translation (same pattern as NixPathTranslator).
class BunfsTranslator : public srchilite::CharTranslator {
public:
    BunfsTranslator(const std::string& outdir) : srchilite::CharTranslator() {
        set_translation("/\\$bunfs", outdir);
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <bun-compiled-elf> [--extract DIR]\n", argv[0]);
        return 1;
    }

    bool extract = false;
    std::string outdir = "/$bunfs";
    for (int ai = 2; ai < argc; ++ai) {
        if (strcmp(argv[ai], "--extract") == 0) {
            if (ai + 1 >= argc) {
                fprintf(stderr, "--extract requires a directory argument\n");
                return 1;
            }
            extract = true;
            outdir = argv[++ai];
            // Strip trailing slash for clean path joining
            while (outdir.size() > 1 && outdir.back() == '/')
                outdir.pop_back();
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[ai]);
            return 1;
        }
    }

    // Read file into memory using patchelf's FileContents type
    auto fileContents = readFileContents(argv[1]);
    if (!fileContents) return 1;

    // Find .bun section using patchelf's ElfFile (handles ELF32/64 + endianness)
    BunSection bun;
    bool found = isElf32(fileContents)
        ? findBunSection<ElfFile32>(fileContents, bun)
        : findBunSection<ElfFile64>(fileContents, bun);
    if (!found) {
        fprintf(stderr, ".bun section not found\n"); return 1;
    }
    printf(".bun section: file_offset=0x%zx size=0x%zx (%zu bytes)\n",
           bun.file_offset, bun.size, bun.size);

    const uint8_t* data = fileContents->data();
    // The .bun ELF section starts with [u64 payload_len][payload bytes].
    // All StringPointer offsets in Bun's format are relative to payload start.
    // Adjust sec to point to payload and bun.size to payload length.
    const uint8_t* sec = data + bun.file_offset + BLOB_HEADER_SIZE;
    bun.size -= BLOB_HEADER_SIZE;
    if (bun.size < BUN_MAGIC_LEN ||
        memcmp(sec + bun.size - BUN_MAGIC_LEN, BUN_MAGIC, BUN_MAGIC_LEN) != 0) {
        fprintf(stderr, "Bun trailer magic not found\n"); return 1;
    }
    printf("trailer magic: OK ('\\n---- Bun! ----\\n')\n");

    // Footer: Bun's Offsets struct (immediately before magic)
    //   Layout (x86_64):
    //     u64 byte_count          (total payload size)
    //     u32 modules_ptr.offset  (offset of modules array in payload)
    //     u32 modules_ptr.length  (size of modules array in bytes)
    //     u32 entry_point_id
    //     u32 exec_argv.offset
    //     u32 exec_argv.length
    //     u32 flags
    //   Total: 32 bytes
    static constexpr size_t FOOTER_LEN = 32;  // Offsets struct size
    if (bun.size < BUN_MAGIC_LEN + FOOTER_LEN) {
        fprintf(stderr, ".bun section too small for footer (%zu bytes)\n", bun.size);
        return 1;
    }
    size_t fe = bun.size - BUN_MAGIC_LEN;

    // Read Offsets struct fields (from end, backwards):
    //   fe-32: byte_count (u64)
    //   fe-24: modules_ptr.offset (u32) = dir_off
    //   fe-20: modules_ptr.length (u32) = dir_size
    //   fe-16: entry_point_id (u32)
    //   fe-12: exec_argv.offset (u32)
    //   fe-8:  exec_argv.length (u32)
    //   fe-4:  flags (u32)
    uint64_t byte_count;     memcpy(&byte_count,     sec + fe - 32, 8);
    uint32_t dir_off;        memcpy(&dir_off,        sec + fe - 24, 4);
    uint32_t dir_size;       memcpy(&dir_size,       sec + fe - 20, 4);
    uint32_t entry_point_id; memcpy(&entry_point_id, sec + fe - 16, 4);
    uint32_t argv_off;       memcpy(&argv_off,       sec + fe - 12, 4);
    uint32_t argv_len;       memcpy(&argv_len,       sec + fe -  8, 4);
    uint32_t flags;          memcpy(&flags,          sec + fe -  4, 4);

    printf("\n--- footer (Offsets struct) ---\n");
    printf("  byte_count      : 0x%" PRIx64 " (%zu)\n", byte_count, (size_t)byte_count);
    printf("  modules_ptr     : {off=0x%08x, len=0x%08x} (%u bytes)\n", dir_off, dir_size, dir_size);
    printf("  entry_point_id  : %u\n", entry_point_id);
    printf("  exec_argv       : {off=0x%08x, len=0x%08x}\n", argv_off, argv_len);
    printf("  flags           : 0x%08x\n", flags);

    // dir_off is payload-relative (offset within the payload data)
    if ((size_t)dir_off + dir_size > bun.size) {
        fprintf(stderr, "directory out of .bun section\n"); return 1;
    }
    const uint8_t* dir = sec + dir_off;

    // Parse the directory as an array of CompiledModuleGraphFile structs.
    // Each struct is 52 bytes (from Bun's StandaloneModuleGraph.zig):
    //   name:                StringPointer {u32 off, u32 len}   bytes  0-7
    //   contents:            StringPointer {u32 off, u32 len}   bytes  8-15
    //   sourcemap:           StringPointer {u32 off, u32 len}   bytes 16-23
    //   bytecode:            StringPointer {u32 off, u32 len}   bytes 24-31
    //   module_info:         StringPointer {u32 off, u32 len}   bytes 32-39
    //   bytecode_origin_path:StringPointer {u32 off, u32 len}   bytes 40-47
    //   encoding:            u8                                  byte  48
    //   loader:              u8                                  byte  49
    //   module_format:       u8                                  byte  50
    //   side:                u8                                  byte  51
    static constexpr size_t MODULE_RECORD_SIZE = 52;
    printf("\n--- directory records ---\n");
    std::vector<Entry> entries;
    size_t module_count = dir_size / MODULE_RECORD_SIZE;
    printf("module_count: %zu (%zu bytes / %zu per record, %zu remainder)\n",
           module_count, (size_t)dir_size, MODULE_RECORD_SIZE,
           (size_t)dir_size % MODULE_RECORD_SIZE);

    for (size_t m = 0; m < module_count; ++m) {
        const uint8_t* rec = dir + m * MODULE_RECORD_SIZE;

        uint32_t name_off, name_len, cont_off, cont_len;
        uint32_t sm_off, sm_len, bc_off, bc_len;
        uint32_t mi_off, mi_len, bcop_off, bcop_len;
        memcpy(&name_off, rec + 0, 4);
        memcpy(&name_len, rec + 4, 4);
        memcpy(&cont_off, rec + 8, 4);
        memcpy(&cont_len, rec + 12, 4);
        memcpy(&sm_off,   rec + 16, 4);
        memcpy(&sm_len,   rec + 20, 4);
        memcpy(&bc_off,   rec + 24, 4);
        memcpy(&bc_len,   rec + 28, 4);
        memcpy(&mi_off,   rec + 32, 4);
        memcpy(&mi_len,   rec + 36, 4);
        memcpy(&bcop_off, rec + 40, 4);
        memcpy(&bcop_len, rec + 44, 4);

        // Resolve name - StringPointer.offset is directly into the payload.
        // A StringPointer signals "absent" with length == 0, NOT offset == 0:
        // offset 0 is the first byte of the payload, and Bun serialises the
        // entry point's name there.  Rejecting offset 0 therefore dropped the
        // application's own module from every report and from --extract.
        size_t nreal = (size_t)name_off;
        if (name_len == 0 || nreal + name_len > bun.size) continue;
        if (!is_bunfs_path(sec + nreal, name_len, bun.size)) continue;

        Entry e{};
        e.dir_offset = m * MODULE_RECORD_SIZE;
        e.stored_name_off = name_off;
        e.name_len = name_len;
        e.name.assign(reinterpret_cast<const char*>(sec + nreal), name_len);

        // Resolve contents - StringPointer.offset is directly into the blob
        size_t creal = (size_t)cont_off;
        if (cont_len >= 4 && creal + cont_len <= bun.size) {
            const uint8_t* cp = sec + creal;
            // Bun preserves the entry point's shebang, so the "// @bun" banner
            // is not always at byte 0.  Skip one leading "#!" line before
            // testing, or the application module is misfiled as raw binary and
            // extracted without its /$bunfs string rewriting.
            size_t banner = 0;
            if (cont_len > 2 && cp[0] == '#' && cp[1] == '!') {
                while (banner < cont_len && cp[banner] != '\n') ++banner;
                if (banner < cont_len) ++banner;
            }
            if (cont_len - banner >= 7 && memcmp(cp + banner, "// @bun", 7) == 0) {
                e.kind = "js";
                e.has_content = true;
            } else if (memcmp(cp, "\x7f""ELF", 4) == 0) {
                e.kind = "elf";
                e.has_content = true;
            } else if (memcmp(cp, "\0asm", 4) == 0) {
                e.kind = "wasm";
                e.has_content = true;
            } else {
                // Unknown content type — extract as raw binary
                e.kind = "binary";
                e.has_content = true;
            }
            if (e.has_content) {
                e.stored_content_off = cont_off;
                e.content_len = cont_len;
            }
        }

        // Resolve bytecode - StringPointer at bytes 24-31 (JSC pre-compiled bytecode)
        size_t bcreal = (size_t)bc_off;
        if (bc_len > 0 && bcreal + bc_len <= bun.size) {
            e.has_bytecode = true;
            e.stored_bytecode_off = bc_off;
            e.bytecode_len = bc_len;
        }

        // Resolve remaining StringPointers
        if (sm_len > 0 && (size_t)sm_off + sm_len <= bun.size) {
            e.stored_sourcemap_off = sm_off;
            e.sourcemap_len = sm_len;
        }
        if (mi_len > 0 && (size_t)mi_off + mi_len <= bun.size)
            e.module_info.assign(reinterpret_cast<const char*>(sec + mi_off), mi_len);
        if (bcop_len > 0 && (size_t)bcop_off + bcop_len <= bun.size)
            e.bytecode_origin_path.assign(reinterpret_cast<const char*>(sec + bcop_off), bcop_len);

        // Metadata byte fields
        e.encoding = rec[48];
        e.loader = rec[49];
        e.module_format = rec[50];
        e.side = rec[51];

        entries.push_back(std::move(e));
    }

    // Every 52-byte record in the directory should yield one entry.  A record
    // that fails the checks above is dropped silently, so the module vanishes
    // from both the report and --extract while the run still exits 0 — and
    // callers gate their builds on this output.  Make any shortfall loud.
    if (entries.size() != module_count)
        fprintf(stderr, "warning: %zu of %zu directory records did not parse as modules\n",
                module_count - entries.size(), module_count);

    printf("\n%-4s %-50s %-12s %-10s %-12s %-10s %-12s %-10s %s\n",
           "idx", "path", "name_off", "name_len", "content_off", "content_len",
           "bc_off", "bc_len", "kind");
    for (size_t k = 0; k < entries.size(); ++k) {
        const auto& e = entries[k];
        std::string path = output_path(e.name, outdir);
        printf("%-4zu %-50s 0x%010x %-10u 0x%010x %-10u",
               k, path.c_str(),
               e.stored_name_off, e.name_len,
               e.stored_content_off, e.content_len);
        if (e.has_bytecode)
            printf(" 0x%010x %-10u", e.stored_bytecode_off, e.bytecode_len);
        else
            printf(" %-12s %-10s", "-", "-");
        printf(" %s%s\n", e.kind, e.has_bytecode ? "+jsc" : "");
    }

    size_t bc_count = 0;
    for (const auto& e : entries) if (e.has_bytecode) ++bc_count;
    printf("\ntotal entries: %zu (%zu with JSC bytecode)\n", entries.size(), bc_count);

    // Per-entry detail block with all StringPointer fields
    static const char* loader_names[] = {
        "jsx", "js", "ts", "tsx", "css", "file", "json", "toml", "wasm", "napi", "base64", "dataurl", "text", "sqlite", "sqlite_embedded"
    };
    static const char* module_format_names[] = { "esm", "cjs", "none" };
    static const char* side_names[] = { "client", "server", "none" };
    printf("\n--- entry details ---\n");
    for (size_t k = 0; k < entries.size(); ++k) {
        const auto& e = entries[k];
        std::string path = output_path(e.name, outdir);
        printf("\n[%zu] %s\n", k, path.c_str());
        printf("  encoding:             %u\n", e.encoding);
        printf("  loader:               %u (%s)\n", e.loader,
               e.loader < sizeof(loader_names)/sizeof(loader_names[0]) ? loader_names[e.loader] : "?");
        printf("  module_format:        %u (%s)\n", e.module_format,
               e.module_format < sizeof(module_format_names)/sizeof(module_format_names[0]) ? module_format_names[e.module_format] : "?");
        printf("  side:                 %u (%s)\n", e.side,
               e.side < sizeof(side_names)/sizeof(side_names[0]) ? side_names[e.side] : "?");
        if (e.sourcemap_len > 0)
            printf("  sourcemap:            %u bytes (off=0x%08x)\n", e.sourcemap_len, e.stored_sourcemap_off);
        if (!e.module_info.empty())
            printf("  module_info:          %.*s\n", (int)std::min(e.module_info.size(), (size_t)200), e.module_info.c_str());
        if (!e.bytecode_origin_path.empty())
            printf("  bytecode_origin_path: %s\n", e.bytecode_origin_path.c_str());
    }

    // Report statically-linked CLI tools multiplexed into this executable.
    scanEmbeddedTools(fileContents);

    if (extract) {
        printf("\n--- extracting ---\n");
        size_t ok = 0, skipped = 0, unsafe = 0, jsc_ok = 0;
        for (const auto& e : entries) {
            if (!e.has_content && !e.has_bytecode) {
                printf("  %s (no content, skipped)\n", e.name.c_str());
                ++skipped;
                continue;
            }
            std::string full = output_path(e.name, outdir);
            if (!is_safe_relative(full) && full.front() != '/') {
                fprintf(stderr, "  refusing unsafe path: %s\n", full.c_str());
                ++unsafe;
                continue;
            }
            // Extract JS/binary content
            if (e.has_content) {
                size_t creal = (size_t)e.stored_content_off;
                if (strcmp(e.kind, "js") == 0) {
                    // Patch source: replace /$bunfs with outdir inside string literals
                    std::string src(reinterpret_cast<const char*>(sec + creal), e.content_len);
                    std::string langFile = detectLanguageFromFile(full, src);
                    if (langFile.empty()) langFile = "javascript.lang"; // fallback for JS
                    BunfsTranslator translator(outdir);
                    std::string patched = patchSourceStrings(src, langFile, translator);
                    if (!write_file(full, patched)) return 1;
                    printf("  %s (%zu bytes, %s, patched)\n",
                           full.c_str(), patched.size(), e.kind);
                } else {
                    if (!write_file(full, sec + creal, e.content_len)) return 1;
                    printf("  %s (%u bytes, %s)\n",
                           full.c_str(), e.content_len, e.kind);
                }
                ++ok;
            }
            // Extract JSC bytecode as sibling .jsc file
            if (e.has_bytecode) {
                std::string jsc_path = full + ".jsc";
                size_t bcreal = (size_t)e.stored_bytecode_off;
                if (!write_file(jsc_path, sec + bcreal, e.bytecode_len)) return 1;
                printf("  %s (%u bytes, jsc bytecode)\n",
                       jsc_path.c_str(), e.bytecode_len);
                ++jsc_ok;
            }
        }
        printf("extracted: %zu source + %zu jsc, skipped: %zu, refused (unsafe path): %zu\n",
               ok, jsc_ok, skipped, unsafe);
    }
    return 0;
}
