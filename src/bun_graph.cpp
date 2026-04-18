// bun_graph.cpp - Parse a Bun --compile ELF and print its Standalone Module Graph.
//   build:  g++ -std=c++17 -O2 -o bun_graph bun_graph.cpp
//   run:    ./bun_graph <claude-bin> [--extract [outdir]]

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <cerrno>

static const char BUN_MAGIC[] = "\n---- Bun! ----\n";
static constexpr size_t BUN_MAGIC_LEN = 16;
static constexpr size_t OFFSET_BIAS = 8;   // stored offsets are ELF-file-offset minus 8

struct Mapping {
    const uint8_t* data{};
    size_t size{};
    int fd{-1};
    ~Mapping() {
        if (data) munmap(const_cast<uint8_t*>(data), size);
        if (fd >= 0) close(fd);
    }
};

static bool map_file(const char* path, Mapping& m) {
    m.fd = open(path, O_RDONLY);
    if (m.fd < 0) { perror("open"); return false; }
    struct stat st{};
    if (fstat(m.fd, &st) < 0) { perror("fstat"); return false; }
    m.size = st.st_size;
    void* p = mmap(nullptr, m.size, PROT_READ, MAP_PRIVATE, m.fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return false; }
    m.data = static_cast<const uint8_t*>(p);
    return true;
}

struct BunSection {
    size_t file_offset;
    size_t size;
};

static bool find_bun_section(const Mapping& m, BunSection& out) {
    if (m.size < sizeof(Elf64_Ehdr)) return false;
    auto ehdr = reinterpret_cast<const Elf64_Ehdr*>(m.data);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "not an ELF file\n"); return false;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "only ELF64 supported\n"); return false;
    }

    auto range_fits = [&](size_t off, size_t len) {
        return off <= m.size && len <= m.size - off;
    };

    if (ehdr->e_shnum == 0 || ehdr->e_shstrndx >= ehdr->e_shnum) return false;
    if (!range_fits(ehdr->e_shoff,
                    static_cast<size_t>(ehdr->e_shnum) * sizeof(Elf64_Shdr))) {
        return false;
    }
    auto shdrs = reinterpret_cast<const Elf64_Shdr*>(m.data + ehdr->e_shoff);

    const Elf64_Shdr& strsh = shdrs[ehdr->e_shstrndx];
    if (!range_fits(strsh.sh_offset, strsh.sh_size) || strsh.sh_size == 0) {
        return false;
    }
    const char* shstr = reinterpret_cast<const char*>(m.data + strsh.sh_offset);

    for (size_t i = 0; i < ehdr->e_shnum; ++i) {
        if (shdrs[i].sh_name >= strsh.sh_size) return false;
        // Ensure the name is NUL-terminated within the string table.
        size_t max_name = strsh.sh_size - shdrs[i].sh_name;
        if (strnlen(shstr + shdrs[i].sh_name, max_name) == max_name) return false;
        if (strcmp(shstr + shdrs[i].sh_name, ".bun") == 0) {
            if (!range_fits(shdrs[i].sh_offset, shdrs[i].sh_size)) return false;
            out.file_offset = shdrs[i].sh_offset;
            out.size = shdrs[i].sh_size;
            return true;
        }
    }
    return false;
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

// Map an internal /$bunfs/... path to a relative output path.
// Strips the "/$bunfs/root/" prefix if present, otherwise drops the leading "/$bunfs/".
static std::string output_path(const std::string& bunfs) {
    const char prefix_root[] = "/$bunfs/root/";
    const char prefix_bare[] = "/$bunfs/";
    if (bunfs.size() > sizeof(prefix_root) - 1 &&
        memcmp(bunfs.data(), prefix_root, sizeof(prefix_root) - 1) == 0) {
        return bunfs.substr(sizeof(prefix_root) - 1);
    }
    if (bunfs.size() > sizeof(prefix_bare) - 1 &&
        memcmp(bunfs.data(), prefix_bare, sizeof(prefix_bare) - 1) == 0) {
        return bunfs.substr(sizeof(prefix_bare) - 1);
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

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <bun-compiled-elf> [--extract [outdir]]\n", argv[0]);
        return 1;
    }

    bool extract = false;
    std::string outdir = ".";
    for (int ai = 2; ai < argc; ++ai) {
        if (strcmp(argv[ai], "--extract") == 0) {
            extract = true;
            if (ai + 1 < argc && argv[ai + 1][0] != '-') {
                outdir = argv[++ai];
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[ai]);
            return 1;
        }
    }

    Mapping m;
    if (!map_file(argv[1], m)) return 1;

    BunSection bun;
    if (!find_bun_section(m, bun)) {
        fprintf(stderr, ".bun section not found\n"); return 1;
    }
    printf(".bun section: file_offset=0x%zx size=0x%zx (%zu bytes)\n",
           bun.file_offset, bun.size, bun.size);

    const uint8_t* sec = m.data + bun.file_offset;
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

    printf("\n%-4s %-40s %-12s %-10s %-12s %-10s %s\n",
           "idx", "name", "name_off", "name_len", "content_off", "content_len", "kind");
    for (size_t k = 0; k < entries.size(); ++k) {
        const auto& e = entries[k];
        printf("%-4zu %-40s 0x%010x %-10u 0x%010x %-10u %s\n",
               k, e.name.c_str(),
               e.stored_name_off, e.name_len,
               e.stored_content_off, e.content_len,
               e.kind);
    }

    printf("\ntotal entries: %zu\n", entries.size());
    printf("(stored offsets are section-relative; add 8 to dereference)\n");

    if (extract) {
        printf("\n--- extracting to %s/ ---\n", outdir.c_str());
        if (mkdir(outdir.c_str(), 0755) < 0 && errno != EEXIST) {
            perror(("mkdir " + outdir).c_str()); return 1;
        }
        size_t ok = 0, skipped = 0, unsafe = 0;
        for (const auto& e : entries) {
            if (!e.has_content) { ++skipped; continue; }
            std::string rel = output_path(e.name);
            if (!is_safe_relative(rel)) {
                fprintf(stderr, "  refusing unsafe path: %s\n", e.name.c_str());
                ++unsafe;
                continue;
            }
            std::string full = outdir + "/" + rel;
            size_t creal = (size_t)e.stored_content_off + OFFSET_BIAS;
            if (!write_file(full, sec + creal, e.content_len)) return 1;
            printf("  %-40s -> %s (%u bytes, %s)\n",
                   e.name.c_str(), full.c_str(), e.content_len, e.kind);
            ++ok;
        }
        printf("extracted: %zu, skipped (name-only): %zu, refused (unsafe path): %zu\n",
               ok, skipped, unsafe);
    }
    return 0;
}
