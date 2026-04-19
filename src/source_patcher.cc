// source_patcher.cc - Shared source-highlight tokenization for string patching
//
// Extracted from patchnar.cc to be shared with bun_graph.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "source_patcher.h"

#include <memory>
#include <sstream>
#include <stdexcept>

#include <srchilite/sourcehighlighter.h>
#include <srchilite/formattermanager.h>
#include <srchilite/formatter.h>
#include <srchilite/formatterparams.h>
#include <srchilite/langdefmanager.h>
#include <srchilite/regexrulefactory.h>
#include <srchilite/textstyleformatter.h>
#include <srchilite/bufferedoutput.h>
#include <srchilite/languageinfer.h>

#include <boost/regex.hpp>
#include <unordered_map>

const std::string sourceHighlightDataDir = SOURCE_HIGHLIGHT_DATA_DIR;

// Language map for extension -> .lang file lookup (loaded once at startup)
srchilite::LangMap langMap(sourceHighlightDataDir, "lang.map");

// Language inferrer for content-based detection (shebang, emacs mode, etc.)
static srchilite::LanguageInfer languageInfer;

// Detect language from content (shebang, emacs mode, xml, etc.)
// Returns .lang filename (e.g., "sh.lang", "python.lang") or empty string
std::string detectLanguage(const std::string& content)
{
    // Normalize Nix store paths in shebang for inference
    // #!/nix/store/xxx-perl-5.42.0/bin/perl → #!/bin/perl
    // This fixes LanguageInfer extracting hash instead of interpreter name
    static const boost::regex nixShebangRegex(
        R"(^(#!\s*)/nix/store/[a-z0-9]+-[^/]+(/bin/\S+))");
    std::string normalized = boost::regex_replace(content, nixShebangRegex, "$1$2",
        boost::regex_constants::format_first_only);

    // Content-based detection (shebang, emacs mode, xml, etc.)
    std::istringstream contentStream(normalized);
    std::string inferredLang = languageInfer.infer(contentStream);

    if (!inferredLang.empty()) {
        // Normalize common interpreter variants not in lang.map
        // e.g., python3 → python, perl5.42.0 → perl
        static const std::unordered_map<std::string, std::string> langAliases = {
            {"python3", "python"},
            {"python2", "python"},
        };
        std::string lookupLang = inferredLang;
        auto aliasIt = langAliases.find(inferredLang);
        if (aliasIt != langAliases.end()) {
            lookupLang = aliasIt->second;
        }

        std::string langFile = langMap.getMappedFileName(lookupLang);
        if (!langFile.empty()) {
            return langFile;
        }
    }

    return "";
}

std::string patchSourceStrings(
    const std::string& content,
    const std::string& langFile,
    srchilite::CharTranslator& translator)
{
    try {
        srchilite::RegexRuleFactory ruleFactory;
        srchilite::LangDefManager langDefManager(&ruleFactory);
        srchilite::SourceHighlighter highlighter(
            langDefManager.getHighlightState(sourceHighlightDataDir, langFile));
        highlighter.setOptimize(false);

        // Output collection
        std::ostringstream outputStream;
        srchilite::BufferedOutput bufferedOutput(outputStream);

        // Identity formatter for non-string elements (outputs text as-is)
        auto identityFormatter = std::make_unique<srchilite::TextStyleFormatter>(
            "$text", &bufferedOutput);

        // String formatter with caller-supplied path translation
        auto stringFormatter = std::make_unique<srchilite::TextStyleFormatter>(
            "$text", &bufferedOutput);
        stringFormatter->setPreFormatter(&translator);

        // Comment formatter with path translation (handles shebangs)
        auto commentFormatter = std::make_unique<srchilite::TextStyleFormatter>(
            "$text", &bufferedOutput);
        commentFormatter->setPreFormatter(&translator);

        // Register formatters
        srchilite::FormatterManager formatterManager(std::move(identityFormatter));
        formatterManager.addFormatter("string", std::move(stringFormatter));
        formatterManager.addFormatter("comment", std::move(commentFormatter));
        highlighter.setFormatterManager(&formatterManager);

        // Process entire content at once
        highlighter.highlightParagraph(content);

        return outputStream.str();

    } catch (const std::exception& e) {
        fprintf(stderr, "  source-highlight patching failed (%s): %s\n",
                langFile.c_str(), e.what());
        return content;
    }
}
