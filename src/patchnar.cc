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
#include <unordered_map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Source-highlight includes for string literal tokenization (required)
#include <srchilite/sourcehighlighter.h>
#include <srchilite/formattermanager.h>
#include <srchilite/formatter.h>
#include <srchilite/formatterparams.h>
#include <srchilite/langdefmanager.h>
#include <srchilite/regexrulefactory.h>
#include <srchilite/langmap.h>
#include <srchilite/languageinfer.h>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>

// Configuration
static std::string prefix;
static std::string glibcPath;
static std::string oldGlibcPath;
static bool debugMode = false;

// Additional paths to prefix in script strings (e.g., "/nix/var/")
static std::vector<std::string> addPrefixToPaths;

// String region representing a string literal in source code
struct StringRegion {
    size_t start;  // Starting position in the content
    size_t end;    // Ending position in the content
};

// ============================================================================
// Tokenizer - Thread-safe wrapper for source-highlight
// ============================================================================
// Source-highlight uses flex internally which has global state, so we need
// a mutex to serialize access. This class encapsulates that protection.

class Tokenizer {
public:
    explicit Tokenizer(const std::string& dataDir) : dataDir_(dataDir) {}

    // Detect language from filename and/or content
    // Returns .lang filename (e.g., "sh.lang") or empty string
    std::string detectLanguage(const std::string& filename, const std::string& content);

    // Get string literal regions in source code
    // Returns empty vector if tokenization fails
    std::vector<StringRegion> getStringRegions(const std::string& content, const std::string& langFile);

private:
    std::string dataDir_;
    std::mutex mutex_;  // Protects flex scanner (not thread-safe)

    // Custom formatter that captures string literal positions
    class StringCapture : public srchilite::Formatter {
    public:
        std::vector<StringRegion>& regions;
        size_t lineOffset = 0;

        explicit StringCapture(std::vector<StringRegion>& r) : regions(r) {}

        void format(const std::string& s, const srchilite::FormatterParams* params) override {
            if (params && params->start >= 0) {
                size_t start = lineOffset + static_cast<size_t>(params->start);
                regions.push_back({start, start + s.length()});
            }
        }
    };

    // Null formatter that discards everything
    class NullFormatter : public srchilite::Formatter {
    public:
        void format(const std::string&, const srchilite::FormatterParams*) override {}
    };
};

// Global tokenizer instance (initialized in main when source-highlight data dir is set)
static std::unique_ptr<Tokenizer> tokenizer;

// Source-highlight data directory (set via --source-highlight-data-dir)
static std::string sourceHighlightDataDir;

// Hash mappings for inter-package reference substitution
// Maps old store path basename to new store path basename
// e.g., "abc123...-bash-5.2" -> "xyz789...-bash-5.2"
static std::map<std::string, std::string> hashMappings;

