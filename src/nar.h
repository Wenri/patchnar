/*
 * NAR (Nix ARchive) format reader/writer
 *
 * NAR format:
 * - All strings are length-prefixed (64-bit LE) and padded to 8-byte boundary
 * - Header: "nix-archive-1"
 * - Node: "(" type {regular|symlink|directory} ... ")"
 */

#ifndef NAR_H
#define NAR_H

#include <cstdint>
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace nar {

// NAR entry types
enum class EntryType {
    Regular,
    Symlink,
    Directory
};

// Callback types for streaming processing
using ContentPatcher = std::function<std::vector<unsigned char>(
    const std::vector<unsigned char>& content, bool executable)>;
using SymlinkPatcher = std::function<std::string(const std::string& target)>;

// NAR processor - reads NAR, applies patches, writes NAR
class NarProcessor {
public:
    NarProcessor(std::istream& in, std::ostream& out);

    // Set patchers
    void setContentPatcher(ContentPatcher patcher) { contentPatcher_ = std::move(patcher); }
    void setSymlinkPatcher(SymlinkPatcher patcher) { symlinkPatcher_ = std::move(patcher); }

    // Process entire NAR stream
    void process();

private:
    // Low-level I/O
    void readExact(void* buf, size_t n);
    uint64_t readU64();
    std::string readString();
    void expectString(const std::string& expected);

    void writeU64(uint64_t n);
    void writeString(const std::string& s);
    void writeString(const std::vector<unsigned char>& s);

    // NAR structure processing
    void processNode();
    void processRegular();
    void processSymlink();
    void processDirectory();

    std::istream& in_;
    std::ostream& out_;
    ContentPatcher contentPatcher_;
    SymlinkPatcher symlinkPatcher_;
};

} // namespace nar

#endif // NAR_H
