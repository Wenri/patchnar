/* bun-clean-fixture: a `bun build --compile` executable in miniature, in either
 * payload layout, and the checks a clean copy of it must pass.
 *
 *   bun-clean-fixture gen section|segment|none OUT
 *   bun-clean-fixture check section|segment ORIGINAL CLEAN
 *
 * The image: a read-only PT_LOAD with the headers and .text, a writable one
 * with .data, the standalone-graph pointer right after it, and .bss; the
 * payload [u64 length][bytes] page-aligned past .bss, either inside the
 * writable PT_LOAD grown over it (section) or under PT_BUN_PAYLOAD with two
 * PT_LOAD pieces, the second executable, and a PT_NULL (segment).  The tail
 * holds .comment, .symtab (a symbol in .bss, whose index follows .bun's; the
 * segment image also names BUN_COMPILED), .strtab and .shstrtab.
 */
#include <elf.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x1000u
#define BASE 0x400000u
#define PT_BUN_PAYLOAD 0x6fb00002u

enum { SH_NULL, SH_TEXT, SH_DATA, SH_BUN, SH_BSS, SH_COMMENT, SH_SYMTAB, SH_STRTAB, SH_SHSTRTAB, SH_COUNT };
static const char shstrtab[] = "\0.text\0.data\0.bun\0.bss\0.comment\0.symtab\0.strtab\0.shstrtab";
static const char strtab[] = "\0bss_sym\0BUN_COMPILED";
static const char comment[] = "bun-clean-fixture";

static const uint64_t TEXT_OFF = 0x200, TEXT_SIZE = 16;
static const uint64_t DATA_OFF = 0x1000, DATA_SIZE = 0x100, BSS_SIZE = 0x300;
static const uint64_t POINTER_OFF = 0x1100;                 /* 8-aligned, right after .data */
static const uint64_t RW_FILESZ = 0x108, RW_MEMSZ = 0x408;
static const uint64_t PAYLOAD_OFF = 0x2000, PAYLOAD_VADDR = BASE + 0x2000;

static void die(const char *what) { fprintf(stderr, "bun-clean-fixture: %s\n", what); exit(1); }

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) { perror(path); exit(1); }
    long n = ftell(f);
    if (n < 0) { perror(path); exit(1); }
    rewind(f);
    uint8_t *bytes = malloc((size_t)n + 1);
    if (!bytes || fread(bytes, 1, (size_t)n, f) != (size_t)n) die("short read");
    fclose(f);
    *size = (size_t)n;
    return bytes;
}

static uint16_t sh_name(const char *name) {
    for (size_t i = 0; i < sizeof shstrtab; i += strlen(shstrtab + i) + 1)
        if (strcmp(shstrtab + i, name) == 0) return (uint16_t)i;
    die("section name");
    return 0;
}

static uint32_t st_name(const char *name) {
    for (size_t i = 0; i < sizeof strtab; i += strlen(strtab + i) + 1)
        if (strcmp(strtab + i, name) == 0) return (uint32_t)i;
    die("symbol name");
    return 0;
}

