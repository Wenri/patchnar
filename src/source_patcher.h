// source_patcher.h - Shared source-highlight tokenization for string patching
//
// Provides a generic function to apply a CharTranslator to string literals
// in source files, using source-highlight for syntax-aware tokenization.

#pragma once

#include <string>
#include <srchilite/chartranslator.h>

// Source-highlight data directory (compile-time constant from configure)
extern const std::string sourceHighlightDataDir;

// Patch source content: apply translator to string literals only.
// Uses source-highlight to tokenize the content according to langFile,
// then applies the translator's doPreformat() to "string" elements.
// Returns the patched content, or the original content on error.
std::string patchSourceStrings(
    const std::string& content,
    const std::string& langFile,
    srchilite::CharTranslator& translator);