// Extensions to skip entirely (don't even call source-highlight)
// These are documentation, binary, or compressed files that never need patching
static const std::set<std::string> SKIP_EXTENSIONS = {
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

// Maximum file size for content-based language detection (shebang parsing)
// Scripts needing patching are typically small; large extensionless files are data/binary
static constexpr size_t MAX_CONTENT_DETECT_SIZE = 64 * 1024;  // 64KB

// Map file extensions to source-highlight .lang files
// This is the fast path - no source-highlight parsing needed
static const std::unordered_map<std::string, std::string> EXTENSION_TO_LANG = {
    // Shell
    {".sh", "sh.lang"}, {".bash", "sh.lang"}, {".zsh", "zsh.lang"},
    // Python
    {".py", "python.lang"}, {".pyw", "python.lang"},
    // Perl
    {".pl", "perl.lang"}, {".pm", "perl.lang"},
    // Ruby
    {".rb", "ruby.lang"},
    // Lua
    {".lua", "lua.lang"},
    // Tcl
    {".tcl", "tcl.lang"},
    // JavaScript/JSON
    {".js", "javascript.lang"}, {".mjs", "javascript.lang"},
    {".json", "json.lang"},
    // Config files
    {".conf", "conf.lang"}, {".cfg", "conf.lang"},
    {".desktop", "desktop.lang"},
    {".properties", "properties.lang"},
    {".ini", "ini.lang"},
    // Build systems
    {".mk", "makefile.lang"},
    {".m4", "m4.lang"},
    // XML
    {".xml", "xml.lang"},
    // Awk
    {".awk", "awk.lang"},
};

// Whitelist of language files worth tokenizing for string literal patching
// These are languages where /nix/store paths commonly appear in string literals
static const std::set<std::string> PATCHABLE_LANG_FILES = {
    // Shell scripts (most common case)
    "sh.lang",
    "zsh.lang",

    // Scripting languages
    "python.lang",
    "perl.lang",
    "ruby.lang",
    "lua.lang",
    "tcl.lang",

    // Web/JS (node scripts, configs)
    "javascript.lang",
    "json.lang",

    // Config files
    "conf.lang",
    "desktop.lang",
    "properties.lang",
    "ini.lang",

    // Build systems
    "makefile.lang",
    "m4.lang",

    // Other
    "xml.lang",
    "awk.lang",
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

// Get lowercase file extension (e.g., ".html", ".png")
static std::string getExtension(const std::string& filename)
{
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return "";
    }
    std::string ext = filename.substr(dot);
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

// Get .lang file from extension (fast path - O(1) hash lookup)
static std::string getLangFromExtension(const std::string& filename)
{
    std::string ext = getExtension(filename);
    auto it = EXTENSION_TO_LANG.find(ext);
    return (it != EXTENSION_TO_LANG.end()) ? it->second : "";
}

// Check if file should be skipped based on extension OR size
// - Non-patchable extensions (.html, .png) are always skipped
// - Large files without a patchable extension mapping are skipped
//   (would need content-based detection which is expensive)
static bool shouldSkipByExtension(const std::string& filename, size_t size)
{
    std::string ext = getExtension(filename);

    // Skip non-patchable extensions
    if (!ext.empty() && SKIP_EXTENSIONS.count(ext) > 0) {
        return true;
    }

    // Skip large files that would need content-based detection
    // (extension not in EXTENSION_TO_LANG means we'd need source-highlight)
    if (EXTENSION_TO_LANG.count(ext) == 0 && size > MAX_CONTENT_DETECT_SIZE) {
        return true;
    }

    return false;
}

// Tokenizer::detectLanguage - Detect language from filename and/or content
// Returns .lang filename (e.g., "sh.lang", "python.lang") or empty string
// Detection priority: 1) Filename-based (LangMap) 2) Content-based (LanguageInfer)
// Thread-safe: uses mutex to protect source-highlight access (flex uses global state)
std::string Tokenizer::detectLanguage(const std::string& filename, const std::string& content)
{
    // Lock for thread safety - source-highlight is not thread-safe
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        srchilite::LangMap langMap(dataDir_, "lang.map");

        // 1. Try filename-based detection first
        std::string langFile = langMap.getMappedFileNameFromFileName(filename);
        if (!langFile.empty()) {
            debug("  detected language from filename: %s -> %s\n",
                  filename.c_str(), langFile.c_str());
            return langFile;
        }

        // 2. Try content-based detection (shebang, emacs mode, xml, etc.)
        srchilite::LanguageInfer languageInfer;
        std::istringstream contentStream(content);
        std::string inferredLang = languageInfer.infer(contentStream);

        if (!inferredLang.empty()) {
            langFile = langMap.getMappedFileName(inferredLang);
            if (!langFile.empty()) {
                debug("  detected language from content: %s -> %s\n",
                      inferredLang.c_str(), langFile.c_str());
                return langFile;
            }
            debug("  inferred language '%s' but no mapping found\n", inferredLang.c_str());
        }

    } catch (const std::exception& e) {
        debug("  language detection failed: %s\n", e.what());
    }

    return "";
}

// Tokenizer::getStringRegions - Get string literal regions in source code
// Takes the .lang file directly (already detected by detectLanguage)
// Returns empty vector if language is empty or processing fails
// Thread-safe: uses mutex to protect source-highlight access (flex uses global state)
std::vector<StringRegion> Tokenizer::getStringRegions(const std::string& content, const std::string& langFile)
{
    std::vector<StringRegion> regions;

    if (langFile.empty()) {
        return regions;
    }

    // Lock for thread safety - source-highlight is not thread-safe
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        debug("  tokenizing with %s\n", langFile.c_str());

        // Initialize language definition manager with regex rule factory
        srchilite::RegexRuleFactory ruleFactory;
        srchilite::LangDefManager langDefManager(&ruleFactory);

        // Get the highlight state for the detected language
        srchilite::SourceHighlighter highlighter(
            langDefManager.getHighlightState(dataDir_, langFile));

        // Disable optimization to get accurate position information
        highlighter.setOptimize(false);

        // Create formatters (source-highlight uses boost::shared_ptr)
        auto stringCapture = boost::make_shared<StringCapture>(regions);
        auto nullFormatter = boost::make_shared<NullFormatter>();

        // Create formatter manager with null default formatter
        srchilite::FormatterManager formatterManager(nullFormatter);

        // Register string formatter - only captures string literals
        formatterManager.addFormatter("string", stringCapture);

        highlighter.setFormatterManager(&formatterManager);

        // Set up position tracking
        srchilite::FormatterParams params;
        highlighter.setFormatterParams(&params);

        // Process each line
        std::istringstream stream(content);
        std::string line;
        size_t lineOffset = 0;

        while (std::getline(stream, line)) {
            // Update formatter with current line offset
            stringCapture->lineOffset = lineOffset;
            params.start = 0;

            highlighter.highlightParagraph(line);

            // Move to next line (+1 for newline character)
            lineOffset += line.length() + 1;
        }

        debug("  found %zu string regions\n", regions.size());

    } catch (const std::exception& e) {
        debug("  source-highlight failed: %s\n", e.what());
        regions.clear();
    }

    return regions;
}

