/*
 * NAR (Nix ARchive) format streaming implementation with C++23 coroutines
 *
 * Streaming architecture:
 * - Generator yields NarEvent items as parsed (no tree allocation)
 * - Batch processor collects files, patches in parallel with std::execution::par
 * - Writes output immediately after each batch
 *
 * Thread count is controlled by TBB via TBB_NUM_THREADS environment variable.
 */

#include "nar.h"

#include <algorithm>
#include <cstring>
#include <execution>
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

std::generator<NarEvent> NarProcessor::parse()
{
    expectString(NAR_MAGIC);

    for (auto&& event : parseNode("")) {
        co_yield std::move(event);
    }
}

std::generator<NarEvent> NarProcessor::parseNode(std::string path)
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
        for (auto&& event : parseDirectory(std::move(path))) {
            co_yield std::move(event);
        }
    } else {
        throw std::runtime_error("Unknown node type: " + nodeType);
    }
}

NarEvent NarProcessor::parseRegular(const std::string& path)
{
    NarEvent event{
        .type = NarEvent::Type::RegularFile,
        .path = path
    };

    std::string marker = readString();

    if (marker == "executable") {
        event.executable = true;
        expectString("");  // Empty executable marker value
        expectString("contents");
        event.content = readBytes();
    } else if (marker == "contents") {
        event.content = readBytes();
    } else {
        throw std::runtime_error("Expected 'executable' or 'contents', got '" + marker + "'");
    }

    stats_.filesPatched++;
    return event;
}

NarEvent NarProcessor::parseSymlink(const std::string& path)
{
    expectString("target");
    std::string target = readString();

    stats_.symlinksPatched++;
    return NarEvent{
        .type = NarEvent::Type::Symlink,
        .path = path,
        .target = std::move(target)
    };
}

std::generator<NarEvent> NarProcessor::parseDirectory(std::string path)
{
    co_yield NarEvent{.type = NarEvent::Type::DirectoryStart, .path = path};

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

        co_yield NarEvent{.type = NarEvent::Type::EntryStart, .name = name, .path = childPath};

        for (auto&& event : parseNode(childPath)) {
            co_yield std::move(event);
        }

        expectString(")");
        co_yield NarEvent{.type = NarEvent::Type::EntryEnd, .path = childPath};
    }

    stats_.directoriesProcessed++;
    co_yield NarEvent{.type = NarEvent::Type::DirectoryEnd, .path = path};
}

// ============================================================================
// Event Writer
// ============================================================================

void NarProcessor::writeEvent(const NarEvent& event)
{
    switch (event.type) {
        case NarEvent::Type::DirectoryStart:
            writeString("(");
            writeString("type");
            writeString("directory");
            break;

        case NarEvent::Type::DirectoryEnd:
            writeString(")");
            break;

        case NarEvent::Type::EntryStart:
            writeString("entry");
            writeString("(");
            writeString("name");
            writeString(event.name);
            writeString("node");
            break;

        case NarEvent::Type::EntryEnd:
            writeString(")");
            break;

        case NarEvent::Type::RegularFile:
            writeString("(");
            writeString("type");
            writeString("regular");
            if (event.executable) {
                writeString("executable");
                writeString("");
            }
            writeString("contents");
            writeBytes(event.content);
            writeString(")");
            break;

        case NarEvent::Type::Symlink:
            writeString("(");
            writeString("type");
            writeString("symlink");
            writeString("target");
            writeString(event.target);
            writeString(")");
            break;
    }
}

// ============================================================================
// Batch Processing
// ============================================================================

void NarProcessor::flushBatch()
{
    if (batch_.empty()) return;

    // Patch all items in parallel
    std::for_each(std::execution::par, batch_.begin(), batch_.end(),
        [this](NarEvent& event) {
            if (event.type == NarEvent::Type::RegularFile && contentPatcher_) {
                event.content = contentPatcher_(event.content, event.executable, event.path);
            } else if (event.type == NarEvent::Type::Symlink && symlinkPatcher_) {
                event.target = symlinkPatcher_(event.target);
            }
        });

    // Write in order (already sorted from input)
    for (const auto& event : batch_) {
        writeEvent(event);
    }
    batch_.clear();
}

// ============================================================================
// Main Processing Pipeline
// ============================================================================

void NarProcessor::process()
{
    writeString(NAR_MAGIC);

    for (NarEvent event : parse()) {
        switch (event.type) {
            case NarEvent::Type::DirectoryStart:
            case NarEvent::Type::DirectoryEnd:
            case NarEvent::Type::EntryStart:
                writeEvent(event);
                break;

            case NarEvent::Type::RegularFile:
            case NarEvent::Type::Symlink:
                batch_.push_back(std::move(event));
                break;

            case NarEvent::Type::EntryEnd:
                flushBatch();  // Patch and write before closing entry
                writeEvent(event);
                break;
        }
    }

    out_.flush();
}

} // namespace nar
