/*
 * NAR (Nix ARchive) format implementation with parallel batch processing
 *
 * Three-phase processing architecture:
 * 1. Parse entire NAR into in-memory tree (NarNode)
 * 2. Patch all regular files in parallel using std::execution::par
 * 3. Serialize patched tree back to NAR format
 *
 * Thread count is controlled by TBB via TBB_NUM_THREADS environment variable.
 */

#include "nar.h"

#include <algorithm>
#include <cstring>
#include <execution>
#include <functional>
#include <stdexcept>

namespace nar {

// ============================================================================
// Constants
// ============================================================================

static constexpr const char* NAR_MAGIC = "nix-archive-1";

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

std::unique_ptr<NarNode> NarProcessor::parseNode(const std::string& path)
{
    expectString("(");
    expectString("type");

    std::string nodeType = readString();

    std::unique_ptr<NarNode> node;
    if (nodeType == "regular") {
        node = parseRegular(path);
        expectString(")");
    } else if (nodeType == "symlink") {
        node = parseSymlink();
        expectString(")");
    } else if (nodeType == "directory") {
        // parseDirectory handles its own closing ")"
        node = parseDirectory(path);
    } else {
        throw std::runtime_error("Unknown node type: " + nodeType);
    }

    return node;
}

std::unique_ptr<NarNode> NarProcessor::parseRegular(const std::string& /*path*/)
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

std::unique_ptr<NarNode> NarProcessor::parseSymlink()
{
    expectString("target");
    std::string target = readString();

    stats_.symlinksPatched++;
    return std::make_unique<NarNode>(NarSymlink{std::move(target)});
}

std::unique_ptr<NarNode> NarProcessor::parseDirectory(const std::string& path)
{
    auto directory = std::make_unique<NarNode>(NarDirectory{});
    auto& dir = std::get<NarDirectory>(*directory);

    while (true) {
        std::string marker = readString();

        if (marker == ")") {
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
// Phase 2: Collect Tasks and Patch in Parallel
// ============================================================================

void NarProcessor::collectPatchTasks(
    NarNode& node,
    const std::string& path,
    std::vector<PatchTask>& tasks)
{
    std::visit([this, &path, &tasks](auto& n) {
        using T = std::decay_t<decltype(n)>;

        if constexpr (std::is_same_v<T, NarRegular>) {
            tasks.push_back({n, path, contentPatcher_});
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

void NarProcessor::patchAllFiles(std::vector<PatchTask>& tasks)
{
    if (tasks.empty() || !contentPatcher_) {
        return;
    }

    // Patch all files in parallel
    // Thread count controlled by TBB_NUM_THREADS environment variable
    std::for_each(std::execution::par, tasks.begin(), tasks.end(),
        std::mem_fn(&PatchTask::operator()));
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
    std::unique_ptr<NarNode> root = parseNode("");

    // === Phase 2: Collect and Patch ===
    std::vector<PatchTask> tasks;
    collectPatchTasks(*root, "", tasks);
    patchAllFiles(tasks);

    // === Phase 3: Write ===
    writeString(NAR_MAGIC);
    writeNode(*root);
    out_.flush();
}

} // namespace nar