static int gen(const char *layout, const char *out) {
    int section = strcmp(layout, "section") == 0, segment = strcmp(layout, "segment") == 0;
    if (!section && !segment && strcmp(layout, "none") != 0) die("layout: section, segment or none");
    uint64_t payload_len = segment ? 0x1800 : 100;             /* the segment payload reaches its code page */
    uint64_t span = segment ? 2 * PAGE : PAGE;
    uint64_t tail = PAYLOAD_OFF + (section || segment ? span : 0);
    if (!section && !segment) tail = DATA_OFF + RW_FILESZ;
    size_t phnum = segment ? 7 : 3;
    uint8_t *image = calloc(tail + 0x1000, 1);
    if (!image) die("memory");

    /* .text: a function that returns, at the entry point */
    memset(image + TEXT_OFF, 0xc3, TEXT_SIZE);
    for (uint64_t i = 0; i < DATA_SIZE; ++i) image[DATA_OFF + i] = (uint8_t)(0x40 + i % 61);
    if (section || segment) {
        uint64_t pointer = PAYLOAD_VADDR;
        memcpy(image + POINTER_OFF, &pointer, 8);
        memcpy(image + PAYLOAD_OFF, &payload_len, 8);
        for (uint64_t i = 0; i < payload_len; ++i) image[PAYLOAD_OFF + 8 + i] = (uint8_t)(0x30 + i % 10);
    }

    Elf64_Phdr ph[7];
    memset(ph, 0, sizeof ph);
    ph[0] = (Elf64_Phdr){ .p_type = PT_LOAD, .p_flags = PF_R, .p_offset = 0, .p_vaddr = BASE, .p_paddr = BASE,
                          .p_filesz = TEXT_OFF + TEXT_SIZE, .p_memsz = TEXT_OFF + TEXT_SIZE, .p_align = PAGE };
    ph[1] = (Elf64_Phdr){ .p_type = PT_LOAD, .p_flags = PF_R | PF_W, .p_offset = DATA_OFF, .p_vaddr = BASE + DATA_OFF,
                          .p_paddr = BASE + DATA_OFF, .p_filesz = RW_FILESZ, .p_memsz = RW_MEMSZ, .p_align = PAGE };
    ph[2] = (Elf64_Phdr){ .p_type = PT_GNU_STACK, .p_flags = PF_R | PF_W, .p_align = 16 };
    if (section) {
        ph[1].p_filesz = ph[1].p_memsz = PAYLOAD_OFF - DATA_OFF + span;   /* grown over the payload */
    } else if (segment) {
        ph[3] = (Elf64_Phdr){ .p_type = PT_BUN_PAYLOAD, .p_flags = PF_R, .p_offset = PAYLOAD_OFF, .p_vaddr = PAYLOAD_VADDR,
                              .p_paddr = PAYLOAD_VADDR, .p_filesz = span, .p_memsz = span, .p_align = PAGE };
        ph[4] = (Elf64_Phdr){ .p_type = PT_LOAD, .p_flags = PF_R, .p_offset = PAYLOAD_OFF, .p_vaddr = PAYLOAD_VADDR,
                              .p_paddr = PAYLOAD_VADDR, .p_filesz = PAGE, .p_memsz = PAGE, .p_align = PAGE };
        ph[5] = (Elf64_Phdr){ .p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_offset = PAYLOAD_OFF + PAGE,
                              .p_vaddr = PAYLOAD_VADDR + PAGE, .p_paddr = PAYLOAD_VADDR + PAGE, .p_filesz = PAGE,
                              .p_memsz = PAGE, .p_align = PAGE };
        ph[6] = (Elf64_Phdr){ .p_type = PT_NULL };
    }

    /* the tail: .comment, .symtab, .strtab, .shstrtab, then the section headers */
    uint64_t comment_off = tail;
    uint64_t symtab_off = (comment_off + sizeof comment + 7) & ~7ull;
    Elf64_Sym syms[3];
    memset(syms, 0, sizeof syms);
    syms[1] = (Elf64_Sym){ .st_name = st_name("bss_sym"), .st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT),
                           .st_shndx = SH_BSS, .st_value = BASE + POINTER_OFF + 8, .st_size = 8 };
    size_t nsyms = 2;
    if (segment) {
        syms[2] = (Elf64_Sym){ .st_name = st_name("BUN_COMPILED"), .st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT),
                               .st_shndx = SH_DATA, .st_value = BASE + POINTER_OFF, .st_size = 8 };
        nsyms = 3;
    }
    uint64_t strtab_off = symtab_off + nsyms * sizeof(Elf64_Sym);
    uint64_t shstrtab_off = strtab_off + sizeof strtab;
    uint64_t shoff = (shstrtab_off + sizeof shstrtab + 7) & ~7ull;
    size_t shnum = (section || segment) ? SH_COUNT : SH_COUNT - 1;
    size_t total = shoff + shnum * sizeof(Elf64_Shdr);
    uint8_t *file = calloc(total, 1);
    if (!file) die("memory");
    memcpy(file, image, tail);
    memcpy(file + comment_off, comment, sizeof comment);
    memcpy(file + symtab_off, syms, nsyms * sizeof(Elf64_Sym));
    memcpy(file + strtab_off, strtab, sizeof strtab);
    memcpy(file + shstrtab_off, shstrtab, sizeof shstrtab);

    Elf64_Shdr sh[SH_COUNT];
    memset(sh, 0, sizeof sh);
    sh[SH_TEXT] = (Elf64_Shdr){ .sh_name = sh_name(".text"), .sh_type = SHT_PROGBITS, .sh_flags = SHF_ALLOC | SHF_EXECINSTR,
                                .sh_addr = BASE + TEXT_OFF, .sh_offset = TEXT_OFF, .sh_size = TEXT_SIZE, .sh_addralign = 16 };
    sh[SH_DATA] = (Elf64_Shdr){ .sh_name = sh_name(".data"), .sh_type = SHT_PROGBITS, .sh_flags = SHF_ALLOC | SHF_WRITE,
                                .sh_addr = BASE + DATA_OFF, .sh_offset = DATA_OFF, .sh_size = DATA_SIZE, .sh_addralign = 64 };
    sh[SH_BUN] = (Elf64_Shdr){ .sh_name = sh_name(".bun"), .sh_type = SHT_PROGBITS, .sh_flags = SHF_ALLOC | SHF_WRITE,
                               .sh_addr = PAYLOAD_VADDR, .sh_offset = PAYLOAD_OFF, .sh_size = 8 + payload_len, .sh_addralign = 8 };
    sh[SH_BSS] = (Elf64_Shdr){ .sh_name = sh_name(".bss"), .sh_type = SHT_NOBITS, .sh_flags = SHF_ALLOC | SHF_WRITE,
                               .sh_addr = BASE + POINTER_OFF + 8, .sh_offset = POINTER_OFF + 8, .sh_size = BSS_SIZE, .sh_addralign = 8 };
    sh[SH_COMMENT] = (Elf64_Shdr){ .sh_name = sh_name(".comment"), .sh_type = SHT_PROGBITS, .sh_flags = SHF_MERGE | SHF_STRINGS,
                                   .sh_offset = comment_off, .sh_size = sizeof comment, .sh_addralign = 1, .sh_entsize = 1 };
    sh[SH_SYMTAB] = (Elf64_Shdr){ .sh_name = sh_name(".symtab"), .sh_type = SHT_SYMTAB, .sh_offset = symtab_off,
                                  .sh_size = nsyms * sizeof(Elf64_Sym), .sh_link = SH_STRTAB, .sh_info = 1, .sh_addralign = 8,
                                  .sh_entsize = sizeof(Elf64_Sym) };
    sh[SH_STRTAB] = (Elf64_Shdr){ .sh_name = sh_name(".strtab"), .sh_type = SHT_STRTAB, .sh_offset = strtab_off,
                                  .sh_size = sizeof strtab, .sh_addralign = 1 };
    sh[SH_SHSTRTAB] = (Elf64_Shdr){ .sh_name = sh_name(".shstrtab"), .sh_type = SHT_STRTAB, .sh_offset = shstrtab_off,
                                    .sh_size = sizeof shstrtab, .sh_addralign = 1 };
    if (section) sh[SH_BSS].sh_offset = DATA_OFF + RW_FILESZ;    /* nominal, as a linker leaves it */
    Elf64_Shdr *table = (Elf64_Shdr *)(file + shoff);
    size_t at = 0;
    for (size_t i = 0; i < SH_COUNT; ++i) {
        if (i == SH_BUN && !(section || segment)) continue;
        Elf64_Shdr copy = sh[i];
        if (!(section || segment)) {                            /* no .bun: later indices move down */
            if (copy.sh_link > SH_BUN) --copy.sh_link;
        }
        table[at++] = copy;
    }
    if (!(section || segment)) {
        Elf64_Sym *written = (Elf64_Sym *)(file + symtab_off);
        written[1].st_shndx = SH_BSS - 1;
    }

    Elf64_Ehdr eh;
    memset(&eh, 0, sizeof eh);
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_EXEC;
    eh.e_machine = EM_X86_64;
    eh.e_version = EV_CURRENT;
    eh.e_entry = BASE + TEXT_OFF;
    eh.e_phoff = sizeof eh;
    eh.e_shoff = shoff;
    eh.e_ehsize = sizeof eh;
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = (uint16_t)phnum;
    eh.e_shentsize = sizeof(Elf64_Shdr);
    eh.e_shnum = (uint16_t)shnum;
    eh.e_shstrndx = (uint16_t)(shnum - 1);
    memcpy(file, &eh, sizeof eh);
    memcpy(file + sizeof eh, ph, phnum * sizeof(Elf64_Phdr));

    FILE *f = fopen(out, "wb");
    if (!f || fwrite(file, 1, total, f) != total || fclose(f) != 0) { perror(out); return 1; }
    free(file);
    free(image);
    return 0;
}

