/*
 * NAR (Nix ARchive) format streaming implementation with C++23 coroutines
 *
 * Streaming architecture:
 * - True streaming pipeline: parse() → patchedStream() → writeNode()
 * - Generator yields NarNode items as parsed (no tree allocation)
 * - Inline patching in patchedStream() - no batching needed
 * - Memory: O(max_file) - only one file in memory at a time
 */

#include "nar.h"

#include <cstring>
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
// Generator-based Parsing
// ============================================================================

std::generator<NarNode> NarProcessor::parse()
{
    expectString(NAR_MAGIC);

    for (auto&& node : parseNode("")) {
        co_yield std::move(node);
    }
}

std::generator<NarNode> NarProcessor::parseNode(std::string path)
{
    expectString("(");
    expectString("type");

    std::string nodeType = readString();

    if (nodeType == "regular") {
        co_yield parseRegular(path);
        expectString(")");
    } else if (nodeType == "symlink") {
        co_yield parseSymlink(path);
        expectString(")");
    } else if (nodeType == "directory") {
        for (auto&& node : parseDirectory(std::move(path))) {
            co_yield std::move(node);
        }
    } else {
        throw std::runtime_error("Unknown node type: " + nodeType);
    }
}

NarNode NarProcessor::parseRegular(const std::string& path)
{
    NarNode node{
        .type = NarNode::Type::RegularFile,
        .path = path
    };

    std::string marker = readString();

    if (marker == "executable") {
        node.executable = true;
        expectString("");  // Empty executable marker value
        expectString("contents");
        node.content = readBytes();
    } else if (marker == "contents") {
        node.content = readBytes();
    } else {
        throw std::runtime_error("Expected 'executable' or 'contents', got '" + marker + "'");
    }

    stats_.filesPatched++;
    return node;
}

NarNode NarProcessor::parseSymlink(const std::string& path)
{
    expectString("target");
    std::string target = readString();

    stats_.symlinksPatched++;
    return NarNode{
        .type = NarNode::Type::Symlink,
        .path = path,
        .target = std::move(target)
    };
}

std::generator<NarNode> NarProcessor::parseDirectory(std::string path)
{
    co_yield NarNode{.type = NarNode::Type::DirectoryStart, .path = path};

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

        co_yield NarNode{.type = NarNode::Type::EntryStart, .name = name, .path = childPath};

        for (auto&& node : parseNode(childPath)) {
            co_yield std::move(node);
        }

        expectString(")");
        co_yield NarNode{.type = NarNode::Type::EntryEnd, .path = childPath};
    }

    stats_.directoriesProcessed++;
    co_yield NarNode{.type = NarNode::Type::DirectoryEnd, .path = path};
}

// ============================================================================
// Node Writer
// ============================================================================

void NarProcessor::writeNode(const NarNode& node)
{
    switch (node.type) {
        case NarNode::Type::DirectoryStart:
            writeString("(");
            writeString("type");
            writeString("directory");
            break;

        case NarNode::Type::DirectoryEnd:
            writeString(")");
            break;

        case NarNode::Type::EntryStart:
            writeString("entry");
            writeString("(");
            writeString("name");
            writeString(node.name);
            writeString("node");
            break;

        case NarNode::Type::EntryEnd:
            writeString(")");
            break;

        case NarNode::Type::RegularFile:
            writeString("(");
            writeString("type");
            writeString("regular");
            if (node.executable) {
                writeString("executable");
                writeString("");
            }
            writeString("contents");
            writeBytes(node.content);
            writeString(")");
            break;

        case NarNode::Type::Symlink:
            writeString("(");
            writeString("type");
            writeString("symlink");
            writeString("target");
            writeString(node.target);
            writeString(")");
            break;
    }
}

// ============================================================================
// Streaming Pipeline: parse() → patchedStream() → writeNode()
// ============================================================================

std::generator<NarNode> NarProcessor::patchedStream()
{
    for (NarNode node : parse()) {
        if (node.type == NarNode::Type::RegularFile && contentPatcher_) {
            node.content = contentPatcher_(node.content, node.executable, node.path);
        } else if (node.type == NarNode::Type::Symlink && symlinkPatcher_) {
            node.target = symlinkPatcher_(node.target);
        }
        co_yield std::move(node);
    }
}

// ============================================================================
// Main Processing Pipeline
// ============================================================================

void NarProcessor::process()
{
    writeString(NAR_MAGIC);

    for (const auto& node : patchedStream()) {
        writeNode(node);
    }

    out_.flush();
}

} // namespace nar