// Check if a position is inside any string region
static bool isInsideString(size_t pos, const std::vector<StringRegion>& regions)
{
    for (const auto& region : regions) {
        if (pos >= region.start && pos < region.end) {
            return true;
        }
    }
    return false;
}

// Add a single hash mapping from full store paths
// Extracts basenames and validates length match
static void addMapping(const std::string& oldPath, const std::string& newPath)
{
    // Extract basename (everything after last /)
    auto oldBase = oldPath.substr(oldPath.rfind('/') + 1);
    auto newBase = newPath.substr(newPath.rfind('/') + 1);

    // Validate same length (required for safe substitution in NAR)
    if (oldBase.length() == newBase.length()) {
        hashMappings[oldBase] = newBase;
        debug("  mapping: %s -> %s\n", oldBase.c_str(), newBase.c_str());
    } else {
        std::cerr << "patchnar: warning: skipping mapping " << oldBase
                  << " -> " << newBase << " (length mismatch: "
                  << oldBase.length() << " vs " << newBase.length() << ")\n";
    }
}

// Load hash mappings from file
// Format: one mapping per line: "/nix/store/old-hash-name /nix/store/new-hash-name"
static void loadMappings(const std::string& filename)
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

        std::string oldPath = line.substr(0, space);
        std::string newPath = line.substr(space + 1);
        addMapping(oldPath, newPath);
    }

    debug("patchnar: loaded %zu hash mappings\n", hashMappings.size());
}

// Apply hash mappings to content (text substitution, like sed)
// This replaces old store path basenames with new ones
static void applyHashMappings(std::vector<std::byte>& content)
{
    if (hashMappings.empty()) return;

    // Convert to string for easier manipulation
    std::string str(reinterpret_cast<const char*>(content.data()), content.size());
    bool modified = false;

    for (const auto& [oldHash, newHash] : hashMappings) {
        size_t pos = 0;
        while ((pos = str.find(oldHash, pos)) != std::string::npos) {
            str.replace(pos, oldHash.length(), newHash);
            pos += newHash.length();
            modified = true;
        }
    }

    if (modified) {
        auto* p = reinterpret_cast<const std::byte*>(str.data());
        content.assign(p, p + str.size());
    }
}

