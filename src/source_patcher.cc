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

const std::string sourceHighlightDataDir = SOURCE_HIGHLIGHT_DATA_DIR;

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
