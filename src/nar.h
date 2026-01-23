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
#include <condition_variable>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace nar {

// ============================================================================
// Semaphore - Limits concurrent operations (C++17 compatible)
// ============================================================================

class Semaphore {
public:
    explicit Semaphore(unsigned int count) : count_(count) {}

    void acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++count_;
        cv_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    unsigned int count_;
};

// RAII guard for semaphore
class SemaphoreGuard {
public:
    explicit SemaphoreGuard(Semaphore& sem) : sem_(sem) { sem_.acquire(); }
    ~SemaphoreGuard() { sem_.release(); }
    SemaphoreGuard(const SemaphoreGuard&) = delete;
    SemaphoreGuard& operator=(const SemaphoreGuard&) = delete;
private:
    Semaphore& sem_;
};

// ============================================================================
// NAR Tree Structure - In-memory representation of NAR contents
// ============================================================================

// Forward declarations
struct NarRegular;
struct NarSymlink;
struct NarDirectory;

// Node variant - type-safe union of all node types
using NarNode = std::variant<NarRegular, NarSymlink, NarDirectory>;
using NarNodePtr = std::unique_ptr<NarNode>;

// Regular file node
struct NarRegular {
    std::vector<unsigned char> content;
    bool executable = false;
};

// Symlink node
struct NarSymlink {
    std::string target;
};

// Directory node - contains named entries in sorted order
struct NarDirectory {
    // Entries sorted by name (NAR requires lexicographic order)
    std::map<std::string, NarNodePtr> entries;
};

// ============================================================================
// Callback types
// ============================================================================

// ContentPatcher receives: content, executable flag, and relative path within NAR
using ContentPatcher = std::function<std::vector<unsigned char>(
    const std::vector<unsigned char>& content, bool executable, const std::string& path)>;

// SymlinkPatcher receives: target path, returns patched target
using SymlinkPatcher = std::function<std::string(const std::string& target)>;

// ============================================================================
// NAR Processor - Two-phase batch processing with std::execution::par
// ============================================================================

class NarProcessor {
public:
    NarProcessor(std::istream& in, std::ostream& out);

    // Set patchers
    void setContentPatcher(ContentPatcher patcher) { contentPatcher_ = std::move(patcher); }
    void setSymlinkPatcher(SymlinkPatcher patcher) { symlinkPatcher_ = std::move(patcher); }

    // Set max concurrent patches (0 = unlimited, uses hardware_concurrency)
    void setNumThreads(unsigned int n) { numThreads_ = n; }

    // Process entire NAR stream (parse -> patch -> write)
    void process();

    // Statistics
    struct Stats {
        size_t filesPatched = 0;
        size_t symlinksPatched = 0;
        size_t directoriesProcessed = 0;
        size_t totalBytes = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    // Patch task: pointer to file and its path
    struct PatchTask {
        NarRegular* file;
        std::string path;
    };

    // Phase 1: Parse NAR into tree
    NarNodePtr parseNode(const std::string& path);
    NarNodePtr parseRegular(const std::string& path);
    NarNodePtr parseSymlink();
    NarNodePtr parseDirectory(const std::string& path);

    // Phase 2: Collect tasks and patch in parallel
    void collectPatchTasks(NarNode& node, const std::string& path,
                           std::vector<PatchTask>& tasks);
    void patchAllFiles(std::vector<PatchTask>& tasks);

    // Phase 3: Write patched tree to output
    void writeNode(const NarNode& node);
    void writeRegular(const NarRegular& regular);
    void writeSymlink(const NarSymlink& symlink);
    void writeDirectory(const NarDirectory& directory);

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
    unsigned int numThreads_ = 0;
    Stats stats_;
};

} // namespace nar

#endif // NAR_H
