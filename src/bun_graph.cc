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
#include <unordered_map>

#include "elf.h"
#include "patchelf.h"

// Shared source-highlight tokenization for syntax-aware string patching
#include "source_patcher.h"

// ElfFile type aliases for cleaner template dispatch
using ElfFile32 = ElfFile<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Addr, Elf32_Off,
    Elf32_Dyn, Elf32_Sym, Elf32_Versym, Elf32_Verdef, Elf32_Verdaux,
    Elf32_Verneed, Elf32_Vernaux, Elf32_Rel, Elf32_Rela, 32>;
using ElfFile64 = ElfFile<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Addr, Elf64_Off,
    Elf64_Dyn, Elf64_Sym, Elf64_Versym, Elf64_Verdef, Elf64_Verdaux,
    Elf64_Verneed, Elf64_Vernaux, Elf64_Rel, Elf64_Rela, 64>;

static const char BUN_MAGIC[] = "\n---- Bun! ----\n";
static constexpr size_t BUN_MAGIC_LEN = 16;
static constexpr size_t OFFSET_BIAS = 8;   // stored offsets are ELF-file-offset minus 8

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

// A resolved entry discovered in the directory region.
struct Entry {
    size_t dir_offset;        // byte offset inside the directory region
    uint32_t stored_name_off; // as stored in the record
    uint32_t name_len;
    uint32_t stored_content_off;
    uint32_t content_len;
    std::string name;
    bool has_content;
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
    const uint8_t* sec = data + bun.file_offset;
    if (bun.size < BUN_MAGIC_LEN ||
        memcmp(sec + bun.size - BUN_MAGIC_LEN, BUN_MAGIC, BUN_MAGIC_LEN) != 0) {
        fprintf(stderr, "Bun trailer magic not found\n"); return 1;
    }
    printf("trailer magic: OK ('\\n---- Bun! ----\\n')\n");

    // Footer (immediately before magic), observed layout:
    //   u32 version/flags   (0x0001000a)
    //   u64 ptr_to_end      (near section end)
    //   u32 dir_offset      (ELF file offset of directory start)
    //   u32 dir_size        (directory region length in bytes)
    //   u32 padding
    //   u64 ptr_secondary
    //   u32 trailer_len_or_count
    //   <magic>
    static constexpr size_t FOOTER_LEN = 36;  // bytes before the magic
    if (bun.size < BUN_MAGIC_LEN + FOOTER_LEN) {
        fprintf(stderr, ".bun section too small for footer (%zu bytes)\n", bun.size);
        return 1;
    }
    size_t fe = bun.size - BUN_MAGIC_LEN;

    uint32_t trailer_tag;    memcpy(&trailer_tag,    sec + fe -  4, 4);
    uint64_t ptr_secondary;  memcpy(&ptr_secondary,  sec + fe - 12, 8);
    uint32_t pad;            memcpy(&pad,            sec + fe - 16, 4);
    uint32_t dir_size;       memcpy(&dir_size,       sec + fe - 20, 4);
    uint32_t dir_off;        memcpy(&dir_off,        sec + fe - 24, 4);
    uint64_t ptr_primary;    memcpy(&ptr_primary,    sec + fe - 32, 8);
    uint32_t flags_hdr;      memcpy(&flags_hdr,      sec + fe - 36, 4);

    printf("\n--- footer ---\n");
    printf("  flags/version : 0x%08x\n", flags_hdr);
    printf("  ptr_primary   : 0x%" PRIx64 "\n", ptr_primary);
    printf("  dir_offset    : 0x%08x  (.bun-relative)\n", dir_off);
    printf("  dir_size      : 0x%08x  (%u bytes)\n", dir_size, dir_size);
    printf("  ptr_secondary : 0x%" PRIx64 "\n", ptr_secondary);
    printf("  trailer_tag   : 0x%08x\n", trailer_tag);

    // dir_off is section-relative (offset within the .bun section)
    if ((size_t)dir_off + dir_size > bun.size) {
        fprintf(stderr, "directory out of .bun section\n"); return 1;
    }
    const uint8_t* dir = sec + dir_off;

