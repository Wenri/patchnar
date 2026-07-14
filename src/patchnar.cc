/*
 * patchnar - NAR stream patcher for Android compatibility
 *
 * Reads a NAR from stdin, patches ELF binaries, symlinks, and scripts,
 * and writes the modified NAR to stdout.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "nar.h"
#include "elf.h"
#include "patchelf.h"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <map>
#include <memory>
#include <unordered_set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Source-highlight: shared tokenization from source_patcher + CharTranslator for path translation
#include "source_patcher.h"
#include <boost/regex.hpp>

// Configuration (compile-time constants from configure)
static const std::string prefix = INSTALL_PREFIX;
// (glibc replacement is now handled uniformly through --mappings; the
// standard glibc -> android glibc pair is just another entry there.)

// Runtime configuration
static bool debugMode = false;

// Additional paths to prefix in script strings
// Default includes /nix/var/ which is commonly needed for nix daemon scripts
static std::vector<std::string> addPrefixToPaths = {"/nix/var/"};



// Byte-level path mappings.  Applied only to raw NAR/ELF content buffers
// (applyPathMappings) where the substring at a given offset must be
// replaced in place; requires OLD and NEW to be the same length so binary
// offsets in ELF `.rodata` etc. stay stable.  On Android, NEW typically
// points into the /data/data/com.termux.nix/nix/ symlink layer (real
// filesystem, kernel-resolvable — needed for static ELFs whose .rodata
// paths bypass the fakechroot LD_PRELOAD).
// e.g. "/nix/store/abc...-bash-5.2" -> "/data/data/com.termux.nix/nix/xyz1..."
static std::map<std::string, std::string> pathMappings;

// Structural path mappings.  Applied through length-flexible machinery
// (patchelf setInterpreter / modifyRPath, NAR symlink target strings,
// source-highlight for scripts, shebang text) so OLD and NEW may differ
// in length.  Typically NEW is the full real filesystem path (under
// installationDir) — no symlink hop needed at runtime for interp/RPATH.
// e.g. "/nix/store/abc...-bash-5.2" -> "/data/data/com.termux.nix/files/usr/nix/store/xyz...-bash-5.2"
static std::map<std::string, std::string> realPathMappings;

// Apply the two-layer mapping to a string.  Layer 1 (pathMappings,
// byte-level, same-length: OLD -> SYMLINK) always applies.  Layer 2
// (realPathMappings, structural, any length: SYMLINK -> REAL) applies
// where the string can be resized (scripts, ELF sections via patchelf).
// Same-length replacements collapse through both layers to a direct
// real-path reference; cross-length replacements (e.g. glibc) stay at
// SYMLINK after Layer 2 since realPathMappings has no entry, and the
// kernel resolves the symlink at runtime.
//
// Forward-declared here — defined after `applyPathMappingsToString`.
static std::string applyLayeredRewrite(std::string str);

// Custom PreFormatter that translates Nix store paths in string literals
// via the two-layer rewrite + installationDir prefix injection for any
// remaining raw /nix/store/ paths.
class NixPathTranslator : public srchilite::CharTranslator {
public:
    NixPathTranslator() : srchilite::CharTranslator() {}

protected:
    const std::string doPreformat(const std::string& text) override {
        // Layer 1 + Layer 2: OLD -> SYMLINK -> REAL where possible.
        std::string result = applyLayeredRewrite(text);

        // Add installationDir prefix to any remaining /nix/store/ paths
        // (paths not in either mapping table — e.g. cutoff packages) and
        // /nix/var/ or other caller-supplied prefixes.
        if (!prefix.empty()) {
            size_t pos = 0;
            while ((pos = result.find("/nix/store/", pos)) != std::string::npos) {
                bool alreadyPrefixed = (pos >= prefix.length() &&
                    result.substr(pos - prefix.length(), prefix.length()) == prefix);
                if (!alreadyPrefixed) {
                    result.insert(pos, prefix);
                    pos += prefix.length();
                }
                pos += 11;  // Skip "/nix/store/"
            }

            for (const auto& pattern : addPrefixToPaths) {
                pos = 0;
                while ((pos = result.find(pattern, pos)) != std::string::npos) {
                    bool alreadyPrefixed = (pos >= prefix.length() &&
                        result.substr(pos - prefix.length(), prefix.length()) == prefix);
                    if (!alreadyPrefixed) {
                        result.insert(pos, prefix);
                        pos += prefix.length();
                    }
                    pos += pattern.length();
                }
            }
        }

        return result;
    }
};


// Extensions to skip entirely (don't even call source-highlight)
// These are documentation, binary, or compressed files that never need patching
static const std::unordered_set<std::string> SKIP_EXTENSIONS = {
    // Documentation
    ".html", ".htm", ".xhtml", ".css", ".svg",
    // Images
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".webp", ".bmp",
    // Compressed/archives
    ".xz", ".gz", ".bz2", ".zst", ".zip", ".tar", ".7z",
    // Fonts
    ".ttf", ".otf", ".woff", ".woff2", ".eot",
    // Other binary/doc formats
    ".pdf", ".ps", ".dvi", ".info", ".texi", ".texinfo",
    // Haddock/Haskell docs
    ".haddock", ".hi", ".o", ".a", ".so", ".dylib",
};




// Whitelist of language files worth tokenizing for string literal patching
// These are script/config files that may contain /nix/store paths
// Default: shell scripts only; extensible via --add-lang option
static std::unordered_set<std::string> patchableLangFiles = {
    "sh.lang",
    "zsh.lang",
};

static void debug(const char* format, ...)
{
    if (debugMode) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}

// Check if file should be skipped based on extension (non-patchable files)
static inline bool shouldSkipByExtension(const std::string& filename)
{
    std::string ext = getExtension(filename);
    return !ext.empty() && SKIP_EXTENSIONS.count(ext) > 0;
}




// Add a single BYTE-LEVEL mapping.  Validates full-path length match —
// required for byte-safe in-place substitution in NAR/ELF content buffers.
static void addMapping(const std::string& oldPath, const std::string& newPath)
{
    if (oldPath.length() == newPath.length()) {
        debug("  mapping: %s -> %s\n", oldPath.c_str(), newPath.c_str());
        pathMappings.emplace(oldPath, newPath);
    } else {
        std::cerr << "patchnar: warning: skipping mapping " << oldPath
                  << " -> " << newPath << " (length mismatch: "
                  << oldPath.length() << " vs " << newPath.length() << ")\n";
    }
}

// Add a single STRUCTURAL mapping.  No length check — used by patchelf,
// source-highlight, and NAR symlink-string builders, all of which accept
// any target length.
static void addRealMapping(const std::string& oldPath, const std::string& newPath)
{
    debug("  real mapping: %s -> %s\n", oldPath.c_str(), newPath.c_str());
    realPathMappings.emplace(oldPath, newPath);
}

// Parse "OLD NEW"-per-line file into a caller-supplied consumer.
template<class Add>
static void loadMappingsFile(const std::string& filename, Add&& add)
{
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "patchnar: warning: cannot open mappings file: " << filename << "\n";
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        size_t space = line.find(' ');
        if (space == std::string::npos) continue;

        add(line.substr(0, space), line.substr(space + 1));
    }
}

static void loadMappings(const std::string& filename)
{
    loadMappingsFile(filename, addMapping);
    debug("patchnar: loaded %zu path mappings\n", pathMappings.size());
}

static void loadRealMappings(const std::string& filename)
{
    loadMappingsFile(filename, addRealMapping);
    debug("patchnar: loaded %zu real path mappings\n", realPathMappings.size());
}

// Apply path mappings to content (byte-level find-and-replace).
// Requires OLD and NEW full paths to be the same length so binary offsets
// (ELF `.rodata` strings, etc.) stay stable.
static void applyPathMappings(std::vector<std::byte>& content)
{
    if (pathMappings.empty()) return;

    // Convert to string for easier manipulation
    std::string str(reinterpret_cast<const char*>(content.data()), content.size());
    bool modified = false;

    for (const auto& [oldPath, newPath] : pathMappings) {
        size_t pos = 0;
        while ((pos = str.find(oldPath, pos)) != std::string::npos) {
            str.replace(pos, oldPath.length(), newPath);
            pos += newPath.length();
            modified = true;
        }
    }

    if (modified) {
        auto bytes = std::as_bytes(std::span(str));
        content.assign(bytes.begin(), bytes.end());
    }
}

// Apply BYTE-LEVEL path mappings (Layer 1) to a string.  Same semantics
// as `applyPathMappings(content)` but operating on `std::string` — used
// as the first stage of the layered rewrite in `applyLayeredRewrite`.
static std::string applyPathMappingsToString(std::string str)
{
    if (pathMappings.empty()) return str;

    for (const auto& [oldPath, newPath] : pathMappings) {
        size_t pos = 0;
        while ((pos = str.find(oldPath, pos)) != std::string::npos) {
            str.replace(pos, oldPath.length(), newPath);
            pos += newPath.length();
        }
    }
    return str;
}

// Check if content is an ELF file
static inline bool isElf(const std::span<const std::byte> content)
{
    if (content.size() < SELFMAG)
        return false;
    return memcmp(content.data(), ELFMAG, SELFMAG) == 0;
}

// Check if content has a shebang (starts with #!)
// Used only to determine if shebang patching should be applied
static inline bool hasShebang(const std::span<const std::byte> content)
{
    if (content.size() < 2)
        return false;
    return std::to_integer<char>(content[0]) == '#' && std::to_integer<char>(content[1]) == '!';
}

// Check if ELF is 32-bit
static inline bool isElf32(const std::span<const std::byte> content)
{
    if (content.size() < EI_CLASS + 1)
        return false;
    return std::to_integer<unsigned char>(content[EI_CLASS]) == ELFCLASS32;
}

// Replace occurrences of old path with new path in string
static std::string replaceAll(std::string str,
                              const std::string& from,
                              const std::string& to)
{
    if (from.empty())
        return str;

    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
    return str;
}

// Apply STRUCTURAL path mappings to a string (Layer 2: SYMLINK -> REAL,
// any length).  Used inside applyLayeredRewrite; not called directly.
static std::string applyRealPathMappingsToString(std::string str)
{
    if (realPathMappings.empty()) return str;

    for (const auto& [oldPath, newPath] : realPathMappings) {
        size_t pos = 0;
        while ((pos = str.find(oldPath, pos)) != std::string::npos) {
            str.replace(pos, oldPath.length(), newPath);
            pos += newPath.length();
        }
    }
    return str;
}

// Two-layer rewrite (forward-declared above for NixPathTranslator).
// Layer 1: pathMappings (OLD -> SYMLINK, byte-safe, covers every rewritten
// closure ref including cross-length pairs like glibc).
// Layer 2: realPathMappings (SYMLINK -> REAL, structural, same-length pairs
// only — cross-length pairs stay at SYMLINK and rely on kernel-level
// symlink resolution at runtime).
static std::string applyLayeredRewrite(std::string str)
{
    str = applyPathMappingsToString(std::move(str));
    str = applyRealPathMappingsToString(std::move(str));
    return str;
}

// Unified store path transformation for STRUCTURAL rewrites (any length).
// Used by: ELF interpreter, RPATH entries, symlinks, shebangs.
// Order: layered rewrite (Layer 1 then Layer 2), then installationDir
// prefix injection for any remaining raw /nix/store/ path prefix (cutoff
// packages, or paths not in the closure).
static std::string transformStorePath(std::string path)
{
    path = applyLayeredRewrite(std::move(path));

    if (path.rfind("/nix/store/", 0) == 0) {
        path.insert(0, prefix);
    }

    return path;
}

// Patch symlink target (takes by value to allow move semantics)
static std::string patchSymlink(std::string target)
{
    return transformStorePath(std::move(target));
}

// Build new RPATH from old RPATH by transforming each entry
static std::string buildNewRpath(const std::string& oldRpath)
{
    if (oldRpath.empty()) {
        return {};
    }

    std::string newRpath;
    newRpath.reserve(oldRpath.size() + prefix.size() * 4);  // Estimate with prefix additions

    std::string current;

    for (size_t i = 0; i <= oldRpath.size(); ++i) {
        if (i == oldRpath.size() || oldRpath[i] == ':') {
            if (!current.empty()) {
                if (!newRpath.empty()) {
                    newRpath += ':';
                }
                newRpath += transformStorePath(std::move(current));
                current.clear();
            }
        } else {
            current += oldRpath[i];
        }
    }

    return newRpath;
}

// Forward declarations for ELF patching (defined in patchelf.cc)
template<ElfFileParams>
class ElfFile;

// Patch ELF binary content
template<class ElfFileType>
static std::vector<std::byte> patchElfContent(
    const std::span<const std::byte> content,
    [[maybe_unused]] const bool executable)
{
    // Convert span to vector for patchelf (unique_ptr converts to shared_ptr)
    auto fileContents = std::make_unique<std::vector<unsigned char>>(
        reinterpret_cast<const unsigned char*>(content.data()),
        reinterpret_cast<const unsigned char*>(content.data()) + content.size());

    try {
        ElfFileType elfFile(std::move(fileContents));

        // Get current interpreter
        std::string interp;
        try {
            interp = elfFile.getInterpreter();
        } catch (...) {
            // No interpreter (probably a shared library)
            interp.clear();
        }

        // Patch interpreter using unified transformation
        if (!interp.empty()) {
            std::string newInterp = transformStorePath(interp);
            if (newInterp != interp) {
                debug("  interpreter: %s -> %s\n", interp.c_str(), newInterp.c_str());
                elfFile.setInterpreter(newInterp);
            }
        }

        // Patch RPATH/RUNPATH
        try {
            std::string currentRpath = elfFile.getRPath();
            if (!currentRpath.empty()) {
                std::string newRpath = buildNewRpath(currentRpath);
                if (newRpath != currentRpath) {
                    debug("  rpath: %s -> %s\n", currentRpath.c_str(), newRpath.c_str());
                    elfFile.modifyRPath(ElfFileType::rpSet, {}, newRpath);
                }
            }
        } catch (...) {
            // No RPATH section - that's fine
        }

        elfFile.rewriteSections();

        // Convert back to std::byte
        auto bytes = std::as_bytes(std::span(*elfFile.fileContents));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    } catch (const std::exception& e) {
        debug("  ELF patch failed: %s\n", e.what());
        // Return original content on error
        return std::vector<std::byte>(content.begin(), content.end());
    }
}

// Patch shebang only (fallback when language detection fails)
// Used for files with shebangs that can't be processed by source-highlight
static std::vector<std::byte> patchShebangOnly(const std::span<const std::byte> content)
{
    if (prefix.empty() || !hasShebang(content)) {
        return std::vector<std::byte>(content.begin(), content.end());
    }

    std::string str(reinterpret_cast<const char*>(content.data()), content.size());

    // Find end of shebang line
    size_t shebangEnd = str.find('\n');
    if (shebangEnd == std::string::npos) shebangEnd = str.size();

    std::string shebang = str.substr(0, shebangEnd);

    // Only patch if shebang contains /nix/store
    if (shebang.find("/nix/store/") == std::string::npos) {
        return {content.begin(), content.end()};
    }

    // Apply the two-layer rewrite (Layer 1: OLD -> SYMLINK; Layer 2:
    // SYMLINK -> REAL for same-length pairs).  Shebang text is
    // length-flexible so both layers are safe here.
    std::string newShebang = applyLayeredRewrite(shebang);

    // Add prefix to any remaining /nix/store paths (cutoff packages,
    // paths not in the closure).
    size_t pos = 2;  // Skip #!
    while ((pos = newShebang.find("/nix/store/", pos)) != std::string::npos) {
        if (pos < prefix.length() ||
            newShebang.substr(pos - prefix.length(), prefix.length()) != prefix) {
            newShebang.insert(pos, prefix);
            pos += prefix.length();
        }
        pos += 11;  // Skip "/nix/store/"
    }

    if (newShebang != shebang) {
        debug("  shebang (fallback): %s -> %s\n", shebang.c_str(), newShebang.c_str());
        str.replace(0, shebangEnd, newShebang);
        std::vector<std::byte> result(str.size());
        std::memcpy(result.data(), str.data(), str.size());
        return result;
    }
    return {content.begin(), content.end()};
}

// Patch source file content using source-highlight
// Strings AND comments (including shebangs) are patched via NixPathTranslator
static std::vector<std::byte> patchSource(
    const std::span<const std::byte> content,
    const std::string& langFile)
{
    if (prefix.empty() || langFile.empty()) {
        return std::vector<std::byte>(content.begin(), content.end());
    }

    std::string str(reinterpret_cast<const char*>(content.data()), content.size());
    NixPathTranslator translator;
    std::string patched = patchSourceStrings(str, langFile, translator);

    if (patched != str) {
        std::vector<std::byte> result(patched.size());
        std::memcpy(result.data(), patched.data(), patched.size());
        return result;
    }
    return {content.begin(), content.end()};
}

// Main content patcher
// The path parameter is the relative path within the NAR (e.g., "bin/bash", "share/nix/nix.sh")
static std::vector<std::byte> patchContent(
    const std::span<const std::byte> content,
    const bool executable,
    const std::string& path)
{
    // Extract filename from path
    std::string filename;
    size_t lastSlash = path.rfind('/');
    filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

    // === ELF FILES ===
    if (isElf(content)) {
        debug("  patching ELF %s (%zu bytes)\n", path.c_str(), content.size());
        auto result = isElf32(content)
            ? patchElfContent<ElfFile<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Addr, Elf32_Off, Elf32_Dyn, Elf32_Sym, Elf32_Versym, Elf32_Verdef, Elf32_Verdaux, Elf32_Verneed, Elf32_Vernaux, Elf32_Rel, Elf32_Rela, 32>>(content, executable)
            : patchElfContent<ElfFile<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Addr, Elf64_Off, Elf64_Dyn, Elf64_Sym, Elf64_Versym, Elf64_Verdef, Elf64_Verdaux, Elf64_Verneed, Elf64_Vernaux, Elf64_Rel, Elf64_Rela, 64>>(content, executable);
        applyPathMappings(result);
        return result;
    }

    // === SKIP NON-PATCHABLE EXTENSIONS ===
    if (shouldSkipByExtension(filename)) {
        debug("  skipping %s (non-patchable extension)\n", path.c_str());
        auto result = std::vector<std::byte>(content.begin(), content.end());
        applyPathMappings(result);
        return result;
    }

    // === LANGUAGE DETECTION ===
    std::string str(reinterpret_cast<const char*>(content.data()), content.size());
    std::string langFile = detectLanguageFromFile(filename, str);

    // === SOURCE PATCHING (strings + comments including shebangs) ===
    std::vector<std::byte> result;
    if (!langFile.empty() && patchableLangFiles.count(langFile)) {
        debug("  patching source %s (%zu bytes, lang=%s)\n",
              path.c_str(), content.size(), langFile.c_str());
        result = patchSource(content, langFile);
    } else if (hasShebang(content)) {
        // Fallback: patch shebang only when language detection fails
        // This handles scripts with unusual interpreters (e.g., ld.so)
        debug("  patching shebang-only %s (%zu bytes)\n", path.c_str(), content.size());
        result = patchShebangOnly(content);
    } else {
        if (!langFile.empty()) {
            debug("  skipping %s (lang=%s not in whitelist)\n", path.c_str(), langFile.c_str());
        }
        result = std::vector<std::byte>(content.begin(), content.end());
    }

    applyPathMappings(result);
    return result;
}

static void showHelp(const char* progName)
{
    std::cerr << "Usage: " << progName << " [OPTIONS]\n"
              << "\n"
              << "Patch NAR stream for Android compatibility.\n"
              << "Reads NAR from stdin, writes patched NAR to stdout.\n"
              << "\n"
              << "Compile-time settings:\n"
              << "  prefix:              " << prefix << "\n"
              << "  source-highlight:    " << sourceHighlightDataDir << "\n"
              << "  add-prefix-to:       /nix/var/ (default)\n"
              << "  patchable-langs:     sh.lang, zsh.lang (default)\n"
              << "\n"
              << "Options:\n"
              << "  --mappings FILE      Layer 1 (byte-level) mappings — applied to all\n"
              << "                       NAR content including ELF .rodata.  Format:\n"
              << "                       OLD NEW per line; OLD and NEW must have equal\n"
              << "                       full-path length (safe for byte substitution).\n"
              << "  --real-mappings FILE Layer 2 (structural) mappings — applied after\n"
              << "                       Layer 1 on ELF interp/RPATH, NAR symlink targets,\n"
              << "                       script literals, and shebangs.  Any length OK.\n"
              << "                       Typically maps SYMLINK -> REAL to skip a runtime\n"
              << "                       symlink hop for length-preservable references.\n"
              << "  --self-mapping MAP   Self-reference (populates only Layer 1; format:\n"
              << "                       \"OLD_PATH NEW_PATH\")\n"
              << "  --add-prefix-to PATH Additional path pattern to prefix in script strings\n"
              << "  --add-lang LANG      Additional language to patch (e.g., python.lang, json.lang)\n"
              << "  --debug              Enable debug output\n"
              << "  --help               Show this help\n";
}

int main(int argc, char** argv)
{
    static struct option longOptions[] = {
        {"mappings",                 required_argument, nullptr, 'm'},
        {"real-mappings",            required_argument, nullptr, 'M'},
        {"self-mapping",             required_argument, nullptr, 's'},
        {"add-prefix-to",            required_argument, nullptr, 'A'},
        {"add-lang",                 required_argument, nullptr, 'L'},
        {"debug",                    no_argument,       nullptr, 'd'},
        {"help",                     no_argument,       nullptr, 'h'},
        {nullptr,                    0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "m:M:s:A:L:dh", longOptions, nullptr)) != -1) {
        switch (opt) {
        case 'm':
            loadMappings(optarg);
            break;
        case 'M':
            loadRealMappings(optarg);
            break;
        case 's': {
            // Parse "OLD_PATH NEW_PATH" format.  Self-ref only appears in
            // byte-level substitution contexts (Layer 1 catches it before
            // any Layer 2 pass would see the OLD side).
            std::string arg = optarg;
            size_t space = arg.find(' ');
            if (space != std::string::npos) {
                std::string oldPath = arg.substr(0, space);
                std::string newPath = arg.substr(space + 1);
                addMapping(oldPath, newPath);
                debug("patchnar: self-mapping: %s -> %s\n", oldPath.c_str(), newPath.c_str());
            } else {
                std::cerr << "patchnar: error: --self-mapping requires \"OLD_PATH NEW_PATH\" format\n";
                return 1;
            }
            break;
        }
        case 'A':
            addPrefixToPaths.push_back(optarg);
            break;
        case 'L':
            patchableLangFiles.insert(optarg);
            break;
        case 'd':
            debugMode = true;
            break;
        case 'h':
            showHelp(argv[0]);
            return 0;
        default:
            showHelp(argv[0]);
            return 1;
        }
    }

    debug("patchnar: prefix=%s\n", prefix.c_str());
    debug("patchnar: source-highlight-data-dir=%s\n", sourceHighlightDataDir.c_str());
    for (const auto& path : addPrefixToPaths) {
        debug("patchnar: add-prefix-to=%s\n", path.c_str());
    }
    for (const auto& lang : patchableLangFiles) {
        debug("patchnar: patchable-lang=%s\n", lang.c_str());
    }

    try {
        // Set stdin/stdout to binary mode
        std::ios_base::sync_with_stdio(false);

        nar::NarProcessor processor(std::cin, std::cout);
        processor.setContentPatcher(patchContent);
        processor.setSymlinkPatcher(patchSymlink);
        processor.process();

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "patchnar: " << e.what() << "\n";
        return 1;
    }
}
