// bun_graph.cc - Parse a Bun --compile ELF and print its Standalone Module Graph.
//   part of patchnar - built via autotools (shares ElfFile from patchelf)
//   run:    ./bun_graph <bun-compiled-elf> [--extract DIR [--no-bytecode]]
//           ./bun_graph <bun-compiled-elf> --natives     (JSON: private runtime entry points)
//           ./bun_graph <bun-compiled-elf> --clean OUT   (the runtime without its payload)

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
#include <algorithm>
#include <utility>
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

// ---- Private runtime entry points (--natives) ----
//
// TaskSolver's unbundled Claude Code (claude/unbundle.py) runs this same private Bun
// without its module graph and calls a few of its stripped entry points: JSC's module
// bytecode generator and the CachedBytecode release (to emit .jsc sidecars), BunString ->
// WTF::String (for its UTF-8 loader shim), and the one call through which the
// already-bundled source loader clones file contents as Latin-1 (the shim replaces it).
// Every release relinks the fork, so their addresses move; their instructions mostly
// don't.  --natives prints one JSON object with each virtual address, found by masked
// x86-64 signatures that must match exactly once in .text; a miss exits 2 naming it.
struct NativeSignature {
    const char* name;
    const char* pattern;  // hex bytes; "??" matches a displacement that moves per build
};
static const NativeSignature NATIVE_SIGNATURES[] = {
    // ZigSourceProvider.cpp's C-ABI module bytecode generator: an argument-shuffling
    // thunk that calls the generator body placed right after it.
    {"module_bytecode_generator",
     "55 48 89 e5 48 83 ec 20 4c 89 c0 41 89 c8 89 d1 0f 28 45 10 0f 11 44 24 08 "
     "4c 89 0c 24 ba 01 00 00 00 49 89 c1 e8 ?? ?? ?? ?? 48 83 c4 20 5d c3"},
    // JSC::CachedBytecode's destructor.
    {"cached_bytecode_destructor",
     "55 48 89 e5 41 57 41 56 53 50 48 89 fb 8b 47 64 4c 8b 77 58 48 85 c0 75 2d "
     "83 7b 60 00 78 78 4d 85 f6"},
    // mimalloc's mi_free; 49 bytes, since its first 36 also open two sibling functions.
    {"mi_free",
     "55 48 89 e5 48 89 fe 48 8b 05 ?? ?? ?? ?? 48 89 f9 48 c1 e9 1d 48 8b 44 c8 50 89 f1 "
     "c1 e9 0d 81 e1 f8 ff 00 00 48 8b 3c 08 48 85 ff 74 29 48 8b 07"},
    // BunString -> WTF::String, returned through rdi (rsi is the BunString).
    {"bun_string_to_wtf",
     "55 48 89 e5 53 50 48 89 fb 0f b6 06 83 f8 03 75 1d 48 8b 46 08 48 8b 56 10 "
     "48 89 df 48 89 c6 e8 ?? ?? ?? ??"},
};

static std::vector<int> parseSignature(const char* text) {
    std::vector<int> bytes;
    for (const char* p = text; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?' && p[1] == '?') bytes.push_back(-1);
        else bytes.push_back(static_cast<int>(std::stoul(std::string(p, 2), nullptr, 16)));
        p += 2;
    }
    return bytes;
}

static std::vector<size_t> findSignature(std::string_view text, const std::vector<int>& signature) {
    std::string literal;  // the leading run up to the first wildcard narrows the scan
    for (int byte : signature) {
        if (byte < 0) break;
        literal.push_back(static_cast<char>(byte));
    }
    std::vector<size_t> hits;
    for (size_t at = text.find(literal); at != std::string_view::npos && at + signature.size() <= text.size();
         at = text.find(literal, at + 1)) {
        bool match = true;
        for (size_t i = literal.size(); match && i < signature.size(); ++i)
            match = signature[i] < 0 || static_cast<uint8_t>(text[at + i]) == signature[i];
        if (match) hits.push_back(at);
    }
    return hits;
}

static uint8_t byteAt(std::string_view text, size_t at) { return static_cast<uint8_t>(text[at]); }

