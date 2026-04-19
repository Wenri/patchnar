// source_patcher.h - Shared source-highlight tokenization for string patching
//
// Provides a generic function to apply a CharTranslator to string literals
// in source files, using source-highlight for syntax-aware tokenization.

#pragma once

#include <string>
#include <srchilite/chartranslator.h>
#include <srchilite/langmap.h>

// Source-highlight data directory (compile-time constant from configure)
extern const std::string sourceHighlightDataDir;

// Language map for extension -> .lang file lookup (loaded once)
extern srchilite::LangMap langMap;

// Detect language from file content (shebang, emacs mode, xml, etc.)
// Returns .lang filename (e.g., "sh.lang", "python.lang") or empty string.
std::string detectLanguage(const std::string& content);

// Get lowercase file extension (e.g., ".html", ".js")
std::string getExtension(const std::string& filename);

// Maximum file size for content-based language detection (shebang, etc.)
static constexpr size_t MAX_CONTENT_DETECT_SIZE = 64 * 1024;  // 64KB

// Detect language from filename (extension) with content fallback.
// First tries langMap extension lookup, then falls back to detectLanguage().
// maxContentDetect limits content-based detection (0 = no content detection).
std::string detectLanguageFromFile(
    const std::string& filename,
    const std::string& content,
    size_t maxContentDetect = MAX_CONTENT_DETECT_SIZE);

// Patch source content: apply translator to string literals only.
// Uses source-highlight to tokenize the content according to langFile,
// then applies the translator's doPreformat() to "string" elements.
// Returns the patched content, or the original content on error.
std::string patchSourceStrings(
    const std::string& content,
    const std::string& langFile,
    srchilite::CharTranslator& translator);
