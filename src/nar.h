/*
 * NAR (Nix ARchive) format streaming processor with TBB parallel_pipeline
 *
 * NAR format:
 * - All strings are length-prefixed (64-bit LE) and padded to 8-byte boundary
 * - Header: "nix-archive-1"
 * - Node: "(" type {regular|symlink|directory} ... ")"
 *
 * Processing architecture:
 * - TBB parallel_pipeline: parse (serial) → patch (parallel) → write (serial)
 * - Generator yields NarNode items as parsed (no tree in memory)
 * - Parallel patching with automatic backpressure (8 tokens in flight)
 * - Order preserved via serial_in_order filter modes
 * - Memory: O(8 × max_file) - bounded by token count
 */

#ifndef NAR_H
#define NAR_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <generator>
#include <istream>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <oneapi/tbb/parallel_pipeline.h>

namespace nar {

// ============================================================================
// Patcher function types (shared between NarNode and NarProcessor)
// ============================================================================

using ContentPatcher = std::function<std::vector<std::byte>(
    std::span<const std::byte>, bool, const std::string&)>;
using SymlinkPatcher = std::function<std::string(const std::string&)>;

// ============================================================================
// NarNode - Data node yielded by the generator
// ============================================================================

struct NarNode {
    enum class Type {
        Invalid = -1,  // Default value for uninitialized nodes
        DirectoryStart, DirectoryEnd,
        EntryStart, EntryEnd,
        RegularFile, Symlink
    };

    Type type = Type::Invalid;  // Initialize to Invalid to catch bugs
    std::string name;                    // Entry name (for EntryStart)
    std::string path;                    // Full path
    std::vector<std::byte> content;      // File content (for RegularFile)
    std::string target;                  // Symlink target (for Symlink)
    bool executable = false;             // For RegularFile
};

// ============================================================================
// NarProcessor - True streaming processor with inline patching
// ============================================================================

class NarProcessor {
public:
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

private:
    // Generator-based parsing
    std::generator<NarNode> parse();
    std::generator<NarNode> parseNode(std::string path);
    std::generator<NarNode> parseDirectory(std::string path);
    NarNode parseRegular(const std::string& path);
    NarNode parseSymlink(const std::string& path);

    // Streaming write
    void writeNode(const NarNode& node);

    // Low-level I/O
    void readExact(void* buf, size_t n);
    uint64_t readU64();
    std::string readString();
    std::vector<std::byte> readBytes();
    void expectString(const std::string& expected);
    void writeU64(uint64_t n);
    void writeString(const std::string& s);
    void writeBytes(std::span<const std::byte> data);

    std::istream& in_;
    std::ostream& out_;
    ContentPatcher contentPatcher_;
    SymlinkPatcher symlinkPatcher_;
    Stats stats_;
    std::generator<NarNode> parseGen_;
    std::ranges::iterator_t<std::generator<NarNode>> it_;
};

} // namespace nar

#endif // NAR_H
