/*
 * NAR (Nix ARchive) format streaming processor with C++23 coroutines
 *
 * NAR format:
 * - All strings are length-prefixed (64-bit LE) and padded to 8-byte boundary
 * - Header: "nix-archive-1"
 * - Node: "(" type {regular|symlink|directory} ... ")"
 *
 * Processing architecture:
 * - Generator yields NarNode items as parsed (no tree in memory)
 * - Batch processor collects files, patches in parallel, writes immediately
 * - Memory: O(batch_size * max_file) instead of O(total_NAR_size)
 */

#ifndef NAR_H
#define NAR_H

#include <cstdint>
#include <functional>
#include <generator>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace nar {

// ============================================================================
// Patcher function types (shared between NarNode and NarProcessor)
// ============================================================================

using ContentPatcher = std::function<std::vector<unsigned char>(
    const std::vector<unsigned char>&, bool, const std::string&)>;
using SymlinkPatcher = std::function<std::string(const std::string&)>;

// ============================================================================
// NarNode - Self-patching node yielded by the generator
// ============================================================================

struct NarNode {
    enum class Type {
        DirectoryStart, DirectoryEnd,
        EntryStart, EntryEnd,
        RegularFile, Symlink
    };

    Type type;
    std::string name;                    // Entry name (for EntryStart)
    std::string path;                    // Full path
    std::vector<unsigned char> content;  // File content (for RegularFile)
    std::string target;                  // Symlink target (for Symlink)
    bool executable = false;             // For RegularFile

    // Patcher pointers (set by NarProcessor::prepareBatch before parallel execution)
    // Non-owning const pointers to NarProcessor's std::function members
    const ContentPatcher* contentPatcher_ = nullptr;
    const SymlinkPatcher* symlinkPatcher_ = nullptr;

    // Self-patching method (thread-safe, modifies only this instance)
    void patch() {
        if (type == Type::RegularFile && contentPatcher_ && *contentPatcher_) {
            content = (*contentPatcher_)(content, executable, path);
        } else if (type == Type::Symlink && symlinkPatcher_ && *symlinkPatcher_) {
            target = (*symlinkPatcher_)(target);
        }
    }
};

// ============================================================================
// NarProcessor - Streaming processor with parallel batch patching
// ============================================================================

class NarProcessor {
public:
    NarProcessor(std::istream& in, std::ostream& out);

    void setContentPatcher(ContentPatcher patcher) { contentPatcher_ = std::move(patcher); }
    void setSymlinkPatcher(SymlinkPatcher patcher) { symlinkPatcher_ = std::move(patcher); }
    void process();

    // Iterator interface for parallel patching
    using iterator = std::vector<NarNode>::iterator;
    using const_iterator = std::vector<NarNode>::const_iterator;
    iterator begin() { return batch_.begin(); }
    iterator end() { return batch_.end(); }
    const_iterator begin() const { return batch_.begin(); }
    const_iterator end() const { return batch_.end(); }

    // Prepare batch for patching (sets patcher pointers on all nodes)
    void prepareBatch();

    // Write all nodes in batch to output
    void write();

    struct Stats {
        size_t filesPatched = 0;
        size_t symlinksPatched = 0;
        size_t directoriesProcessed = 0;
        size_t totalBytes = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    // Generator-based parsing
    std::generator<NarNode> parse();
    std::generator<NarNode> parseNode(std::string path);
    std::generator<NarNode> parseDirectory(std::string path);
    NarNode parseRegular(const std::string& path);
    NarNode parseSymlink(const std::string& path);

    // Streaming write
    void writeNode(const NarNode& node);

    // Batch processing
    void flushBatch();

    // Low-level I/O
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
    std::vector<NarNode> batch_;
};

} // namespace nar

#endif // NAR_H