static int failed;
static void expect(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "bun-clean-fixture: check failed: %s\n", what); failed = 1; }
}

static int check(const char *layout, const char *original_path, const char *clean_path) {
    int section = strcmp(layout, "section") == 0;
    if (!section && strcmp(layout, "segment") != 0) die("layout: section or segment");
    size_t osize, csize;
    uint8_t *o = read_file(original_path, &osize), *c = read_file(clean_path, &csize);
    Elf64_Ehdr oe, ce;
    memcpy(&oe, o, sizeof oe);
    memcpy(&ce, c, sizeof ce);
    expect(csize < osize, "the clean image is smaller");
    expect(ce.e_shnum == oe.e_shnum - 1, "one section header fewer");
    expect(ce.e_shstrndx == oe.e_shstrndx - 1, "e_shstrndx moved down past .bun");
    expect(ce.e_phnum == oe.e_phnum && ce.e_phoff == oe.e_phoff, "the program header table stays");
    expect(ce.e_entry == oe.e_entry, "the entry point stays");
    expect(csize == ce.e_shoff + (size_t)ce.e_shnum * sizeof(Elf64_Shdr), "the section headers end the file");

    Elf64_Phdr *oph = (Elf64_Phdr *)(o + oe.e_phoff), *cph = (Elf64_Phdr *)(c + ce.e_phoff);
    uint64_t cut = 0;
    for (size_t i = 0; i < oe.e_phnum; ++i) {
        if (section && i == 1) {
            expect(cph[i].p_type == PT_LOAD && cph[i].p_filesz == RW_FILESZ && cph[i].p_memsz == RW_MEMSZ &&
                   cph[i].p_offset == oph[i].p_offset && cph[i].p_vaddr == oph[i].p_vaddr && cph[i].p_flags == oph[i].p_flags,
                   "the writable PT_LOAD shrinks back to .data, the pointer and .bss");
            cut = cph[i].p_offset + cph[i].p_filesz;
        } else if (!section && i >= 3) {
            Elf64_Phdr zero;
            memset(&zero, 0, sizeof zero);
            expect(memcmp(&cph[i], &zero, sizeof zero) == 0, "PT_BUN_PAYLOAD and its pieces go back to PT_NULL");
            cut = PAYLOAD_OFF;
        } else {
            expect(memcmp(&cph[i], &oph[i], sizeof(Elf64_Phdr)) == 0, "other program headers are unchanged");
        }
        expect(!(cph[i].p_type == PT_LOAD && cph[i].p_vaddr <= PAYLOAD_VADDR && PAYLOAD_VADDR < cph[i].p_vaddr + cph[i].p_memsz),
               "nothing maps the payload");
    }
    uint64_t pointer;
    memcpy(&pointer, o + POINTER_OFF, 8);
    expect(pointer == PAYLOAD_VADDR, "the original points at the payload");
    memcpy(&pointer, c + POINTER_OFF, 8);
    expect(pointer == 0, "the clean image's pointer is cleared");
    uint64_t head = oe.e_phoff + (uint64_t)oe.e_phnum * sizeof(Elf64_Phdr);   /* the headers are compared field by field */
    expect(head <= POINTER_OFF && cut >= POINTER_OFF + 8, "the pointer sits in the loaded, file-backed head");
    expect(memcmp(o + head, c + head, POINTER_OFF - head) == 0 &&
           memcmp(o + POINTER_OFF + 8, c + POINTER_OFF + 8, cut - POINTER_OFF - 8) == 0,
           "every other loaded byte is unchanged");

    Elf64_Shdr *osh = (Elf64_Shdr *)(o + oe.e_shoff), *csh = (Elf64_Shdr *)(c + ce.e_shoff);
    const char *onames = (const char *)(o + osh[oe.e_shstrndx].sh_offset), *cnames = (const char *)(c + csh[ce.e_shstrndx].sh_offset);
    for (size_t i = 0, j = 0; i < oe.e_shnum; ++i) {
        if (i == SH_BUN) continue;
        const Elf64_Shdr *a = &osh[i], *b = &csh[j++];
        expect(strcmp(onames + a->sh_name, cnames + b->sh_name) == 0, "section names keep their order");
        expect(a->sh_type == b->sh_type && a->sh_flags == b->sh_flags && a->sh_addr == b->sh_addr && a->sh_size == b->sh_size &&
               a->sh_addralign == b->sh_addralign && a->sh_entsize == b->sh_entsize && a->sh_info == b->sh_info,
               "section headers keep their fields");
        expect(b->sh_link == (a->sh_link > SH_BUN ? a->sh_link - 1 : a->sh_link), "sh_link follows the renumbering");
        if (a->sh_type == SHT_NOBITS) {
            expect(b->sh_offset == (section ? cut : a->sh_offset), ".bss's nominal offset sits at the cut");
        } else if (a->sh_type != SHT_NULL) {
            expect(a->sh_offset < cut ? b->sh_offset == a->sh_offset : b->sh_offset >= cut, "offsets before the cut stay");
            if (a->sh_type == SHT_SYMTAB) {
                Elf64_Sym *os = (Elf64_Sym *)(o + a->sh_offset), *cs = (Elf64_Sym *)(c + b->sh_offset);
                for (size_t k = 0; k < a->sh_size / sizeof(Elf64_Sym); ++k) {
                    expect(cs[k].st_shndx == (os[k].st_shndx > SH_BUN ? os[k].st_shndx - 1 : os[k].st_shndx),
                           "symbols follow the renumbering");
                    Elf64_Sym x = os[k], y = cs[k];
                    x.st_shndx = y.st_shndx = 0;
                    expect(memcmp(&x, &y, sizeof x) == 0, "symbols are otherwise unchanged");
                }
            } else {
                expect(memcmp(o + a->sh_offset, c + b->sh_offset, a->sh_size) == 0, "section bytes are unchanged");
            }
        }
    }
    for (size_t j = 0; j < ce.e_shnum; ++j) expect(strcmp(cnames + csh[j].sh_name, ".bun") != 0, "no .bun header remains");
    free(o);
    free(c);
    return failed;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "gen") == 0) return gen(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "check") == 0) return check(argv[2], argv[3], argv[4]);
    fprintf(stderr, "usage: %s gen section|segment|none OUT | check section|segment ORIGINAL CLEAN\n", argv[0]);
    return 2;
}