// bun.String.cloneLatin1: malloc(len + 0x14) (lea rdi, [reg+0x14]; call; test rax, rax), then
// an 8-bit WTF::StringImpl header: refcount 2 at [p] and the 8-bit flag 4 at [p+0x10].
// [r12] and [r12+0x10] take a SIB byte (c7 04 24 / c7 44 24 10).
static bool isLatin1Clone(std::string_view text, size_t function) {
    if (function >= text.size()) return false;
    std::string_view body = text.substr(function, 0x60);
    auto storeAt = [&](size_t at, uint8_t displacement, uint8_t value) -> size_t {
        if (at + 2 > body.size() || byteAt(body, at) != 0xc7) return 0;
        uint8_t modrm = byteAt(body, at + 1);
        size_t operand = at + 2;
        if (displacement == 0 ? modrm == 0x04 : modrm == 0x44) {
            if (operand >= body.size() || byteAt(body, operand) != 0x24) return 0;
            ++operand;
        } else if (displacement == 0 ? (modrm > 0x07 || modrm == 0x04 || modrm == 0x05)
                                     : (modrm < 0x40 || modrm > 0x47 || modrm == 0x44)) {
            return 0;
        }
        if (displacement != 0) {
            if (operand >= body.size() || byteAt(body, operand) != displacement) return 0;
            ++operand;
        }
        if (operand + 4 > body.size() || byteAt(body, operand) != value ||
            body.substr(operand + 1, 3) != std::string_view("\0\0\0", 3))
            return 0;
        return operand + 4;
    };
    for (size_t i = 0; i + 12 <= body.size(); ++i) {
        uint8_t rex = byteAt(body, i), modrm = byteAt(body, i + 2);
        if ((rex != 0x48 && rex != 0x49) || byteAt(body, i + 1) != 0x8d || modrm < 0x78 || modrm > 0x7f ||
            byteAt(body, i + 3) != 0x14 || byteAt(body, i + 4) != 0xe8 ||
            body.substr(i + 9, 3) != "\x48\x85\xc0")
            continue;
        for (size_t refs = i + 14; refs <= i + 30 && refs < body.size(); ++refs) {
            size_t end = storeAt(refs, 0, 0x02);
            if (!end) continue;
            for (size_t flags = end; flags <= end + 24 && flags < body.size(); ++flags)
                if (storeAt(flags, 0x10, 0x04)) return true;
        }
    }
    return false;
}

// The already-bundled branch of Bun's module loader builds its ResolvedSource from
// bun.String.cloneLatin1(contents), then input_specifier.createIfDifferent(path.text):
//   e8 rel32                        call  <clone>
//   (optional mov reg, [rbp-d])
//   REX.W 8b mod01 20               mov   reg, [spec+0x20]      (the specifier)
//   (up to 8 bytes)                 mov   reg, [rbp-d]          (the path, when spilled)
//   REX.W 8b mod01 20 REX.W 8b mod01 28  mov ptr/len, path.text
//   48 8d 7d|bd ...                 lea   rdi, [rbp-d]          (out-of-line createIfDifferent)
//     or: 3x (REX.W 89 mod11) e8 rel32 48 8d 7d|bd ...          (inlined createIfDifferent)
// The same argument setup follows other calls in that function; the site is the one
// whose callee is a Latin-1 clone.
struct CloneCall { size_t call; size_t clone; };
static bool isLoad64Disp8(std::string_view text, size_t at, uint8_t disp, uint8_t* baseOut = nullptr) {
    if (at + 4 > text.size()) return false;
    uint8_t rex = byteAt(text, at), modrm = byteAt(text, at + 2);
    if ((rex != 0x48 && rex != 0x49 && rex != 0x4c && rex != 0x4d) || byteAt(text, at + 1) != 0x8b ||
        (modrm >> 6) != 1 || (modrm & 7) == 4 || byteAt(text, at + 3) != disp)
        return false;
    if (baseOut) *baseOut = static_cast<uint8_t>(((rex & 1) << 3) | (modrm & 7));
    return true;
}
static bool isLeaRdiFrame(std::string_view text, size_t at) {
    if (at + 4 > text.size() || byteAt(text, at) != 0x48 || byteAt(text, at + 1) != 0x8d) return false;
    uint8_t modrm = byteAt(text, at + 2);
    return modrm == 0x7d || (modrm == 0xbd && at + 7 <= text.size());
}
static bool isRegMov64(std::string_view text, size_t at) {
    if (at + 3 > text.size()) return false;
    uint8_t rex = byteAt(text, at);
    return (rex == 0x48 || rex == 0x49 || rex == 0x4c || rex == 0x4d) &&
           byteAt(text, at + 1) == 0x89 && (byteAt(text, at + 2) >> 6) == 3;
}
static std::vector<CloneCall> findCloneCalls(std::string_view text) {
    std::vector<CloneCall> calls;
    for (size_t at = 0; at + 12 <= text.size(); ++at) {
        if (byteAt(text, at + 1) != 0x8b || byteAt(text, at + 3) != 0x20 ||
            byteAt(text, at + 5) != 0x8b || byteAt(text, at + 7) != 0x28)
            continue;
        uint8_t base0 = 0, base1 = 0;
        if (!isLoad64Disp8(text, at, 0x20, &base0) || !isLoad64Disp8(text, at + 4, 0x28, &base1) || base0 != base1)
            continue;
        size_t after = at + 8;
        bool tail = isLeaRdiFrame(text, after) ||
                    (after + 14 < text.size() && isRegMov64(text, after) && isRegMov64(text, after + 3) &&
                     isRegMov64(text, after + 6) && byteAt(text, after + 9) == 0xe8 &&
                     isLeaRdiFrame(text, after + 14));
        if (!tail) continue;
        for (size_t gap = 0; gap <= 8; ++gap) {
            if (at < gap + 4 + 5) break;
            size_t load = at - gap - 4;
            if (!isLoad64Disp8(text, load, 0x20)) continue;
            for (size_t width : {static_cast<size_t>(0), static_cast<size_t>(7), static_cast<size_t>(4)}) {
                if (load < width + 5) continue;
                if (width > 0) {
                    size_t frame = load - width;
                    uint8_t rex = byteAt(text, frame), modrm = byteAt(text, frame + 2);
                    if ((rex != 0x48 && rex != 0x4c) || byteAt(text, frame + 1) != 0x8b ||
                        (modrm & 0xc7) != (width == 7 ? 0x85 : 0x45))
                        continue;
                }
                size_t call = load - width - 5;
                if (byteAt(text, call) != 0xe8) continue;
                int32_t relative;
                memcpy(&relative, text.data() + call + 1, 4);
                int64_t target = static_cast<int64_t>(call) + 5 + relative;
                bool seen = false;
                for (const auto& known : calls) seen = seen || known.call == call;
                if (!seen && target >= 0 && static_cast<size_t>(target) < text.size() &&
                    isLatin1Clone(text, static_cast<size_t>(target)))
                    calls.push_back({call, static_cast<size_t>(target)});
            }
        }
    }
    return calls;
}

