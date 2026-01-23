/*
 * NAR (Nix ARchive) format reader/writer with parallel batch processing
 *
 * NAR format:
 * - All strings are length-prefixed (64-bit LE) and padded to 8-byte boundary
 * - Header: "nix-archive-1"
 * - Node: "(" type {regular|symlink|directory} ... ")"
 *
 * Processing phases:
 * 1. Parse: Read entire NAR into tree structure
 * 2. Patch: Apply patches to all files in parallel (std::execution::par)
 * 3. Write: Serialize tree back to NAR format
 */

#ifndef NAR_H
#define NAR_H

#include <cstdint>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace nar {

// ============================================================================
// NAR Tree Structure - In-memory representation of NAR contents
// ============================================================================

struct NarRegular {
    std::vector<unsigned char> content;
    bool executable = false;
};

struct NarSymlink {
    std::string target;
};

struct NarDirectory;  // Forward declaration for recursive type

using NarNode = std::variant<NarRegular, NarSymlink, NarDirectory>;

struct NarDirectory {
    std::map<std::string, std::unique_ptr<NarNode>> entries;
};

// ============================================================================
// NAR Processor - Two-phase batch processing with std::execution::par
// ============================================================================

class NarProcessor {
public:
    using ContentPatcher = std::function<std::vector<unsigned char>(
        const std::vector<unsigned char>&, bool, const std::string&)>;
    using SymlinkPatcher = std::function<std::string(const std::string&)>;

    NarProcessor(std::istream& in, std::ostream& out);

    void setContentPatcher(ContentPatcher patcher) { contentPatcher_ = std::move(patcher); }
    void setSymlinkPatcher(SymlinkPatcher patcher) { symlinkPatcher_ = std::move(patcher); }
    void process();

    struct Stats {
        size_t filesPatched = 0;
        size_t symlinksPatched = 0;
        size_t directoriesProcessed = 0;
        size_t totalBytes = 0;
    };
    const Stats& stats() const { return stats_; }

    struct PatchTask {
        NarRegular& file;
        const std::string path;
        const ContentPatcher& patcher;
        void operator()() {
            file.content = patcher(file.content, file.executable, path);
        }
    };

private:
    std::unique_ptr<NarNode> parseNode(const std::string& path);
    std::unique_ptr<NarNode> parseRegular(const std::string& path);
    std::unique_ptr<NarNode> parseSymlink();
    std::unique_ptr<NarNode> parseDirectory(const std::string& path);

    void collectPatchTasks(NarNode& node, const std::string& path, std::vector<PatchTask>& tasks);
    void patchAllFiles(std::vector<PatchTask>& tasks);

    void writeNode(const NarNode& node);
    void writeRegular(const NarRegular& regular);
    void writeSymlink(const NarSymlink& symlink);
    void writeDirectory(const NarDirectory& directory);

    void readExact(void* buf, size_t n);
    uint64_t readU64();
    std::string readString();
    std::vector<unsigned char> readBytes();
    void expectString(const std::string& expected);
    void writeU64(uint64_t n);
    void writeString(const std::string& s);
    void writeBytes(const std::vector<unsigned char>& data);

    std::istream& in_;
    std::ostream& out_;
    ContentPatcher contentPatcher_;
    SymlinkPatcher symlinkPatcher_;
    Stats stats_;
};

} // namespace nar

#endif // NAR_H
