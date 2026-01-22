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
 * 2. Patch: Apply patches to all files in parallel
 * 3. Write: Serialize tree back to NAR format
 */

#ifndef NAR_H
#define NAR_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace nar {

// ============================================================================
// Thread Pool - RAII-managed pool for parallel task execution
// ============================================================================

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads);
    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit a task and get a future for its result
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    // Get number of threads
    size_t size() const { return workers_.size(); }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_{false};
};

// Template implementation must be in header
template<typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
{
    using ReturnType = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<ReturnType> result = task->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            throw std::runtime_error("ThreadPool: cannot submit to stopped pool");
        }
        tasks_.emplace([task]() { (*task)(); });
    }
    condition_.notify_one();
    return result;
}

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

    // For parallel patching - future holds the patched result
    mutable std::future<std::vector<unsigned char>> patchedContent;
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
// NAR Processor - Two-phase batch processing
// ============================================================================

class NarProcessor {
public:
    NarProcessor(std::istream& in, std::ostream& out);

    // Set patchers
    void setContentPatcher(ContentPatcher patcher) { contentPatcher_ = std::move(patcher); }
    void setSymlinkPatcher(SymlinkPatcher patcher) { symlinkPatcher_ = std::move(patcher); }

    // Set number of worker threads (0 or 1 = sequential processing)
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
    // Phase 1: Parse NAR into tree
    NarNodePtr parseNode(const std::string& path);
    NarNodePtr parseRegular(const std::string& path);
    NarNodePtr parseSymlink();
    NarNodePtr parseDirectory(const std::string& path);

    // Phase 2: Patch all files in parallel
    void patchTree(NarNode& node, const std::string& path);
    void patchTreeParallel(NarNode& node, const std::string& path, ThreadPool& pool);
    void collectPatchTasks(NarNode& node, const std::string& path,
                           std::vector<std::tuple<NarRegular*, std::string>>& tasks);

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