template<class ElfFileType>
static bool findTextSection(FileContents fc, size_t& off, size_t& size, uint64_t& addr) {
    try {
        ElfFileType elfFile(fc);
        auto info = elfFile.findSection(".text");
        if (!info) return false;
        off = info->offset; size = info->size; addr = info->addr;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

static int printNatives(FileContents fc) {
    const auto& bytes = *fc;
    if (isElf32(fc) || bytes.size() < 20 || bytes[EI_DATA] != ELFDATA2LSB ||
        (bytes[18] | (bytes[19] << 8)) != EM_X86_64) {
        fprintf(stderr, "--natives requires a little-endian x86-64 ELF64 executable\n");
        return 2;
    }
    size_t off = 0, size = 0;
    uint64_t addr = 0;
    if (!findTextSection<ElfFile64>(fc, off, size, addr) || off + size > bytes.size()) {
        fprintf(stderr, ".text section not found\n");
        return 2;
    }
    std::string_view text(reinterpret_cast<const char*>(bytes.data()) + off, size);
    std::vector<std::pair<std::string, uint64_t>> found;
    for (const auto& native : NATIVE_SIGNATURES) {
        auto hits = findSignature(text, parseSignature(native.pattern));
        if (hits.size() != 1) {
            fprintf(stderr, "%s: %zu signature matches in .text, not 1\n", native.name, hits.size());
            return 2;
        }
        found.emplace_back(native.name, addr + hits[0]);
    }
    auto calls = findCloneCalls(text);
    if (calls.size() != 1) {
        fprintf(stderr, "bundled_source_clone_call: %zu loader call sites, not 1\n", calls.size());
        return 2;
    }
    found.emplace_back("latin1_clone", addr + calls[0].clone);
    found.emplace_back("bundled_source_clone_call", addr + calls[0].call);
    printf("{\"format\": 1, \"natives\": {");
    for (size_t i = 0; i < found.size(); ++i)
        printf("%s\"%s\": %" PRIu64, i ? ", " : "", found[i].first.c_str(), found[i].second);
    printf("}}\n");
    return 0;
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

// ---- The payload's place in an ELF64 image, and --clean ----
//
// `bun build --compile` appends the standalone module graph to the runtime as
// [u64 length][payload], the `.bun` section.  Two layouts exist
// (oven-sh/bun src/exe_format/elf_payload.rs):
//
//   section   up to Bun 1.4: the writable PT_LOAD grows to cover the payload,
//             placed page-aligned past .bss.  The section header is repointed.
//   segment   since 2026-10-27 (bun build --compile --aot): a PT_BUN_PAYLOAD
//             header spans the payload at the highest address, followed by up
//             to three PT_LOAD pieces (data, ahead-of-time compiled code, data),
//             written into program headers the runtime reserved.  llvm-strip
//             may duplicate a section no single segment contains, so the
//             segment, not the section, is the payload's authority.
//
// Either way the linked `.bun` location, BUN_COMPILED right after .data, holds
// the payload's virtual address; the runtime reads it to find the graph.
//
// `--clean OUT` writes the runtime without the payload: the span leaves the
// file, the headers that mapped it shrink (section layout) or go back to
// PT_NULL (segment layout), BUN_COMPILED is cleared, the `.bun` header goes
// and the tail (non-loaded sections, the section header table) moves down.
// Everything the loader maps stays byte for byte: the caller compares.

namespace elf64 {

constexpr uint32_t PT_BUN_PAYLOAD = 0x6fb00002;
constexpr size_t MAX_PIECES = 3;

struct Image {
    explicit Image(FileContents contents) : fc(std::move(contents)) {}

    FileContents fc;
    Elf64_Ehdr ehdr{};
    std::vector<Elf64_Phdr> phdrs;
    std::vector<Elf64_Shdr> shdrs;
    std::string shstrtab;

    size_t size() const { return fc->size(); }
    const uint8_t* data() const { return fc->data(); }

    bool inFile(uint64_t offset, uint64_t length) const {
        return offset <= size() && length <= size() - offset;
    }

    std::string_view name(const Elf64_Shdr& sh) const {
        if (sh.sh_name >= shstrtab.size()) return {};
        std::string_view rest(shstrtab.data() + sh.sh_name, shstrtab.size() - sh.sh_name);
        return rest.substr(0, rest.find('\0'));
    }

    // A little-endian x86-64 ELF64 executable with its tables inside the file.
    bool parse(std::string& error) {
        if (size() < sizeof(Elf64_Ehdr) || memcmp(data(), ELFMAG, SELFMAG) != 0) {
            error = "not an ELF file"; return false;
        }
        memcpy(&ehdr, data(), sizeof ehdr);
        if (ehdr.e_ident[EI_CLASS] != ELFCLASS64 || ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
            ehdr.e_machine != EM_X86_64) {
            error = "requires a little-endian x86-64 ELF64 file"; return false;
        }
        if (ehdr.e_phentsize != sizeof(Elf64_Phdr) || ehdr.e_shentsize != sizeof(Elf64_Shdr)) {
            error = "unexpected program or section header size"; return false;
        }
        if (!inFile(ehdr.e_phoff, uint64_t(ehdr.e_phnum) * sizeof(Elf64_Phdr)) ||
            !inFile(ehdr.e_shoff, uint64_t(ehdr.e_shnum) * sizeof(Elf64_Shdr)) ||
            ehdr.e_shnum == 0 || ehdr.e_shstrndx >= ehdr.e_shnum) {
            error = "program or section header table outside the file"; return false;
        }
        phdrs.resize(ehdr.e_phnum);
        memcpy(phdrs.data(), data() + ehdr.e_phoff, phdrs.size() * sizeof(Elf64_Phdr));
        shdrs.resize(ehdr.e_shnum);
        memcpy(shdrs.data(), data() + ehdr.e_shoff, shdrs.size() * sizeof(Elf64_Shdr));
        const Elf64_Shdr& names = shdrs[ehdr.e_shstrndx];
        if (names.sh_type != SHT_STRTAB || !inFile(names.sh_offset, names.sh_size)) {
            error = "section name table outside the file"; return false;
        }
        shstrtab.assign(reinterpret_cast<const char*>(data() + names.sh_offset), names.sh_size);
        for (const Elf64_Shdr& sh : shdrs) {
            if (sh.sh_type != SHT_NOBITS && sh.sh_type != SHT_NULL && !inFile(sh.sh_offset, sh.sh_size)) {
                error = "section '" + std::string(name(sh)) + "' outside the file"; return false;
            }
        }
        for (const Elf64_Phdr& ph : phdrs) {
            if (ph.p_type != PT_NULL && !inFile(ph.p_offset, ph.p_filesz)) {
                error = "program header outside the file"; return false;
            }
        }
        return true;
    }

    int sectionIndex(std::string_view wanted) const {
        for (size_t i = 0; i < shdrs.size(); ++i)
            if (shdrs[i].sh_type != SHT_NULL && name(shdrs[i]) == wanted) return int(i);
        return -1;
    }

    uint64_t pageSize() const {
        uint64_t page = 0;
        for (const Elf64_Phdr& ph : phdrs)
            if (ph.p_type == PT_LOAD && ph.p_align > page) page = ph.p_align;
        return page ? page : 0x1000;
    }
};

static uint64_t alignUp(uint64_t value, uint64_t align) {
    if (align < 2) return value;
    return (value + align - 1) / align * align;
}

enum class Layout { Section, Segment };

struct Payload {
    Layout layout = Layout::Section;
    int section = -1;             // the `.bun` header, -1 when stripped
    int segment = -1;             // PT_BUN_PAYLOAD, or the PT_LOAD the section ends
    std::vector<int> pieces;      // the reserved headers after PT_BUN_PAYLOAD
    bool aot = false;             // a piece maps ahead-of-time compiled code
    uint64_t file_offset = 0;     // [u64 length][payload]
    uint64_t vaddr = 0;
    uint64_t length = 0;          // the payload behind its length
    uint64_t span_offset = 0;     // the page-aligned file span that leaves on --clean
    uint64_t span_end = 0;
};

static bool locatePayload(const Image& image, Payload& out, std::string& error) {
    Payload p;
    p.section = image.sectionIndex(".bun");
    for (size_t i = 0; i < image.phdrs.size(); ++i)
        if (image.phdrs[i].p_type == PT_BUN_PAYLOAD) {
            if (p.segment >= 0) { error = "two PT_BUN_PAYLOAD headers"; return false; }
            p.segment = int(i);
        }
    if (p.segment >= 0) {
        const Elf64_Phdr& span = image.phdrs[p.segment];
        p.layout = Layout::Segment;
        p.file_offset = span.p_offset;
        p.vaddr = span.p_vaddr;
        p.span_offset = span.p_offset;
        p.span_end = span.p_offset + span.p_filesz;
        if (span.p_filesz != span.p_memsz || span.p_filesz < 8) {
            error = "PT_BUN_PAYLOAD does not span a file-backed payload"; return false;
        }
        for (size_t i = p.segment + 1; i < image.phdrs.size() && p.pieces.size() < MAX_PIECES; ++i) {
            const Elf64_Phdr& ph = image.phdrs[i];
            bool inside = ph.p_type == PT_LOAD && ph.p_vaddr >= span.p_vaddr &&
                          ph.p_vaddr + ph.p_memsz <= span.p_vaddr + span.p_memsz;
            if (ph.p_type != PT_NULL && !inside) break;
            if (inside && (ph.p_flags & PF_X)) p.aot = true;
            p.pieces.push_back(int(i));
        }
        if (p.section >= 0) {
            const Elf64_Shdr& sh = image.shdrs[p.section];
            if (sh.sh_addr != span.p_vaddr || sh.sh_offset != span.p_offset || sh.sh_size > span.p_filesz) {
                error = ".bun section disagrees with PT_BUN_PAYLOAD"; return false;
            }
        }
    } else {
        if (p.section < 0) { error = ".bun section not found"; return false; }
        const Elf64_Shdr& sh = image.shdrs[p.section];
        if (sh.sh_type != SHT_PROGBITS || !(sh.sh_flags & SHF_ALLOC) || sh.sh_size < 8) {
            error = ".bun is not a loaded section with a length"; return false;
        }
        for (size_t i = 0; i < image.phdrs.size(); ++i) {
            const Elf64_Phdr& ph = image.phdrs[i];
            if (ph.p_type == PT_LOAD && ph.p_vaddr <= sh.sh_addr && sh.sh_addr < ph.p_vaddr + ph.p_memsz) {
                if (p.segment >= 0) { error = ".bun is mapped twice"; return false; }
                p.segment = int(i);
            }
        }
        if (p.segment < 0) { error = ".bun is not mapped"; return false; }
        const Elf64_Phdr& ph = image.phdrs[p.segment];
        p.file_offset = sh.sh_offset;
        p.vaddr = sh.sh_addr;
        p.span_offset = sh.sh_offset;
        p.span_end = ph.p_offset + ph.p_filesz;
        if (sh.sh_offset - ph.p_offset != sh.sh_addr - ph.p_vaddr ||
            sh.sh_offset + sh.sh_size > p.span_end ||
            alignUp(sh.sh_offset + sh.sh_size, image.pageSize()) < p.span_end) {
            error = ".bun does not end its PT_LOAD"; return false;
        }
    }
    if (!image.inFile(p.file_offset, 8)) { error = "payload length outside the file"; return false; }
    memcpy(&p.length, image.data() + p.file_offset, 8);
    uint64_t available = p.span_end - p.file_offset;
    if (p.length > available - 8) { error = "payload length exceeds its span"; return false; }
    if (p.section >= 0 && image.shdrs[p.section].sh_size != 8 + p.length) {
        error = ".bun section size does not match the payload length"; return false;
    }
    out = p;
    return true;
}

// The linked `.bun`: BUN_COMPILED, which the symbol table names, or the
// 8-aligned slot right after .data where the linker placed it.  It holds the
// payload's address.
static bool locatePointer(const Image& image, const Payload& payload, uint64_t& file_offset,
                          std::string& error) {
    uint64_t vaddr = 0;
    bool found = false;
    for (const Elf64_Shdr& sh : image.shdrs) {
        if (sh.sh_type != SHT_SYMTAB || sh.sh_link >= image.shdrs.size()) continue;
        const Elf64_Shdr& strings = image.shdrs[sh.sh_link];
        if (!image.inFile(strings.sh_offset, strings.sh_size)) continue;
        std::string_view strtab(reinterpret_cast<const char*>(image.data() + strings.sh_offset), strings.sh_size);
        for (uint64_t off = 0; off + sizeof(Elf64_Sym) <= sh.sh_size; off += sizeof(Elf64_Sym)) {
            Elf64_Sym sym;
            memcpy(&sym, image.data() + sh.sh_offset + off, sizeof sym);
            if (sym.st_name >= strtab.size()) continue;
            std::string_view rest = strtab.substr(sym.st_name);
            if (rest.substr(0, rest.find('\0')) == "BUN_COMPILED") { vaddr = sym.st_value; found = true; }
        }
    }
    if (!found) {
        int data = image.sectionIndex(".data");
        if (data < 0) { error = "neither BUN_COMPILED nor .data found"; return false; }
        const Elf64_Shdr& sh = image.shdrs[data];
        vaddr = alignUp(sh.sh_addr + sh.sh_size, 8);
    }
    for (const Elf64_Phdr& ph : image.phdrs) {
        if (ph.p_type != PT_LOAD || vaddr < ph.p_vaddr || vaddr + 8 > ph.p_vaddr + ph.p_filesz) continue;
        file_offset = ph.p_offset + (vaddr - ph.p_vaddr);
        uint64_t stored;
        memcpy(&stored, image.data() + file_offset, 8);
        if (stored != payload.vaddr) {
            char text[128];
            snprintf(text, sizeof text, "the standalone-graph pointer holds 0x%" PRIx64 ", not the payload's 0x%" PRIx64,
                     stored, payload.vaddr);
            error = text;
            return false;
        }
        return true;
    }
    error = "the standalone-graph pointer is not file-backed";
    return false;
}

static bool writeAll(const char* path, const std::vector<uint8_t>& bytes) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (fd < 0) { fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno)); return false; }
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
            close(fd);
            return false;
        }
        done += size_t(n);
    }
    if (close(fd) != 0) { fprintf(stderr, "cannot close %s: %s\n", path, strerror(errno)); return false; }
    return true;
}