    // Scan the directory for (u32 stored_offset, u32 length) pairs that resolve to
    // either "/$bunfs/..." paths or "// @bun ..." content blocks. Records in the
    // observed format place name_off/len immediately followed by content_off/len,
    // so we pair a found name with the content pair right after it.
    printf("\n--- directory records ---\n");
    std::vector<Entry> entries;
    size_t i = 0;
    while (i + 8 <= dir_size) {
        uint32_t off, ln;
        memcpy(&off,  dir + i,     4);
        memcpy(&ln,   dir + i + 4, 4);

        // Does (off, ln) resolve to a /$bunfs/ path? (offsets are section-relative)
        size_t real = (size_t)off + OFFSET_BIAS;
        if (off > 0 && real + ln <= bun.size
            && is_bunfs_path(sec + real, ln, bun.size)) {

            Entry e{};
            e.dir_offset = i;
            e.stored_name_off = off;
            e.name_len = ln;
            e.name.assign(reinterpret_cast<const char*>(sec + real), ln);

            // Look at the following (off, len) — does it point to a content blob?
            // Accept "// @bun ..." (JS), "\x7fELF" (native .node), or any plausible blob
            // whose offset/length stay inside the section.
            e.kind = "name-only";
            if (i + 16 <= dir_size) {
                uint32_t coff, clen;
                memcpy(&coff, dir + i + 8,  4);
                memcpy(&clen, dir + i + 12, 4);
                size_t creal = (size_t)coff + OFFSET_BIAS;
                // Content must physically follow the name in the section
                // (rejects mis-paired flag bytes being read as a bogus offset).
                bool follows_name = creal >= real + ln;
                if (follows_name && coff > 0 && clen >= 4 && creal + clen <= bun.size) {
                    const uint8_t* cp = sec + creal;
                    if (clen >= 7 && memcmp(cp, "// @bun", 7) == 0) {
                        e.kind = "js";
                        e.has_content = true;
                    } else if (memcmp(cp, "\x7f""ELF", 4) == 0) {
                        e.kind = "elf";
                        e.has_content = true;
                    } else if (memcmp(cp, "\0asm", 4) == 0) {
                        e.kind = "wasm";
                        e.has_content = true;
                    }
                    if (e.has_content) {
                        e.stored_content_off = coff;
                        e.content_len = clen;
                    }
                }
            }
            entries.push_back(std::move(e));
            i += e.has_content ? 16 : 8;
            continue;
        }
        i += 4;
    }

    printf("\n%-4s %-50s %-12s %-10s %-12s %-10s %s\n",
           "idx", "path", "name_off", "name_len", "content_off", "content_len", "kind");
    for (size_t k = 0; k < entries.size(); ++k) {
        const auto& e = entries[k];
        std::string path = output_path(e.name, outdir);
        printf("%-4zu %-50s 0x%010x %-10u 0x%010x %-10u %s\n",
               k, path.c_str(),
               e.stored_name_off, e.name_len,
               e.stored_content_off, e.content_len,
               e.kind);
    }

    printf("\ntotal entries: %zu\n", entries.size());
    printf("(stored offsets are section-relative; add 8 to dereference)\n");

    if (extract) {
        printf("\n--- extracting ---\n");
        size_t ok = 0, skipped = 0, linked = 0, unsafe = 0;

        // First pass: extract content entries, remember path mapping
        std::unordered_map<std::string, std::string> extracted; // bunfs name -> output path
        for (const auto& e : entries) {
            if (!e.has_content) continue;
            std::string full = output_path(e.name, outdir);
            if (!is_safe_relative(full) && full.front() != '/') {
                fprintf(stderr, "  refusing unsafe path: %s\n", full.c_str());
                ++unsafe;
                continue;
            }
            size_t creal = (size_t)e.stored_content_off + OFFSET_BIAS;
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
            extracted[e.name] = full;
            ++ok;
        }

        // Second pass: handle name-only entries (symlinks for aliases)
        for (const auto& e : entries) {
            if (e.has_content) continue;
            std::string full = output_path(e.name, outdir);
            auto it = extracted.find(e.name);
            if (it != extracted.end() && it->second == full) {
                // Same path as content entry — duplicate, no action needed
                printf("  %s (name-only, duplicate)\n", full.c_str());
                ++skipped;
            } else if (it != extracted.end()) {
                // Different output path — create symlink
                mkparents(full);
                if (symlink(it->second.c_str(), full.c_str()) != 0) {
                    fprintf(stderr, "  symlink %s -> %s: %s\n",
                            full.c_str(), it->second.c_str(), strerror(errno));
                } else {
                    printf("  %s -> %s (symlink)\n", full.c_str(), it->second.c_str());
                    ++linked;
                }
            } else {
                printf("  %s (name-only, no content)\n", full.c_str());
                ++skipped;
            }
        }
        printf("extracted: %zu, symlinked: %zu, skipped (name-only): %zu, refused (unsafe path): %zu\n",
               ok, linked, skipped, unsafe);
    }
    return 0;
}
