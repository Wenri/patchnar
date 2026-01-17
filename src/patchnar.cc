/*
 * patchnar - NAR stream patcher for Android compatibility
 *
 * Reads a NAR from stdin, patches ELF binaries, symlinks, and scripts,
 * and writes the modified NAR to stdout.
 */

#include "nar.h"
#include "elf.h"
#include "patchelf.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Configuration
static std::string prefix;
static std::string glibcPath;
static std::string gccLibPath;
static std::string oldGlibcPath;
static std::string oldGccLibPath;
static bool debugMode = false;

static void debug(const char* format, ...)
{
    if (debugMode) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}

// Check if content is an ELF file
static bool isElf(const std::vector<unsigned char>& content)
{
    if (content.size() < SELFMAG)
        return false;
    return memcmp(content.data(), ELFMAG, SELFMAG) == 0;
}

// Check if content is a script (starts with #!)
static bool isScript(const std::vector<unsigned char>& content)
{
    if (content.size() < 2)
        return false;
    return content[0] == '#' && content[1] == '!';
}

// Check if ELF is 32-bit
static bool isElf32(const std::vector<unsigned char>& content)
{
    if (content.size() < EI_CLASS + 1)
        return false;
    return content[EI_CLASS] == ELFCLASS32;
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

// Patch symlink target
static std::string patchSymlink(const std::string& target)
{
    // Add prefix to /nix/store paths
    if (target.rfind("/nix/store/", 0) == 0) {
        std::string newTarget = prefix + target;
        debug("  symlink: %s -> %s\n", target.c_str(), newTarget.c_str());
        return newTarget;
    }
    return target;
}

// Forward declarations for ELF patching (defined in patchelf.cc)
template<ElfFileParams>
class ElfFile;

// Patch ELF binary content
template<class ElfFileType>
static std::vector<unsigned char> patchElfContent(
    std::vector<unsigned char> content,
    [[maybe_unused]] bool executable)
{
    auto fileContents = std::make_shared<std::vector<unsigned char>>(std::move(content));

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

            // Replace old glibc interpreter
            if (!oldGlibcPath.empty() && interp.find(oldGlibcPath) != std::string::npos) {
                newInterp = replaceAll(interp, oldGlibcPath, glibcPath);
            }
            // Add prefix to other interpreters
            else if (interp.rfind("/nix/store/", 0) == 0) {
                newInterp = prefix + interp;
            }

            if (newInterp != interp) {
                debug("  interpreter: %s -> %s\n", interp.c_str(), newInterp.c_str());
                elfFile.setInterpreter(newInterp);
            }
        }

        // Patch RPATH/RUNPATH
        // First, read current rpath by printing it
        // For now, we'll use rpSet to set new rpath
        // TODO: Get current rpath and transform it
        std::string currentRpath;
        try {
            // This is a workaround - modifyRPath with rpPrint outputs to stdout
            // We need a way to get the current rpath programmatically
            // For now, we'll set a new rpath based on glibc/gcc-lib paths
            if (!glibcPath.empty() || !gccLibPath.empty()) {
                std::string newRpath;
                if (!glibcPath.empty()) {
                    newRpath = glibcPath + "/lib";
                }
                if (!gccLibPath.empty()) {
                    if (!newRpath.empty()) newRpath += ":";
                    newRpath += gccLibPath + "/lib";
                }
                // Don't set empty rpath
                if (!newRpath.empty()) {
                    elfFile.modifyRPath(ElfFileType::rpSet, {}, newRpath);
                }
            }
        } catch (...) {
            // No RPATH section - that's fine
        }

        elfFile.rewriteSections();

        return *elfFile.fileContents;
    } catch (const std::exception& e) {
        debug("  ELF patch failed: %s\n", e.what());
        // Return original content on error
        return *fileContents;
    }
}

// Patch script shebang
static std::vector<unsigned char> patchScript(std::vector<unsigned char> content)
{
    // Find end of first line
    size_t lineEnd = 0;
    while (lineEnd < content.size() && content[lineEnd] != '\n') {
        lineEnd++;
    }

    std::string shebang(content.begin(), content.begin() + lineEnd);

    // Check if shebang contains /nix/store
    if (shebang.find("/nix/store/") != std::string::npos) {
        // Replace old glibc paths
        std::string newShebang = shebang;
        if (!oldGlibcPath.empty()) {
            newShebang = replaceAll(newShebang, oldGlibcPath, glibcPath);
        }
        if (!oldGccLibPath.empty()) {
            newShebang = replaceAll(newShebang, oldGccLibPath, gccLibPath);
        }

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

            std::vector<unsigned char> result;
            result.reserve(content.size() + newShebang.size() - shebang.size());
            result.insert(result.end(), newShebang.begin(), newShebang.end());
            result.insert(result.end(), content.begin() + lineEnd, content.end());
            return result;
        }
    }

    return content;
}

// Main content patcher
static std::vector<unsigned char> patchContent(
    const std::vector<unsigned char>& content,
    bool executable)
{
    if (isElf(content)) {
        debug("  patching ELF (%zu bytes)\n", content.size());
        if (isElf32(content)) {
            return patchElfContent<ElfFile<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Addr, Elf32_Off, Elf32_Dyn, Elf32_Sym, Elf32_Versym, Elf32_Verdef, Elf32_Verdaux, Elf32_Verneed, Elf32_Vernaux, Elf32_Rel, Elf32_Rela, 32>>(
                std::vector<unsigned char>(content), executable);
        } else {
            return patchElfContent<ElfFile<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Addr, Elf64_Off, Elf64_Dyn, Elf64_Sym, Elf64_Versym, Elf64_Verdef, Elf64_Verdaux, Elf64_Verneed, Elf64_Vernaux, Elf64_Rel, Elf64_Rela, 64>>(
                std::vector<unsigned char>(content), executable);
        }
    } else if (isScript(content)) {
        debug("  patching script (%zu bytes)\n", content.size());
        return patchScript(std::vector<unsigned char>(content));
    }

    return content;
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
              << "  --gcc-lib PATH       Android gcc-lib store path\n"
              << "  --old-glibc PATH     Original glibc store path to replace\n"
              << "  --old-gcc-lib PATH   Original gcc-lib store path to replace\n"
              << "  --debug              Enable debug output\n"
              << "  --help               Show this help\n";
}

int main(int argc, char** argv)
{
    static struct option longOptions[] = {
        {"prefix",      required_argument, nullptr, 'p'},
        {"glibc",       required_argument, nullptr, 'g'},
        {"gcc-lib",     required_argument, nullptr, 'c'},
        {"old-glibc",   required_argument, nullptr, 'G'},
        {"old-gcc-lib", required_argument, nullptr, 'C'},
        {"debug",       no_argument,       nullptr, 'd'},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr,       0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:g:c:G:C:dh", longOptions, nullptr)) != -1) {
        switch (opt) {
        case 'p':
            prefix = optarg;
            break;
        case 'g':
            glibcPath = optarg;
            break;
        case 'c':
            gccLibPath = optarg;
            break;
        case 'G':
            oldGlibcPath = optarg;
            break;
        case 'C':
            oldGccLibPath = optarg;
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

    debug("patchnar: prefix=%s\n", prefix.c_str());
    debug("patchnar: glibc=%s\n", glibcPath.c_str());
    debug("patchnar: gcc-lib=%s\n", gccLibPath.c_str());
    debug("patchnar: old-glibc=%s\n", oldGlibcPath.c_str());
    debug("patchnar: old-gcc-lib=%s\n", oldGccLibPath.c_str());

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