static int cleanRuntime(FileContents fc, const char* out_path) {
    Image image(fc);
    std::string error;
    if (!image.parse(error) || image.ehdr.e_type != ET_EXEC) {
        fprintf(stderr, "--clean: %s\n", error.empty() ? "requires an ET_EXEC executable" : error.c_str());
        return 1;
    }
    Payload payload;
    if (!locatePayload(image, payload, error)) { fprintf(stderr, "--clean: %s\n", error.c_str()); return 1; }
    uint64_t pointer;
    if (!locatePointer(image, payload, pointer, error)) { fprintf(stderr, "--clean: %s\n", error.c_str()); return 1; }

    // The bytes that leave: the whole payload span, and in the section layout
    // also the zero page padding between the segment's last file-backed bytes
    // and the page-aligned payload.
    std::vector<Elf64_Phdr> phdrs = image.phdrs;
    uint64_t cut_begin = payload.span_offset, cut_end = payload.span_end;
    if (payload.layout == Layout::Segment) {
        phdrs[payload.segment] = Elf64_Phdr{};
        for (int piece : payload.pieces) phdrs[piece] = Elf64_Phdr{};
    } else {
        Elf64_Phdr& seg = phdrs[payload.segment];
        uint64_t filesz = 0, memsz = 0;
        for (size_t i = 0; i < image.shdrs.size(); ++i) {
            const Elf64_Shdr& sh = image.shdrs[i];
            if (int(i) == payload.section || !(sh.sh_flags & SHF_ALLOC) || sh.sh_type == SHT_NULL) continue;
            if (sh.sh_addr < seg.p_vaddr || sh.sh_addr + sh.sh_size > seg.p_vaddr + seg.p_memsz) continue;
            memsz = std::max(memsz, sh.sh_addr + sh.sh_size - seg.p_vaddr);
            if (sh.sh_type != SHT_NOBITS) filesz = std::max(filesz, sh.sh_offset + sh.sh_size - seg.p_offset);
        }
        if (filesz == 0 || memsz < filesz) { fprintf(stderr, "--clean: the payload's PT_LOAD has no other sections\n"); return 1; }
        // The pointer slot sits after .data, outside every section: keep it
        // mapped and file-backed when nothing follows it in the segment.
        if (pointer >= seg.p_offset && pointer < payload.span_offset) {
            filesz = std::max(filesz, pointer + 8 - seg.p_offset);
            memsz = std::max(memsz, filesz);
        }
        cut_begin = seg.p_offset + filesz;
        seg.p_filesz = filesz;
        seg.p_memsz = memsz;
    }
    if (cut_begin >= cut_end || image.ehdr.e_phoff + phdrs.size() * sizeof(Elf64_Phdr) > cut_begin ||
        image.ehdr.e_shoff < cut_end) {
        fprintf(stderr, "--clean: the header tables are not clear of the payload span\n");
        return 1;
    }
    for (size_t i = 0; i < image.shdrs.size(); ++i) {
        const Elf64_Shdr& sh = image.shdrs[i];
        if (int(i) == payload.section || sh.sh_type == SHT_NOBITS || sh.sh_type == SHT_NULL) continue;
        if (sh.sh_offset < cut_end && sh.sh_offset + sh.sh_size > cut_begin) {
            fprintf(stderr, "--clean: section '%s' lies in the payload span\n", std::string(image.name(sh)).c_str());
            return 1;
        }
    }
    if (pointer + 8 > cut_begin && pointer < cut_end) pointer = UINT64_MAX;   // zero-filled once cut
    if (pointer != UINT64_MAX && pointer + 8 > cut_begin) {
        fprintf(stderr, "--clean: the standalone-graph pointer lies past the payload\n");
        return 1;
    }

    // The head, then every section the cut moves (packed at its alignment in
    // file order), then the section header table.
    std::vector<uint8_t> out(image.data(), image.data() + cut_begin);
    if (pointer != UINT64_MAX) memset(out.data() + pointer, 0, 8);
    std::vector<Elf64_Shdr> shdrs = image.shdrs;
    std::vector<size_t> moved;
    for (size_t i = 0; i < shdrs.size(); ++i) {
        Elf64_Shdr& sh = shdrs[i];
        if (int(i) == payload.section || sh.sh_type == SHT_NULL) continue;
        if (sh.sh_type == SHT_NOBITS) {
            if (sh.sh_offset >= cut_begin && sh.sh_offset < cut_end) sh.sh_offset = cut_begin;
            else if (sh.sh_offset >= cut_end) sh.sh_offset -= cut_end - cut_begin;
            continue;
        }
        if (sh.sh_offset >= cut_end) moved.push_back(i);
    }
    std::sort(moved.begin(), moved.end(), [&](size_t a, size_t b) {
        return image.shdrs[a].sh_offset < image.shdrs[b].sh_offset;
    });
    for (size_t i : moved) {
        Elf64_Shdr& sh = shdrs[i];
        uint64_t at = alignUp(out.size(), sh.sh_addralign);
        out.resize(at, 0);
        out.insert(out.end(), image.data() + sh.sh_offset, image.data() + sh.sh_offset + sh.sh_size);
        sh.sh_offset = at;
    }

    // Drop the `.bun` header: later indices move down by one wherever a
    // header or a symbol names them.  Dynamic symbols stay: their section is
    // loaded, and the runtime never reads their section index.
    uint16_t removed = uint16_t(payload.section);
    auto renumber = [&](uint32_t index) -> uint32_t { return index > removed && index < SHN_LORESERVE ? index - 1 : index; };
    if (payload.section >= 0) {
        for (Elf64_Shdr& sh : shdrs) {
            sh.sh_link = renumber(sh.sh_link);
            if (sh.sh_flags & SHF_INFO_LINK) sh.sh_info = renumber(sh.sh_info);
        }
        for (size_t i : moved) {
            const Elf64_Shdr& sh = shdrs[i];
            if (sh.sh_type != SHT_SYMTAB) continue;
            for (uint64_t off = 0; off + sizeof(Elf64_Sym) <= sh.sh_size; off += sizeof(Elf64_Sym)) {
                Elf64_Sym sym;
                memcpy(&sym, out.data() + sh.sh_offset + off, sizeof sym);
                sym.st_shndx = uint16_t(renumber(sym.st_shndx));
                memcpy(out.data() + sh.sh_offset + off, &sym, sizeof sym);
            }
        }
        shdrs.erase(shdrs.begin() + payload.section);
    }
    Elf64_Ehdr ehdr = image.ehdr;
    ehdr.e_shoff = alignUp(out.size(), 8);
    ehdr.e_shnum = uint16_t(shdrs.size());
    if (payload.section >= 0) ehdr.e_shstrndx = uint16_t(renumber(ehdr.e_shstrndx));
    out.resize(ehdr.e_shoff, 0);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(shdrs.data()),
               reinterpret_cast<const uint8_t*>(shdrs.data() + shdrs.size()));
    memcpy(out.data(), &ehdr, sizeof ehdr);
    memcpy(out.data() + ehdr.e_phoff, phdrs.data(), phdrs.size() * sizeof(Elf64_Phdr));

    // Read the result back: the payload is unmapped, unnamed and unreferenced.
    auto result = std::make_shared<std::vector<unsigned char>>(out.begin(), out.end());
    Image check(result);
    if (!check.parse(error) || check.sectionIndex(".bun") >= 0) {
        fprintf(stderr, "--clean: the result is not a clean image: %s\n", error.c_str());
        return 1;
    }
    for (const Elf64_Phdr& ph : check.phdrs) {
        if ((ph.p_type == PT_LOAD || ph.p_type == PT_BUN_PAYLOAD) && ph.p_vaddr <= payload.vaddr &&
            payload.vaddr < ph.p_vaddr + ph.p_memsz) {
            fprintf(stderr, "--clean: the result still maps the payload\n");
            return 1;
        }
    }
    if (!writeAll(out_path, out)) return 1;
    printf("clean runtime: %s (%zu bytes; removed the %s-layout payload, %" PRIu64 " bytes at 0x%" PRIx64 "%s)\n",
           out_path, out.size(), payload.layout == Layout::Segment ? "segment" : "section",
           payload.length, payload.vaddr, payload.aot ? ", ahead-of-time compiled" : "");
    return 0;
}

}  // namespace elf64

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <bun-compiled-elf> [--extract DIR [--no-bytecode] | --natives | --clean OUT]\n", argv[0]);
        return 1;
    }

    bool extract = false, natives = false, clean = false, no_bytecode = false;
    std::string outdir = "/$bunfs", clean_path;
    for (int ai = 2; ai < argc; ++ai) {
        if (strcmp(argv[ai], "--natives") == 0) {
            natives = true;
        } else if (strcmp(argv[ai], "--no-bytecode") == 0) {
            no_bytecode = true;
        } else if (strcmp(argv[ai], "--clean") == 0) {
            if (ai + 1 >= argc) {
                fprintf(stderr, "--clean requires an output path\n");
                return 1;
            }
            clean = true;
            clean_path = argv[++ai];
        } else if (strcmp(argv[ai], "--extract") == 0) {
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

    if (int(natives) + int(extract) + int(clean) > 1) {
        fprintf(stderr, "--natives, --extract and --clean are separate modes\n");
        return 1;
    }
    if (no_bytecode && !extract) {
        fprintf(stderr, "--no-bytecode applies to --extract\n");
        return 1;
    }

    // Read file into memory using patchelf's FileContents type
    auto fileContents = readFileContents(argv[1]);
    if (!fileContents) return 1;
    // --natives prints only its JSON report, for tools to parse.
    if (natives) return printNatives(fileContents);
    if (clean) return elf64::cleanRuntime(fileContents, clean_path.c_str());

    // The payload: by its segment or section on little-endian ELF64 (where
    // --clean works too), by the section name elsewhere.
    BunSection bun;
    bool found;
    if (isElf32(fileContents) || fileContents->size() <= EI_DATA || (*fileContents)[EI_DATA] != ELFDATA2LSB) {
        found = isElf32(fileContents)
            ? findBunSection<ElfFile32>(fileContents, bun)
            : findBunSection<ElfFile64>(fileContents, bun);
        if (!found) {
            fprintf(stderr, ".bun section not found\n"); return 1;
        }
    } else {
        elf64::Image image(fileContents);
        elf64::Payload payload;
        std::string error;
        if (!image.parse(error) || !elf64::locatePayload(image, payload, error)) {
            fprintf(stderr, "%s\n", error.c_str()); return 1;
        }
        bun.file_offset = payload.file_offset;
        bun.size = 8 + payload.length;
        printf("payload layout: %s%s%s\n",
               payload.layout == elf64::Layout::Segment ? "segment (PT_BUN_PAYLOAD)" : "section (in the writable PT_LOAD)",
               payload.section < 0 ? ", no .bun header" : "",
               payload.aot ? ", ahead-of-time compiled code" : "");
        if (payload.aot && extract) {
            fprintf(stderr, "--extract: the payload holds ahead-of-time compiled code, not source to extract\n");
            return 1;
        }
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
            if (e.has_bytecode && !no_bytecode) {
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