// Check if content is an ELF file
static bool isElf(std::span<const std::byte> content)
{
    if (content.size() < SELFMAG)
        return false;
    return memcmp(content.data(), ELFMAG, SELFMAG) == 0;
}

// Check if content has a shebang (starts with #!)
// Used only to determine if shebang patching should be applied
static bool hasShebang(std::span<const std::byte> content)
{
    if (content.size() < 2)
        return false;
    return std::to_integer<char>(content[0]) == '#' && std::to_integer<char>(content[1]) == '!';
}

// Check if ELF is 32-bit
static bool isElf32(std::span<const std::byte> content)
{
    if (content.size() < EI_CLASS + 1)
        return false;
    return std::to_integer<unsigned char>(content[EI_CLASS]) == ELFCLASS32;
}

// Replace occurrences of old path with new path in string
static std::string replaceAll(const std::string& str,
                              const std::string& from,
                              const std::string& to)
{
    if (from.empty())
        return str;

    std::string result = str;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.length(), to);
        pos += to.length();
    }
    return result;
}

// Apply hash mappings to a string (for symlinks, etc.)
static std::string applyHashMappingsToString(const std::string& str)
{
    if (hashMappings.empty()) return str;

    std::string result = str;
    for (const auto& [oldHash, newHash] : hashMappings) {
        size_t pos = 0;
        while ((pos = result.find(oldHash, pos)) != std::string::npos) {
            result.replace(pos, oldHash.length(), newHash);
            pos += newHash.length();
        }
    }
    return result;
}

// Patch symlink target
static std::string patchSymlink(const std::string& target)
{
    std::string newTarget = target;

    // IMPORTANT: glibc substitution MUST happen BEFORE hash mappings
    // This is the same order as transformRpathEntry to ensure consistency
    // Replace old glibc with Android glibc (for symlinks like ld.so)
    if (!oldGlibcPath.empty()) {
        // Try full path first (for absolute symlinks)
        if (newTarget.find(oldGlibcPath) != std::string::npos) {
            newTarget = replaceAll(newTarget, oldGlibcPath, glibcPath);
        } else {
            // For relative symlinks (e.g., ../../hash-glibc-version/lib/...)
            // Extract basenames and try to match those
            auto oldBase = oldGlibcPath.substr(oldGlibcPath.rfind('/') + 1);
            auto newBase = glibcPath.substr(glibcPath.rfind('/') + 1);
            if (!oldBase.empty() && newTarget.find(oldBase) != std::string::npos) {
                newTarget = replaceAll(newTarget, oldBase, newBase);
            }
        }
    }

    // Then apply hash mappings for inter-package references
    newTarget = applyHashMappingsToString(newTarget);

    // Finally add prefix to /nix/store paths
    if (newTarget.rfind("/nix/store/", 0) == 0) {
        newTarget = prefix + newTarget;
    }

    if (newTarget != target) {
        debug("  symlink: %s -> %s\n", target.c_str(), newTarget.c_str());
    }
    return newTarget;
}

// Transform a single RPATH entry
static std::string transformRpathEntry(const std::string& entry)
{
    std::string result = entry;

    // IMPORTANT: glibc substitution MUST happen BEFORE hash mappings
    // because hash mappings would change the path and prevent matching
    // gcc-lib is handled by hash mapping (same package, different hash)

    // Replace old glibc with new glibc (Android glibc)
    if (!oldGlibcPath.empty() && result.find(oldGlibcPath) != std::string::npos) {
        result = replaceAll(result, oldGlibcPath, glibcPath);
    }

    // Then apply hash mappings for inter-package references (including gcc-lib)
    result = applyHashMappingsToString(result);

    // Add prefix to /nix/store paths (for Android glibc and other libs)
    if (result.rfind("/nix/store/", 0) == 0) {
        result = prefix + result;
    }

    return result;
}

