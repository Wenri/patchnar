/*
 * NAR (Nix ARchive) format implementation with parallel batch processing
 *
 * Two-phase processing architecture:
 * 1. Parse entire NAR into in-memory tree (NarNode)
 * 2. Patch all regular files in parallel using thread pool
 * 3. Serialize patched tree back to NAR format
 *
 * This approach maximizes parallelism while preserving NAR's required
 * sequential output format.
 */

#include "nar.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace nar {

// ============================================================================
// Constants
// ============================================================================

static constexpr const char* NAR_MAGIC = "nix-archive-1";

// ============================================================================
// ThreadPool Implementation
// ============================================================================

ThreadPool::ThreadPool(size_t numThreads)
{
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    condition_.wait(lock, [this] {
                        return stop_ || !tasks_.empty();
                    });

                    if (stop_ && tasks_.empty()) {
                        return;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool()
{
    stop_ = true;
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

// ============================================================================
// NarProcessor - Constructor
// ============================================================================

NarProcessor::NarProcessor(std::istream& in, std::ostream& out)
    : in_(in), out_(out)
{
}

// ============================================================================
// Low-level I/O Operations
// ============================================================================

void NarProcessor::readExact(void* buf, size_t n)
{
    in_.read(static_cast<char*>(buf), n);
    if (static_cast<size_t>(in_.gcount()) != n) {
        throw std::runtime_error("Unexpected EOF reading NAR");
    }
}

uint64_t NarProcessor::readU64()
{
    uint64_t val;
    readExact(&val, sizeof(val));
    return val;  // NAR uses little-endian (native on x86/ARM)
}

std::string NarProcessor::readString()
{
    uint64_t len = readU64();
    std::string s(len, '\0');
    if (len > 0) {
        readExact(s.data(), len);
    }

    // Skip padding to 8-byte boundary
    size_t pad = (8 - len % 8) % 8;
    if (pad > 0) {
        char padding[8];
        readExact(padding, pad);
    }

    return s;
}

std::vector<unsigned char> NarProcessor::readBytes()
{
    uint64_t len = readU64();
    std::vector<unsigned char> data(len);
    if (len > 0) {
        readExact(data.data(), len);
    }

    // Skip padding to 8-byte boundary
    size_t pad = (8 - len % 8) % 8;
    if (pad > 0) {
        char padding[8];
        readExact(padding, pad);
    }

    stats_.totalBytes += len;
    return data;
}

void NarProcessor::expectString(const std::string& expected)
{
    std::string s = readString();
    if (s != expected) {
        throw std::runtime_error("NAR parse error: expected '" + expected + "', got '" + s + "'");
    }
}

void NarProcessor::writeU64(uint64_t n)
{
    out_.write(reinterpret_cast<const char*>(&n), sizeof(n));
}

void NarProcessor::writeString(const std::string& s)
{
    writeU64(s.size());
    out_.write(s.data(), s.size());

    // Padding to 8-byte boundary
    size_t pad = (8 - s.size() % 8) % 8;
    if (pad > 0) {
        static constexpr char zeros[8] = {0};
        out_.write(zeros, pad);
    }
}

void NarProcessor::writeBytes(const std::vector<unsigned char>& data)
{
    writeU64(data.size());
    out_.write(reinterpret_cast<const char*>(data.data()), data.size());

    // Padding to 8-byte boundary
    size_t pad = (8 - data.size() % 8) % 8;
    if (pad > 0) {
        static constexpr char zeros[8] = {0};
        out_.write(zeros, pad);
    }
}

// ============================================================================
// Phase 1: Parse NAR into Tree
// ============================================================================

NarNodePtr NarProcessor::parseNode(const std::string& path)
{
    expectString("(");
    expectString("type");

    std::string nodeType = readString();

    NarNodePtr node;
    if (nodeType == "regular") {
        node = parseRegular(path);
        expectString(")");
    } else if (nodeType == "symlink") {
        node = parseSymlink();
        expectString(")");
    } else if (nodeType == "directory") {
        // parseDirectory handles its own closing ")" since it reads
        // entries in a loop until it sees the closing paren
        node = parseDirectory(path);
    } else {
        throw std::runtime_error("Unknown node type: " + nodeType);
    }

    return node;
}

NarNodePtr NarProcessor::parseRegular(const std::string& /*path*/)
{
    auto regular = std::make_unique<NarNode>(NarRegular{});
    auto& reg = std::get<NarRegular>(*regular);

    std::string marker = readString();

    if (marker == "executable") {
        reg.executable = true;
        expectString("");  // Empty executable marker value
        expectString("contents");
        reg.content = readBytes();
    } else if (marker == "contents") {
        reg.content = readBytes();
    } else {
        throw std::runtime_error("Expected 'executable' or 'contents', got '" + marker + "'");
    }

    stats_.filesPatched++;
    return regular;
}

NarNodePtr NarProcessor::parseSymlink()
{
    expectString("target");
    std::string target = readString();

    stats_.symlinksPatched++;
    return std::make_unique<NarNode>(NarSymlink{std::move(target)});
}

NarNodePtr NarProcessor::parseDirectory(const std::string& path)
{
    auto directory = std::make_unique<NarNode>(NarDirectory{});
    auto& dir = std::get<NarDirectory>(*directory);

    while (true) {
        std::string marker = readString();

        if (marker == ")") {
            // Put back the closing paren for parseNode to consume
            // Actually, we need to handle this differently since parseNode expects ")"
            // Let's restructure: we'll handle ")" here and not in parseNode for directories
            break;
        }

        if (marker != "entry") {
            throw std::runtime_error("Expected 'entry' or ')', got '" + marker + "'");
        }

        expectString("(");
        expectString("name");
        std::string name = readString();
        expectString("node");

        std::string childPath = path.empty() ? name : path + "/" + name;
        dir.entries[name] = parseNode(childPath);

        expectString(")");
    }

    stats_.directoriesProcessed++;
    return directory;
}

// ============================================================================
// Phase 2: Patch Tree (Sequential or Parallel)
// ============================================================================

void NarProcessor::patchTree(NarNode& node, const std::string& path)
{
    std::visit([this, &path](auto& n) {
        using T = std::decay_t<decltype(n)>;

        if constexpr (std::is_same_v<T, NarRegular>) {
            if (contentPatcher_) {
                n.content = contentPatcher_(n.content, n.executable, path);
            }
        } else if constexpr (std::is_same_v<T, NarSymlink>) {
            if (symlinkPatcher_) {
                n.target = symlinkPatcher_(n.target);
            }
        } else if constexpr (std::is_same_v<T, NarDirectory>) {
            for (auto& [name, child] : n.entries) {
                std::string childPath = path.empty() ? name : path + "/" + name;
                patchTree(*child, childPath);
            }
        }
    }, node);
}

void NarProcessor::collectPatchTasks(
    NarNode& node,
    const std::string& path,
    std::vector<std::tuple<NarRegular*, std::string>>& tasks)
{
    std::visit([this, &path, &tasks](auto& n) {
        using T = std::decay_t<decltype(n)>;

        if constexpr (std::is_same_v<T, NarRegular>) {
            tasks.emplace_back(&n, path);
        } else if constexpr (std::is_same_v<T, NarSymlink>) {
            // Symlinks are patched synchronously (fast operation)
            if (symlinkPatcher_) {
                n.target = symlinkPatcher_(n.target);
            }
        } else if constexpr (std::is_same_v<T, NarDirectory>) {
            for (auto& [name, child] : n.entries) {
                std::string childPath = path.empty() ? name : path + "/" + name;
                collectPatchTasks(*child, childPath, tasks);
            }
        }
    }, node);
}

void NarProcessor::patchTreeParallel(NarNode& node, const std::string& path, ThreadPool& pool)
{
    // Collect all files that need patching
    std::vector<std::tuple<NarRegular*, std::string>> tasks;
    collectPatchTasks(node, path, tasks);

    if (tasks.empty() || !contentPatcher_) {
        return;
    }

    // Submit all patching tasks to thread pool
    std::vector<std::future<std::vector<unsigned char>>> futures;
    futures.reserve(tasks.size());

    for (auto& [regular, filePath] : tasks) {
        // Capture by value for thread safety
        auto content = regular->content;  // Copy for thread safety
        bool executable = regular->executable;
        auto patcher = contentPatcher_;

        futures.push_back(pool.submit([patcher, content = std::move(content), executable, filePath]() {
            return patcher(content, executable, filePath);
        }));
    }

    // Collect results in order
    for (size_t i = 0; i < tasks.size(); ++i) {
        auto& [regular, filePath] = tasks[i];
        regular->content = futures[i].get();
    }
}

// ============================================================================
// Phase 3: Write Patched Tree to Output
// ============================================================================

void NarProcessor::writeNode(const NarNode& node)
{
    writeString("(");
    writeString("type");

    std::visit([this](const auto& n) {
        using T = std::decay_t<decltype(n)>;

        if constexpr (std::is_same_v<T, NarRegular>) {
            writeString("regular");
            writeRegular(n);
        } else if constexpr (std::is_same_v<T, NarSymlink>) {
            writeString("symlink");
            writeSymlink(n);
        } else if constexpr (std::is_same_v<T, NarDirectory>) {
            writeString("directory");
            writeDirectory(n);
        }
    }, node);

    writeString(")");
}

void NarProcessor::writeRegular(const NarRegular& regular)
{
    if (regular.executable) {
        writeString("executable");
        writeString("");
    }
    writeString("contents");
    writeBytes(regular.content);
}

void NarProcessor::writeSymlink(const NarSymlink& symlink)
{
    writeString("target");
    writeString(symlink.target);
}

void NarProcessor::writeDirectory(const NarDirectory& directory)
{
    // NAR requires entries in lexicographic order - std::map already provides this
    for (const auto& [name, child] : directory.entries) {
        writeString("entry");
        writeString("(");
        writeString("name");
        writeString(name);
        writeString("node");
        writeNode(*child);
        writeString(")");
    }
}

// ============================================================================
// Main Processing Pipeline
// ============================================================================

void NarProcessor::process()
{
    // === Phase 1: Parse ===
    expectString(NAR_MAGIC);
    NarNodePtr root = parseNode("");

    // === Phase 2: Patch ===
    if (numThreads_ > 1) {
        // Parallel patching with thread pool
        ThreadPool pool(numThreads_);
        patchTreeParallel(*root, "", pool);
    } else {
        // Sequential patching
        patchTree(*root, "");
    }

    // === Phase 3: Write ===
    writeString(NAR_MAGIC);
    writeNode(*root);
    out_.flush();
}

} // namespace nar
