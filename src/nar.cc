/*
 * NAR (Nix ARchive) format implementation
 */

#include "nar.h"

#include <cstring>
#include <stdexcept>

namespace nar {

static const std::string NAR_MAGIC = "nix-archive-1";

NarProcessor::NarProcessor(std::istream& in, std::ostream& out)
    : in_(in), out_(out)
{
}

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
    // NAR uses little-endian
    return val;
}

std::string NarProcessor::readString()
{
    uint64_t len = readU64();
    std::string s(len, '\0');
    if (len > 0) {
        readExact(&s[0], len);
    }

    // Read padding to 8-byte boundary
    size_t pad = (8 - len % 8) % 8;
    if (pad > 0) {
        char padding[8];
        readExact(padding, pad);
    }

    return s;
}

void NarProcessor::expectString(const std::string& expected)
{
    std::string s = readString();
    if (s != expected) {
        throw std::runtime_error("Expected '" + expected + "', got '" + s + "'");
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
        static const char zeros[8] = {0};
        out_.write(zeros, pad);
    }
}

void NarProcessor::writeString(const std::vector<unsigned char>& s)
{
    writeU64(s.size());
    out_.write(reinterpret_cast<const char*>(s.data()), s.size());

    // Padding to 8-byte boundary
    size_t pad = (8 - s.size() % 8) % 8;
    if (pad > 0) {
        static const char zeros[8] = {0};
        out_.write(zeros, pad);
    }
}

void NarProcessor::process()
{
    // Header
    expectString(NAR_MAGIC);
    writeString(NAR_MAGIC);

    // Root node has empty path
    processNode("");

    out_.flush();
}

void NarProcessor::processNode(const std::string& path)
{
    expectString("(");
    writeString("(");

    expectString("type");
    writeString("type");

    std::string nodeType = readString();
    writeString(nodeType);

    if (nodeType == "regular") {
        processRegular(path);
    } else if (nodeType == "symlink") {
        processSymlink();
    } else if (nodeType == "directory") {
        processDirectory(path);
        // processDirectory already handled the closing ")"
        return;
    } else {
        throw std::runtime_error("Unknown node type: " + nodeType);
    }

    expectString(")");
    writeString(")");
}

void NarProcessor::processRegular(const std::string& path)
{
    std::string marker = readString();
    bool executable = false;

    if (marker == "executable") {
        executable = true;
        writeString("executable");

        expectString("");
        writeString("");

        expectString("contents");
        marker = "contents";
    }

    if (marker != "contents") {
        throw std::runtime_error("Expected 'executable' or 'contents', got '" + marker + "'");
    }

    // Read content
    uint64_t len = readU64();
    std::vector<unsigned char> content(len);
    if (len > 0) {
        readExact(content.data(), len);
    }

    // Read padding
    size_t pad = (8 - len % 8) % 8;
    if (pad > 0) {
        char padding[8];
        readExact(padding, pad);
    }

    // Patch content if patcher is set
    std::vector<unsigned char> patched;
    if (contentPatcher_) {
        if (numThreads_ > 0) {
            // Async patching - launch patch operation in background
            // Note: we capture by value to avoid lifetime issues
            auto futurePatched = std::async(std::launch::async,
                [patcher = contentPatcher_, content = std::move(content), executable, path]() {
                    return patcher(content, executable, path);
                });

            // Wait for result (NAR is sequential, must write in order)
            // Future improvement: batch multiple files and write results in order
            patched = futurePatched.get();
        } else {
            // Sequential patching
            patched = contentPatcher_(content, executable, path);
        }
    } else {
        patched = std::move(content);
    }

    // Write patched content
    writeString("contents");
    writeString(patched);
}

void NarProcessor::processSymlink()
{
    expectString("target");
    writeString("target");

    std::string target = readString();

    // Patch symlink target if patcher is set
    std::string patched;
    if (symlinkPatcher_) {
        patched = symlinkPatcher_(target);
    } else {
        patched = target;
    }

    writeString(patched);
}

void NarProcessor::processDirectory(const std::string& path)
{
    while (true) {
        std::string marker = readString();

        if (marker == ")") {
            // End of directory
            writeString(")");
            return;
        }

        if (marker != "entry") {
            throw std::runtime_error("Expected 'entry' or ')', got '" + marker + "'");
        }

        writeString("entry");

        expectString("(");
        writeString("(");

        expectString("name");
        writeString("name");

        std::string name = readString();
        writeString(name);

        expectString("node");
        writeString("node");

        // Build child path: parent/name (or just name for root entries)
        std::string childPath = path.empty() ? name : path + "/" + name;
        processNode(childPath);

        expectString(")");
        writeString(")");
    }
}

} // namespace nar