// Build new RPATH from old RPATH by transforming each entry
static std::string buildNewRpath(const std::string& oldRpath)
{
    if (oldRpath.empty()) {
        return "";
    }

    std::string newRpath;
    std::string current;

    for (size_t i = 0; i <= oldRpath.size(); ++i) {
        if (i == oldRpath.size() || oldRpath[i] == ':') {
            if (!current.empty()) {
                std::string transformed = transformRpathEntry(current);
                if (!newRpath.empty()) {
                    newRpath += ':';
                }
                newRpath += transformed;
            }
            current.clear();
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
    std::span<const std::byte> content,
    [[maybe_unused]] bool executable)
{
    // Convert span to vector for patchelf (needs mutable shared_ptr)
    auto fileContents = std::make_shared<std::vector<unsigned char>>(
        reinterpret_cast<const unsigned char*>(content.data()),
        reinterpret_cast<const unsigned char*>(content.data()) + content.size());

    try {
        ElfFileType elfFile(fileContents);

        // Get current interpreter
        std::string interp;
        try {
            interp = elfFile.getInterpreter();
        } catch (...) {
            // No interpreter (probably a shared library)
            interp.clear();
        }

        // Patch interpreter
        if (!interp.empty()) {
            std::string newInterp = interp;

            // IMPORTANT: glibc substitution MUST happen BEFORE hash mappings
            // because hash mappings would change the path and prevent matching

            // Replace old glibc interpreter with Android glibc
            if (!oldGlibcPath.empty() && newInterp.find(oldGlibcPath) != std::string::npos) {
                newInterp = replaceAll(newInterp, oldGlibcPath, glibcPath);
            }

            // Then apply hash mappings for inter-package references
            newInterp = applyHashMappingsToString(newInterp);

            // Add prefix to /nix/store interpreters (needed for kernel to find ld.so)
            if (newInterp.rfind("/nix/store/", 0) == 0) {
                newInterp = prefix + newInterp;
            }

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
        auto& uc = *elfFile.fileContents;
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(uc.data()),
            reinterpret_cast<const std::byte*>(uc.data()) + uc.size());
    } catch (const std::exception& e) {
        debug("  ELF patch failed: %s\n", e.what());
        // Return original content on error
        return std::vector<std::byte>(content.begin(), content.end());
    }
}

// Patch source file: shebang (if present) and string literals
// The langFile is the detected .lang file (e.g., "sh.lang", "python.lang")
static std::vector<std::byte> patchSource(std::span<const std::byte> content, const std::string& langFile)
{
    if (prefix.empty()) {
        return std::vector<std::byte>(content.begin(), content.end());
    }

    std::string str(reinterpret_cast<const char*>(content.data()), content.size());
    bool modified = false;
    size_t contentStart = 0;  // Where string-aware patching starts

    // === SHEBANG PATCHING ===
    // Only patch shebang if the file actually has one
    if (hasShebang(content)) {
        // Find end of first line (shebang)
        size_t shebangEnd = str.find('\n');
        if (shebangEnd == std::string::npos) shebangEnd = str.size();

        std::string shebang = str.substr(0, shebangEnd);

        // Check if shebang contains /nix/store
        if (shebang.find("/nix/store/") != std::string::npos) {
            // Replace old glibc paths (gcc-lib handled by hash mapping)
            std::string newShebang = shebang;
            if (!oldGlibcPath.empty()) {
                newShebang = replaceAll(newShebang, oldGlibcPath, glibcPath);
            }

            // Apply hash mappings for inter-package references
            newShebang = applyHashMappingsToString(newShebang);

            // Add prefix to remaining /nix/store paths
            // Look for /nix/store/ after #!
            size_t pos = 2; // Skip #!
            while ((pos = newShebang.find("/nix/store/", pos)) != std::string::npos) {
                // Check if this is already prefixed
                if (pos < prefix.length() ||
                    newShebang.substr(pos - prefix.length(), prefix.length()) != prefix) {
                    newShebang.insert(pos, prefix);
                    pos += prefix.length();
                }
                pos += 11; // Skip "/nix/store/"
            }

            if (newShebang != shebang) {
                debug("  shebang: %s -> %s\n", shebang.c_str(), newShebang.c_str());
                str.replace(0, shebangEnd, newShebang);
                // Adjust contentStart for the size difference
                contentStart = shebangEnd + (newShebang.length() - shebang.length());
                modified = true;
            } else {
                contentStart = shebangEnd;
            }
        } else {
            contentStart = shebangEnd;
        }
    }

    // === STRING-AWARE CONTENT PATCHING ===
    // Patch additional paths (like /nix/var/) only inside string literals
    // Uses source-highlight with the pre-detected language
    if (!addPrefixToPaths.empty() && !langFile.empty() && tokenizer) {
        // Get string regions using source-highlight with the detected language
        std::vector<StringRegion> stringRegions = tokenizer->getStringRegions(str, langFile);

        if (!stringRegions.empty()) {
            debug("  patching additional paths in %zu string regions\n", stringRegions.size());

            for (const auto& pattern : addPrefixToPaths) {
                size_t pos = contentStart;  // Skip shebang line if present
                while ((pos = str.find(pattern, pos)) != std::string::npos) {
                    // Only patch if inside a string literal
                    if (isInsideString(pos, stringRegions)) {
                        // Check not already prefixed
                        bool alreadyPrefixed = (pos >= prefix.length() &&
                            str.substr(pos - prefix.length(), prefix.length()) == prefix);

                        if (!alreadyPrefixed) {
                            debug("  patching %s at position %zu (inside string)\n",
                                  pattern.c_str(), pos);
                            str.insert(pos, prefix);

                            // Adjust all subsequent regions for the insertion
                            for (auto& region : stringRegions) {
                                if (region.start > pos) {
                                    region.start += prefix.length();
                                }
                                if (region.end > pos) {
                                    region.end += prefix.length();
                                }
                            }
                            pos += prefix.length();
                            modified = true;
                        }
                    }
                    pos += pattern.length();
                }
            }
        } else {
            debug("  no string regions found, skipping additional path patching\n");
        }
    }

    if (modified) {
        auto* p = reinterpret_cast<const std::byte*>(str.data());
        return std::vector<std::byte>(p, p + str.size());
    }
    return std::vector<std::byte>(content.begin(), content.end());
}

// Main content patcher
// The path parameter is the relative path within the NAR (e.g., "bin/bash", "share/nix/nix.sh")
static std::vector<std::byte> patchContent(
    std::span<const std::byte> content,
    bool executable,
    const std::string& path)
{
    std::vector<std::byte> result;

    // Extract filename from path for language detection
    std::string filename;
    size_t lastSlash = path.rfind('/');
    filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

    // ELF check MUST come first - ELF files are often large and extensionless
    if (isElf(content)) {
        debug("  patching ELF %s (%zu bytes)\n", path.c_str(), content.size());
        if (isElf32(content)) {
            result = patchElfContent<ElfFile<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Addr, Elf32_Off, Elf32_Dyn, Elf32_Sym, Elf32_Versym, Elf32_Verdef, Elf32_Verdaux, Elf32_Verneed, Elf32_Vernaux, Elf32_Rel, Elf32_Rela, 32>>(
                content, executable);
        } else {
            result = patchElfContent<ElfFile<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Addr, Elf64_Off, Elf64_Dyn, Elf64_Sym, Elf64_Versym, Elf64_Verdef, Elf64_Verdaux, Elf64_Verneed, Elf64_Vernaux, Elf64_Rel, Elf64_Rela, 64>>(
                content, executable);
        }
    } else if (shouldSkipByExtension(filename, content.size())) {
        // Fast path: skip non-patchable extensions OR large extensionless non-ELF files
        debug("  skipping %s (skip extension or large extensionless)\n", path.c_str());
        result = std::vector<std::byte>(content.begin(), content.end());
        applyHashMappings(result);
        return result;
    } else {
        // Fast path 2: extension-based language detection (O(1) hash lookup)
        std::string langFile = getLangFromExtension(filename);

        // Fallback: content-based detection for small extensionless files with shebang
        if (langFile.empty() && tokenizer && hasShebang(content)) {
            std::string contentStr(reinterpret_cast<const char*>(content.data()), content.size());
            langFile = tokenizer->detectLanguage(filename, contentStr);
        }

        // Only process if language is in our whitelist of patchable languages
        if (!langFile.empty() && PATCHABLE_LANG_FILES.count(langFile)) {
            debug("  patching source %s (%zu bytes, lang=%s)\n",
                  path.c_str(), content.size(), langFile.c_str());
            result = patchSource(content, langFile);
        } else {
            if (!langFile.empty()) {
                debug("  skipping %s (lang=%s not in whitelist)\n",
                      path.c_str(), langFile.c_str());
            }
            result = std::vector<std::byte>(content.begin(), content.end());
        }
    }

    // Apply hash mappings for inter-package references
    // This must be done AFTER ELF/script patching to not interfere with structure
    applyHashMappings(result);

    return result;
}

static void showHelp(const char* progName)
{
    std::cerr << "Usage: " << progName << " [OPTIONS]\n"
              << "\n"
              << "Patch NAR stream for Android compatibility.\n"
              << "Reads NAR from stdin, writes patched NAR to stdout.\n"
              << "\n"
              << "Options:\n"
              << "  --prefix PATH        Installation prefix (e.g., /data/.../usr)\n"
              << "  --glibc PATH         Android glibc store path\n"
              << "  --old-glibc PATH     Original glibc store path to replace\n"
              << "  --mappings FILE      Hash mappings file for inter-package refs\n"
              << "                       Format: OLD_PATH NEW_PATH (one per line)\n"
              << "  --self-mapping MAP   Self-reference mapping (format: \"OLD_PATH NEW_PATH\")\n"
              << "  --add-prefix-to PATH Path pattern to add prefix to in script strings\n"
              << "                       (e.g., /nix/var/). Can be specified multiple times.\n"
              << "  --source-highlight-data-dir DIR\n"
              << "                       Path to source-highlight data files (.lang files)\n"
              << "  --debug              Enable debug output\n"
              << "  --help               Show this help\n"
              << "\n"
              << "Thread count can be controlled via TBB_NUM_THREADS environment variable.\n";
}

int main(int argc, char** argv)
{
    static struct option longOptions[] = {
        {"prefix",                   required_argument, nullptr, 'p'},
        {"glibc",                    required_argument, nullptr, 'g'},
        {"old-glibc",                required_argument, nullptr, 'G'},
        {"mappings",                 required_argument, nullptr, 'm'},
        {"self-mapping",             required_argument, nullptr, 's'},
        {"add-prefix-to",            required_argument, nullptr, 'A'},
        {"source-highlight-data-dir", required_argument, nullptr, 'D'},
        {"debug",                    no_argument,       nullptr, 'd'},
        {"help",                     no_argument,       nullptr, 'h'},
        {nullptr,                    0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:g:G:m:s:A:D:dh", longOptions, nullptr)) != -1) {
        switch (opt) {
        case 'p':
            prefix = optarg;
            break;
        case 'g':
            glibcPath = optarg;
            break;
        case 'G':
            oldGlibcPath = optarg;
            break;
        case 'm':
            loadMappings(optarg);
            break;
        case 's': {
            // Parse "OLD_PATH NEW_PATH" format
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
        case 'D':
            sourceHighlightDataDir = optarg;
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

    if (prefix.empty()) {
        std::cerr << "Error: --prefix is required\n";
        showHelp(argv[0]);
        return 1;
    }

    // Initialize tokenizer if source-highlight data dir is set
    if (!sourceHighlightDataDir.empty()) {
        tokenizer = std::make_unique<Tokenizer>(sourceHighlightDataDir);
    }

    debug("patchnar: prefix=%s\n", prefix.c_str());
    debug("patchnar: glibc=%s\n", glibcPath.c_str());
    debug("patchnar: old-glibc=%s\n", oldGlibcPath.c_str());
    debug("patchnar: source-highlight-data-dir=%s\n", sourceHighlightDataDir.c_str());
    for (const auto& path : addPrefixToPaths) {
        debug("patchnar: add-prefix-to=%s\n", path.c_str());
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
