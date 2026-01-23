/*
 * NAR (Nix ARchive) format streaming processor with C++23 coroutines
 *
 * NAR format:
 * - All strings are length-prefixed (64-bit LE) and padded to 8-byte boundary
 * - Header: "nix-archive-1"
 * - Node: "(" type {regular|symlink|directory} ... ")"
 *
 * Processing architecture:
 * - Generator yields NarEvent items as parsed (no tree in memory)
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
// NarEvent - What the generator yields
// ============================================================================

struct NarEvent {
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
};

// ============================================================================
// NarProcessor - Streaming processor with parallel batch patching
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

private:
    // Generator-based parsing
    std::generator<NarEvent> parse();
    std::generator<NarEvent> parseNode(std::string path);
    std::generator<NarEvent> parseDirectory(std::string path);
    NarEvent parseRegular(const std::string& path);
    NarEvent parseSymlink(const std::string& path);

    // Streaming write
    void writeEvent(const NarEvent& event);

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
    std::vector<NarEvent> batch_;
};

} // namespace nar

#endif // NAR_H
