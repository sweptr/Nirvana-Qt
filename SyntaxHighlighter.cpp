
#include "SyntaxHighlighter.h"
#include "QJson4/QJsonArray.h"
#include "QJson4/QJsonDocument.h"
#include "QJson4/QJsonObject.h"
#include "QJson4/QJsonParseError.h"
#include "TextBuffer.h"
#include "X11Colors.h"
#include <QDomDocument>
#include <QFile>
#include <QMap>
#include <QMessageBox>
#include <QRegExp>
#include <QtDebug>
#include <QtGlobal>
#include <algorithm>
#include <climits>
#include <cstring>
#include <fstream>

namespace {

/* Pattern flags for modifying pattern matching behavior */
enum PatternFlags {
    PARSE_SUBPATS_FROM_START = 1,
    DEFER_PARSING = 2,
    COLOR_ONLY = 4,
};

const int PLAIN_LANGUAGE_MODE = -1;

const int MAX_TITLE_FORMAT_LEN  = 50;

/* How much re-parsing to do when an unfinished style is encountered */
const int PASS_2_REPARSE_CHUNK_SIZE = 1000;

/* Initial forward expansion of parsing region in incremental reparsing,
   when style changes propagate forward beyond the original modification.
   This distance is increased by a factor of two for each subsequent step. */
const int REPARSE_CHUNK_SIZE = 80;

/* Scanning context can be reduced (with big efficiency gains) if we
   know that patterns can't cross line boundaries, which is implied
   by a context requirement of 1 line and 0 characters */
#define CAN_CROSS_LINE_BOUNDARIES(contextRequirements)                                                                 \
    (contextRequirements->nLines != 1 || contextRequirements->nChars != 0)

/* Compare two styles where one of the styles may not yet have been processed
   with pass2 patterns */
#define EQUIVALENT_STYLE(style1, style2, firstPass2Style)                                                              \
    (style1 == style2 ||                                                                                               \
     (style1 == UNFINISHED_STYLE && (style2 == PLAIN_STYLE || (unsigned char)style2 >= firstPass2Style)) ||            \
     (style2 == UNFINISHED_STYLE && (style1 == PLAIN_STYLE || (unsigned char)style1 >= firstPass2Style)))


const char_type delimiters[] = _T(".,/\\`'!|@#%^&*()-=+{}[]\":;<>?~ \t\n");
}


SyntaxHighlighter::SyntaxHighlighter() {

    RegExp::SetREDefaultWordDelimiters(_T(".,/\\`'!|@#%^&*()-=+{}[]\":;<>?"));

    loadStyles(":/DefaultStyle.xml");

    auto mode = new languageModeRec;
    mode->defTipsFile = "";
	
#ifdef USE_WCHAR
	mode->delimiters = QString::fromWCharArray(delimiters, sizeof(delimiters) / sizeof(char_type));
#else
	mode->delimiters = QString::fromLatin1(delimiters, sizeof(delimiters) / sizeof(char_type));
#endif	
	
    mode->emTabDist = 4;
    mode->extensions << ".cc" << ".hh" << ".C" << ".H" <<  ".i" <<  ".cxx" <<  ".hxx" <<  ".cpp" <<  ".c++" <<  ".h" <<  ".hpp";
    mode->indentStyle = 0;
    mode->name = "C++";
    mode->recognitionExpr = "";
    mode->tabDist = 4;
    mode->wrapStyle = 0;
    LanguageModes.push_back(mode);

    loadLanguages(":/DefaultLanguages.json");


    /* Find the pattern set matching the window's current
       language mode, tell the user if it can't be done */
    bool warn = true;
    if (patternSet *patterns = findPatternsForWindow(warn)) {
#if 1
        highlightData_ = createHighlightData(patterns);
#else
        highlightData_ = nullptr;
#endif
    }
}

SyntaxHighlighter::~SyntaxHighlighter() {
}

TextBuffer *SyntaxHighlighter::styleBuffer() const {
    if (highlightData_) {
        return highlightData_->styleBuffer;
    }

    return nullptr;
}

void SyntaxHighlighter::bufferModified(const ModifyEvent *event) {
    const int nInserted = event->nInserted;
    const int nDeleted = event->nDeleted;
    const int pos = event->pos;

    if (!highlightData_) {
        return;
    }

    /* Restyling-only modifications (usually a primary or secondary  selection)
       don't require any processing, but clear out the style buffer selection
       so the widget doesn't think it has to keep redrawing the old area */
    if (nInserted == 0 && nDeleted == 0) {
        highlightData_->styleBuffer->BufUnselect();
        return;
    }

    /* First and foremost, the style buffer must track the text buffer
       accurately and correctly */
    if (nInserted > 0) {

        auto insStyle = new char_type[nInserted + 1];
        int i;
        for (i = 0; i < nInserted; i++) {
            insStyle[i] = UNFINISHED_STYLE;
        }
        insStyle[i] = '\0';
        // TODO(eteran): BUGCHECK: should this be nInserted? here, not nDeleted?
        highlightData_->styleBuffer->BufReplace(pos, pos + nDeleted, insStyle);
        delete[] insStyle;
    } else {
        highlightData_->styleBuffer->BufRemove(pos, pos + nDeleted);
    }

    /* Mark the changed region in the style buffer as requiring redraw.  This
       is not necessary for getting it redrawn, it will be redrawn anyhow by
       the text display callback, but it clears the previous selection and
       saves the modifyStyleBuf routine from unnecessary work in tracking
       changes that are already scheduled for redraw */
    highlightData_->styleBuffer->BufSelect(pos, pos + nInserted);

    /* Re-parse around the changed region */
    if (highlightData_->pass1Patterns) {
        incrementalReparse(highlightData_, event->buffer, pos, nInserted, delimiters);
    }
}

void SyntaxHighlighter::loadLanguages(const QString &filename) {

    QFile file(filename);
    if(file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QJsonParseError e;
        QJsonDocument d = QJsonDocument::fromJson(file.readAll(), &e);
        if(!d.isNull()) {
            auto pattern_set = new patternSet;
            pattern_set->charContext  = 0;
            pattern_set->languageMode = "C++";
            pattern_set->lineContext  = 1;

            QJsonArray arr = d.array();
            for(QJsonValue entry : arr) {
                QJsonObject obj = entry.toObject();

                highlightPattern pattern;

                if(obj.contains("name") && !obj["name"].isNull()) {
                    pattern.name = obj["name"].toString();
                }

                if(obj.contains("style") && !obj["style"].isNull()) {
                    pattern.style = obj["style"].toString();
                }

                if(obj.contains("defered")) {
                    pattern.flags = obj["defered"].toBool() ? DEFER_PARSING : 0;
                }

                if(obj.contains("start") && !obj["start"].isNull()) {
                    pattern.startRE = obj["start"].toString();
                }

                if(obj.contains("end") && !obj["end"].isNull()) {
                    pattern.endRE = obj["end"].toString();
                } else {
                    pattern.endRE = nullptr;
                }

                if(obj.contains("error") && !obj["error"].isNull()) {
                    pattern.errorRE = obj["error"].toString();
                } else {
                    pattern.errorRE = nullptr;
                }

                if(obj.contains("parent") && !obj["parent"].isNull()) {
                    pattern.subPatternOf = obj["parent"].toString();
                }

                pattern_set->patterns.push_back(pattern);
            }


            PatternSets.push_back(pattern_set);
        }
    }
}

void SyntaxHighlighter::loadStyles(const QString &filename) {
    QFile file(filename);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QDomDocument doc;
        doc.setContent(&file);

        QDomElement root = doc.documentElement();
        QDomNodeList styles = root.elementsByTagName("style");

        for (int i = 0; i < styles.size(); i++) {
            QDomElement e = styles.at(i).toElement();

            auto style = new highlightStyleRec;
            style->bgColor = "white";
            style->color = "black";
            style->font = 0;
            style->name = e.attribute("name");

            if (e.hasAttribute("foreground")) {
                style->color = e.attribute("foreground");
            }

            if (e.hasAttribute("background")) {
                style->bgColor = e.attribute("background");
            }

            if (e.hasAttribute("bold")) {
                style->bold = e.attribute("bold") == "true";
            }

            if (e.hasAttribute("italic")) {
                style->italic = e.attribute("italic") == "true";
            }


            HighlightStyles.push_back(style);
        }

        file.close();
    }
}

/*
** Re-parse the smallest region possible around a modification to buffer "buf"
** to gurantee that the promised context lines and characters have
** been presented to the patterns.  Changes the style buffer in "highlightData"
** with the parsing result.
*/
void SyntaxHighlighter::incrementalReparse(windowHighlightData *highlightData, TextBuffer *buf, int pos, int nInserted,
                                           const char_type *delimiters) {

    TextBuffer *const styleBuf            = highlightData_->styleBuffer;
    HighlightDataRecord *const pass1Patterns = highlightData->pass1Patterns;
    HighlightDataRecord *const pass2Patterns = highlightData->pass2Patterns;
    ReparseContext *const context         = &highlightData->contextRequirements;
    char_type *const parentStyles         = highlightData->parentStyles;

    /* Find the position "beginParse" at which to begin reparsing.  This is
       far enough back in the buffer such that the guranteed number of
       lines and characters of context are examined. */
    int beginParse = pos;
    int parseInStyle = findSafeParseRestartPos(buf, highlightData, &beginParse);

    /* Find the position "endParse" at which point it is safe to stop
       parsing, unless styles are getting changed beyond the last
       modification */
    int lastMod = pos + nInserted;
    int endParse = forwardOneContext(buf, context, lastMod);

    /*
    ** Parse the buffer from beginParse, until styles compare
    ** with originals for one full context distance.  Distance increases
    ** by powers of two until nothing changes from previous step.  If
    ** parsing ends before endParse, start again one level up in the
    ** pattern hierarchy
    */
    for (int nPasses = 0;; nPasses++) {

        /* Parse forward from beginParse to one context beyond the end of the last modification */
        HighlightDataRecord *startPattern = patternOfStyle(pass1Patterns, parseInStyle);
        /* If there is no pattern matching the style, it must be a pass-2
           style. It that case, it is (probably) safe to start parsing with
           the root pass-1 pattern again. Anyway, passing a nullptr-pointer to
           the parse routine would result in a crash; restarting with pass-1
           patterns is certainly preferable, even if there is a slight chance
           of a faulty coloring. */
        if (!startPattern) {
            startPattern = pass1Patterns;
        }
        int endAt = parseBufferRange(startPattern, pass2Patterns, buf, styleBuf, context, beginParse, endParse, delimiters);

        /* If parse completed at this level, move one style up in the
       hierarchy and start again from where the previous parse left off. */
        if (endAt < endParse) {
            beginParse = endAt;
            endParse = forwardOneContext(buf, context, qMax(endAt, qMax(lastModified(styleBuf), lastMod)));
            if (IS_PLAIN(parseInStyle)) {
                qDebug("internal error: incr. reparse fell short\n");
                return;
            }
            parseInStyle = parentStyleOf(parentStyles, parseInStyle);

            /* One context distance beyond last style changed means we're done */
        } else if (lastModified(styleBuf) <= lastMod) {
            return;

            /* Styles are changing beyond the modification, continue extending
            the end of the parse range by powers of 2 * REPARSE_CHUNK_SIZE and
            reparse until nothing changes */
        } else {
            lastMod  = lastModified(styleBuf);
            endParse = qMin(buf->BufGetLength(), forwardOneContext(buf, context, lastMod) + (REPARSE_CHUNK_SIZE << nPasses));
        }
    }
}

/*
** Return a position far enough back in "buf" from "fromPos" to give patterns
** their guranteed amount of context for matching (from "context").  If
** backing up by lines yields the greater distance, the returned position will
** be to the newline character before the start of the line, rather than to
** the first character of the line.  (I did this because earlier prototypes of
** the syntax highlighting code, which were based on single-line context, used
** this to ensure that line-spanning expressions would be detected.  I think
** it may reduce some 2 line context requirements to one line, at a cost of
** only one extra character, but I'm not sure, and my brain hurts from
** thinking about it).
*/
int SyntaxHighlighter::backwardOneContext(TextBuffer *buf, ReparseContext *context, int fromPos) {
    if (context->nLines == 0) {
        return qMax(0, fromPos - context->nChars);
    } else if (context->nChars == 0) {
        return qMax(0, buf->BufCountBackwardNLines(fromPos, context->nLines - 1) - 1);
    } else {
        return qMax(0, qMin(qMax(0, buf->BufCountBackwardNLines(fromPos, context->nLines - 1) - 1), fromPos - context->nChars));
    }
}

/*
** Return a position far enough forward in "buf" from "fromPos" to ensure
** that patterns are given their required amount of context for matching
** (from "context").  If moving forward by lines yields the greater
** distance, the returned position will be the first character of of the
** next line, rather than the newline character at the end (see notes in
** backwardOneContext).
*/
int SyntaxHighlighter::forwardOneContext(TextBuffer *buf, ReparseContext *context, int fromPos) {
    if (context->nLines == 0) {
        return qMin(buf->BufGetLength(), fromPos + context->nChars);
    } else if (context->nChars == 0) {
        return qMin(buf->BufGetLength(), buf->BufCountForwardNLines(fromPos, context->nLines));
    } else {
        return qMin(buf->BufGetLength(), qMax(buf->BufCountForwardNLines(fromPos, context->nLines), fromPos + context->nChars));
    }
}

/*
** Back up position pointed to by "pos" enough that parsing from that point
** on will satisfy context gurantees for pattern matching for modifications
** at pos.  Returns the style with which to begin parsing.  The caller is
** guranteed that parsing may safely BEGIN with that style, but not that it
** will continue at that level.
**
** This routine can be fooled if a continuous style run of more than one
** context distance in length is produced by multiple pattern matches which
** abut, rather than by a single continuous match.  In this  case the
** position returned by this routine may be a bad starting point which will
** result in an incorrect re-parse.  However this will happen very rarely,
** and, if it does, is unlikely to result in incorrect highlighting.
*/
int SyntaxHighlighter::findSafeParseRestartPos(TextBuffer *buf, windowHighlightData *highlightData, int *pos) {
    int style;
    int checkBackTo;
    int safeParseStart;

    char_type *const parentStyles         = highlightData->parentStyles;
    HighlightDataRecord *const pass1Patterns = highlightData->pass1Patterns;
    ReparseContext *const context         = &highlightData->contextRequirements;

    Q_ASSERT(pos);

    /* We must begin at least one context distance back from the change */
    *pos = backwardOneContext(buf, context, *pos);

    /* If the new position is outside of any styles or at the beginning of
       the buffer, this is a safe place to begin parsing, and we're done */
    if (*pos == 0) {
        return PLAIN_STYLE;
    }

    int startStyle = highlightData->styleBuffer->BufGetCharacter(*pos);
    if (IS_PLAIN(startStyle)) {
        return PLAIN_STYLE;
    }

    /*
    ** The new position is inside of a styled region, meaning, its pattern
    ** could potentially be affected by the modification.
    **
    ** Follow the style back by enough context to ensure that if we don't find
    ** its beginning, at least we've found a safe place to begin parsing
    ** within the styled region.
    **
    ** A safe starting position within a style is either at a style
    ** boundary, or far enough from the beginning and end of the style run
    ** to ensure that it's not within the start or end expression match
    ** (unfortunately, abutting styles can produce false runs so we're not
    ** really ensuring it, just making it likely).
    */
    if (patternIsParsable(patternOfStyle(pass1Patterns, startStyle))) {
        safeParseStart = backwardOneContext(buf, context, *pos);
        checkBackTo    = backwardOneContext(buf, context, safeParseStart);
    } else {
        safeParseStart = 0;
        checkBackTo    = 0;
    }

    int runningStyle = startStyle;
    for (int i = *pos - 1;; i--) {

        /* The start of the buffer is certainly a safe place to parse from */
        if (i == 0) {
            *pos = 0;
            return PLAIN_STYLE;
        }

        /* If the style is preceded by a parent style, it's safe to parse
       with the parent style, provided that the parent is parsable. */
        style = highlightData->styleBuffer->BufGetCharacter(i);
        if (isParentStyle(parentStyles, style, runningStyle)) {
            if (patternIsParsable(patternOfStyle(pass1Patterns, style))) {
                *pos = i + 1;
                return style;
            } else {
                /* The parent is not parsable, so well have to continue
                   searching. The parent now becomes the running style. */
                runningStyle = style;
            }
        }

        /* If the style is preceded by a child style, it's safe to resume
           parsing with the running style, provided that the running
           style is parsable. */
        else if (isParentStyle(parentStyles, runningStyle, style)) {
            if (patternIsParsable(patternOfStyle(pass1Patterns, runningStyle))) {
                *pos = i + 1;
                return runningStyle;
            }
            /* Else: keep searching; it's no use switching to the child style
               because even the running one is not parsable. */
        }

        /* If the style is preceded by a sibling style, it's safe to resume
           parsing with the common ancestor style, provided that the ancestor
           is parsable. Checking for siblings is very hard; checking whether
           the style has the same parent will probably catch 99% of the cases
           in practice. */
        else if (runningStyle != style && isParentStyle(parentStyles, parentStyleOf(parentStyles, runningStyle), style)) {
            int parentStyle = parentStyleOf(parentStyles, runningStyle);
            if (patternIsParsable(patternOfStyle(pass1Patterns, parentStyle))) {
                *pos = i + 1;
                return parentStyle;
            } else {
                /* Switch to the new style */
                runningStyle = style;
            }
        }

        /* If the style is preceded by an unrelated style, it's safe to
           resume parsing with PLAIN_STYLE. (Actually, it isn't, because
           we didn't really check for all possible sibling relations; but
           it will be ok in practice.) */
        else if (runningStyle != style) {
            *pos = i + 1;
            return PLAIN_STYLE;
        }

        /* If the style is parsable and didn't change for one whole context
           distance on either side of safeParseStart, safeParseStart is a
           reasonable guess at a place to start parsing.
           Note: No 'else' here! We may come from one of the 'fall-through
           cases' above. */
        if (i == checkBackTo) {
            *pos = safeParseStart;

            /* We should never return a non-parsable style, because it will
               result in an internal error. If the current style is not
               parsable, the pattern set most probably contains a context
               distance violation. In that case we can only avoid internal
               errors (by climbing the pattern hierarchy till we find a
               parsable ancestor) and hope that the highlighting errors are
               minor. */
            while (!patternIsParsable(patternOfStyle(pass1Patterns, runningStyle)))
                runningStyle = parentStyleOf(parentStyles, runningStyle);

            return runningStyle;
        }
    }
}

/*
** Search for a pattern in pattern list "patterns" with style "style"
*/
HighlightDataRecord *SyntaxHighlighter::patternOfStyle(HighlightDataRecord *patterns, int style) const {

    for (int i = 0; patterns[i].style != 0; i++) {
        if (patterns[i].style == style) {
            return &patterns[i];
        }
    }

    if (style == PLAIN_STYLE || style == UNFINISHED_STYLE) {
        return &patterns[0];
    }
    return nullptr;
}

/*
** Parse text in buffer "buf" between positions "beginParse" and "endParse"
** using pass 1 patterns over the entire range and pass 2 patterns where needed
** to determine whether re-parsed areas have changed and need to be redrawn.
** Deposits style information in "styleBuf" and expands the selection in
** styleBuf to show the additional areas which have changed and need
** redrawing.  beginParse must be a position from which pass 1 parsing may
** safely be started using the pass1Patterns given.  Internally, adds a
** "takeoff" safety region before beginParse, so that pass 2 patterns will be
** allowed to match properly if they begin before beginParse, and a "landing"
** safety region beyond endparse so that endParse is guranteed to be parsed
** correctly in both passes.  Returns the buffer position at which parsing
** finished (this will normally be endParse, unless the pass1Patterns is a
** pattern which does end and the end is reached).
*/
int SyntaxHighlighter::parseBufferRange(HighlightDataRecord *pass1Patterns, HighlightDataRecord *pass2Patterns,
                                        TextBuffer *buf, TextBuffer *styleBuf, ReparseContext *contextRequirements,
                                        int beginParse, int endParse, const char_type *delimiters) {
    int endSafety;
    int endPass2Safety;
    int startPass2Safety;
    int modStart;
    int modEnd;
    int beginSafety;
    int style;
    int firstPass2Style = !pass2Patterns ? INT_MAX : (unsigned char)pass2Patterns[1].style;

    /* Begin parsing one context distance back (or to the last style change) */
    int beginStyle = pass1Patterns->style;
    if (CAN_CROSS_LINE_BOUNDARIES(contextRequirements)) {
        beginSafety = backwardOneContext(buf, contextRequirements, beginParse);
        for (int p = beginParse; p >= beginSafety; p--) {
            style = styleBuf->BufGetCharacter(p - 1);
            if (!EQUIVALENT_STYLE(style, beginStyle, firstPass2Style)) {
                beginSafety = p;
                break;
            }
        }
    } else {
        for (beginSafety = qMax(0, beginParse - 1); beginSafety > 0; beginSafety--) {
            style = styleBuf->BufGetCharacter(beginSafety);
            if (!EQUIVALENT_STYLE(style, beginStyle, firstPass2Style) || buf->BufGetCharacter(beginSafety) == '\n') {
                beginSafety++;
                break;
            }
        }
    }

    /* Parse one parse context beyond requested end to gurantee that parsing
       at endParse is complete, unless patterns can't cross line boundaries,
       in which case the end of the line is fine */
    if (endParse == 0) {
        return 0;
    }

    if (CAN_CROSS_LINE_BOUNDARIES(contextRequirements)) {
        endSafety = forwardOneContext(buf, contextRequirements, endParse);
    } else if (endParse >= buf->BufGetLength() || (buf->BufGetCharacter(endParse - 1) == '\n')) {
        endSafety = endParse;
    } else {
        endSafety = qMin(buf->BufGetLength(), buf->BufEndOfLine(endParse) + 1);
    }

    /* copy the buffer range into a string */
    char_type *string      = buf->BufGetRange(beginSafety, endSafety);
    char_type *styleString = styleBuf->BufGetRange(beginSafety, endSafety);

    /* Parse it with pass 1 patterns */
    /* qDebug("parsing from %d thru %d\n", beginSafety, endSafety); */
    char_type prevChar         = getPrevChar(buf, beginParse);
    const char_type *stringPtr = &string[beginParse - beginSafety];
    char_type *stylePtr        = &styleString[beginParse - beginSafety];

    parseString(pass1Patterns, &stringPtr, &stylePtr, endParse - beginParse, &prevChar, false, delimiters, string, nullptr);

    /* On non top-level patterns, parsing can end early */
    endParse = qMin<long>(endParse, stringPtr - string + beginSafety);

    /* If there are no pass 2 patterns, we're done */
    if (!pass2Patterns) {
        goto parseDone;
    }

    /* Parsing of pass 2 patterns is done only as necessary for determining
       where styles have changed.  Find the area to avoid, which is already
       marked as changed (all inserted text and previously modified areas) */
    if (styleBuf->BufGetPrimarySelection().selected) {
        modStart = styleBuf->BufGetPrimarySelection().start;
        modEnd   = styleBuf->BufGetPrimarySelection().end;
    } else {
        modStart = 0;
        modEnd   = 0;
    }

    /* Re-parse the areas before the modification with pass 2 patterns, from
       beginSafety to far enough beyond modStart to gurantee that parsing at
       modStart is correct (pass 2 patterns must match entirely within one
       context distance, and only on the top level).  If the parse region
       ends entirely before the modification or at or beyond modEnd, parse
       the whole thing and take advantage of the safety region which will be
       thrown away below.  Otherwise save the contents of the safety region
       temporarily, and restore it after the parse. */
    if (beginSafety < modStart) {
        if (endSafety > modStart) {
            endPass2Safety = forwardOneContext(buf, contextRequirements, modStart);
            if (endPass2Safety + PASS_2_REPARSE_CHUNK_SIZE >= modEnd) {
                endPass2Safety = endSafety;
            }
        } else {
            endPass2Safety = endSafety;
		}
		
        prevChar = getPrevChar(buf, beginSafety);
        if (endPass2Safety == endSafety) {
            passTwoParseString(pass2Patterns, string, styleString, endParse - beginSafety, &prevChar, delimiters, string, nullptr);
            goto parseDone;
        } else {
            int tempLen = endPass2Safety - modStart;
            char_type *const temp = new char_type[tempLen];
			
            _strncpy(temp, &styleString[modStart - beginSafety], tempLen);

            passTwoParseString(pass2Patterns, string, styleString, modStart - beginSafety, &prevChar, delimiters, string, nullptr);
            _strncpy(&styleString[modStart - beginSafety], temp, tempLen);

            delete[] temp;
        }
    }

    /* Re-parse the areas after the modification with pass 2 patterns, from
       modEnd to endSafety, with an additional safety region before modEnd
       to ensure that parsing at modEnd is correct. */
    if (endParse > modEnd) {
        if (beginSafety > modEnd) {
            prevChar = getPrevChar(buf, beginSafety);
            passTwoParseString(pass2Patterns, string, styleString, endParse - beginSafety, &prevChar, delimiters, string, nullptr);
        } else {
            startPass2Safety = qMax(beginSafety, backwardOneContext(buf, contextRequirements, modEnd));
            int tempLen = modEnd - startPass2Safety;
            char_type *const temp = new char_type[tempLen];
            _strncpy(temp, &styleString[startPass2Safety - beginSafety], tempLen);

            prevChar = getPrevChar(buf, startPass2Safety);
            passTwoParseString(pass2Patterns, &string[startPass2Safety - beginSafety], &styleString[startPass2Safety - beginSafety], endParse - startPass2Safety, &prevChar, delimiters, string, nullptr);
							   
            _strncpy(&styleString[startPass2Safety - beginSafety], temp, tempLen);

            delete[] temp;
        }
    }

parseDone:

    /* Update the style buffer with the new style information, but only
       through endParse.  Skip the safety region at the end */
    styleString[endParse - beginSafety] = '\0';
    modifyStyleBuf(styleBuf, &styleString[beginParse - beginSafety], beginParse, endParse, firstPass2Style);
    delete[] styleString;
    delete[] string;

    return endParse;
}

/*
** Return the last modified position in styleBuf (as marked by modifyStyleBuf
** by the convention used for conveying modification information to the
** text widget, which is selecting the text)
*/
int SyntaxHighlighter::lastModified(TextBuffer *styleBuf) const {
    if (styleBuf->BufGetPrimarySelection().selected) {
        return qMax(0, styleBuf->BufGetPrimarySelection().end);
    }
    return 0;
}

int SyntaxHighlighter::parentStyleOf(const char_type *parentStyles, int style) {
    return parentStyles[(unsigned char)style - UNFINISHED_STYLE];
}

bool SyntaxHighlighter::isParentStyle(const char_type *parentStyles, int style1, int style2) {

    for (int p = parentStyleOf(parentStyles, style2); p != '\0'; p = parentStyleOf(parentStyles, p)) {
        if (style1 == p) {
            return true;
        }
    }
    return false;
}

/*
** Discriminates patterns which can be used with parseString from those which
** can't.  Leaf patterns are not suitable for parsing, because patterns
** contain the expressions used for parsing within the context of their own
** operation, i.e. the parent pattern initiates, and leaf patterns merely
** confirm and color.  Returns TRUE if the pattern is suitable for parsing.
*/
int SyntaxHighlighter::patternIsParsable(HighlightDataRecord *pattern) {
    return pattern && pattern->subPatternRE;
}

/*
** Takes a string which has already been parsed through pass1 parsing and
** re-parses the areas where pass two patterns are applicable.  Parameters
** have the same meaning as in parseString, except that strings aren't doubly
** indirect and string pointers are not updated.
*/
void SyntaxHighlighter::passTwoParseString(HighlightDataRecord *pattern, char_type *string, char_type *styleString, int length,
                                           char_type *prevChar, const char_type *delimiters, const char_type *lookBehindTo,
                                           const char_type *match_till) {

    int firstPass2Style = (unsigned char)pattern[1].style;

	char_type *s = styleString;
	char_type *c = string;
	for (;; c++, s++) {

		bool inParseRegion = false;
		const char_type *parseStart = nullptr;

        if (!inParseRegion && *c != _T('\0') &&
            (*s == UNFINISHED_STYLE || *s == PLAIN_STYLE || (unsigned char)*s >= firstPass2Style)) {
            parseStart = c;
            inParseRegion = true;
        }

        if (inParseRegion && (*c == _T('\0') || !(*s == UNFINISHED_STYLE || *s == PLAIN_STYLE || (unsigned char)*s >= firstPass2Style))) {
            char_type *parseEnd = c;
            if (parseStart != string) {
                *prevChar = *(parseStart - 1);
            }

            const char_type *stringPtr = parseStart;
            char_type *stylePtr = &styleString[parseStart - string];
            char_type temp = *parseEnd;
            *parseEnd = _T('\0');

            /* qDebug("pass2 parsing %d chars\n", strlen(stringPtr)); */
            parseString(pattern, &stringPtr, &stylePtr, qMin(parseEnd - parseStart, length - (parseStart - string)), prevChar, false, delimiters, lookBehindTo, match_till);

            *parseEnd = temp;
            inParseRegion = false;
        }

        if (*c == _T('\0') || (!inParseRegion && c - string >= length)) {
            break;
        }
    }
}

/*
** Get the character before position "pos" in buffer "buf"
*/
char_type SyntaxHighlighter::getPrevChar(TextBuffer *buf, int pos) {
    return pos == 0 ? _T('\0') : buf->BufGetCharacter(pos - 1);
}

/*
** Parses "string" according to compiled regular expressions in "pattern"
** until endRE is or errorRE are matched, or end of string is reached.
** Advances "string", "styleString" pointers to the next character past
** the end of the parsed section, and updates "prevChar" to reflect
** the new character before "string".
** If "anchored" is true, just scan the sub-pattern starting at the beginning
** of the string.  "length" is how much of the string must be parsed, but
** "string" must still be null terminated, the termination indicating how
** far the string should be searched, and "length" the part which is actually
** required (the string may or may not be parsed beyond "length").
**
** "lookBehindTo" indicates the boundary till where look-behind patterns may
** look back. If nullptr, the start of the string is assumed to be the boundary.
**
** "match_till" indicates the boundary till where matches may extend. If nullptr,
** it is assumed that the terminating \0 indicates the boundary. Note that
** look-ahead patterns can peek beyond the boundary, if supplied.
**
** Returns True if parsing was done and the parse succeeded.  Returns False if
** the error pattern matched, if the end of the string was reached without
** matching the end expression, or in the unlikely event of an internal error.
*/
bool SyntaxHighlighter::parseString(HighlightDataRecord *pattern, const char_type **string, char_type **styleString, int length,
                                    char_type *prevChar, bool anchored, const char_type *delimiters, const char_type *lookBehindTo,
                                    const char_type *match_till) {
    int i;
    bool subExecuted;
    signed char *subExpr;
    char_type succChar = match_till ? (*match_till) : '\0';
    HighlightDataRecord *subPat = nullptr;
    HighlightDataRecord *subSubPat;

    if (length <= 0)
        return false;

    const char_type *stringPtr = *string;
    char_type *stylePtr = *styleString;

    while (pattern->subPatternRE->ExecRE(stringPtr, anchored ? *string + 1 : *string + length + 1, false, *prevChar, succChar, delimiters, lookBehindTo, match_till)) {
        /* Beware of the case where only one real branch exists, but that
           branch has sub-branches itself. In that case the top_branch refers
           to the matching sub-branch and must be ignored. */
        int subIndex = (pattern->nSubBranches > 1) ? pattern->subPatternRE->top_branch() : 0;
        /* Combination of all sub-patterns and end pattern matched */
        /* qDebug("combined patterns RE matched at %d\n", pattern->subPatternRE->startp[0] - *string); */
        const char_type *startingStringPtr = stringPtr;

        /* Fill in the pattern style for the text that was skipped over before
           the match, and advance the pointers to the start of the pattern */
        fillStyleString(&stringPtr, &stylePtr, pattern->subPatternRE->startp(0), pattern->style, prevChar);

        /* If the combined pattern matched this pattern's end pattern, we're
           done.  Fill in the style string, update the pointers, color the
           end expression if there were coloring sub-patterns, and return */
        const char_type *savedStartPtr = stringPtr;
        char_type savedPrevChar = *prevChar;
        if (pattern->endRE) {
            if (subIndex == 0) {
                fillStyleString(&stringPtr, &stylePtr, pattern->subPatternRE->endp(0), pattern->style, prevChar);

                subExecuted = false;
                for (i = 0; i < pattern->nSubPatterns; i++) {
                    subPat = pattern->subPatterns[i];
                    if (subPat->colorOnly) {
                        if (!subExecuted) {
                            if (!pattern->endRE->ExecRE(savedStartPtr, savedStartPtr + 1, false, savedPrevChar,
                                                        succChar, delimiters, lookBehindTo, match_till)) {
                                qDebug("Internal error, failed to recover end match in parseString");
                                return false;
                            }
                            subExecuted = true;
                        }
                        for (subExpr = subPat->endSubexprs; *subExpr != -1; subExpr++)
                            recolorSubexpr(pattern->endRE, *subExpr, subPat->style, *string, *styleString);
                    }
                }
                *string = stringPtr;
                *styleString = stylePtr;
                return true;
            }
            --subIndex;
        }

        /* If the combined pattern matched this pattern's error pattern, we're
           done.  Fill in the style string, update the pointers, and return */
        if (pattern->errorRE) {
            if (subIndex == 0) {
                fillStyleString(&stringPtr, &stylePtr, pattern->subPatternRE->startp(0), pattern->style, prevChar);
                *string = stringPtr;
                *styleString = stylePtr;
                return false;
            }
            --subIndex;
        }

        /* Figure out which sub-pattern matched */
        for (i = 0; i < pattern->nSubPatterns; i++) {
            subPat = pattern->subPatterns[i];
            if (subPat->colorOnly)
                ++subIndex;
            else if (i == subIndex)
                break;
        }
        if (i == pattern->nSubPatterns) {
            qDebug("Internal error, failed to match in parseString");
            return false;
        }

        /* the sub-pattern is a simple match, just color it */
        if (!subPat->subPatternRE) {
            fillStyleString(&stringPtr, &stylePtr, pattern->subPatternRE->endp(0), /* subPat->startRE->endp(0),*/
                            subPat->style, prevChar);

            /* Parse the remainder of the sub-pattern */
        } else if (subPat->endRE) {
            /* The pattern is a starting/ending type of pattern, proceed with
               the regular hierarchical parsing. */

            /* If parsing should start after the start pattern, advance
               to that point (this is currently always the case) */
            if (!(subPat->flags & PARSE_SUBPATS_FROM_START))
                fillStyleString(&stringPtr, &stylePtr, pattern->subPatternRE->endp(0), /* subPat->startRE->endp(0),*/
                                subPat->style, prevChar);

            /* Parse to the end of the subPattern */
            parseString(subPat, &stringPtr, &stylePtr, length - (stringPtr - *string), prevChar, false, delimiters,
                        lookBehindTo, match_till);
        } else {
            /* If the parent pattern is not a start/end pattern, the
               sub-pattern can between the boundaries of the parent's
               match. Note that we must limit the recursive matches such
               that they do not exceed the parent's ending boundary.
               Without that restriction, matching becomes unstable. */

            /* Parse to the end of the subPattern */
            parseString(subPat, &stringPtr, &stylePtr, pattern->subPatternRE->endp(0) - stringPtr, prevChar, false,
                        delimiters, lookBehindTo, pattern->subPatternRE->endp(0));
        }

        /* If the sub-pattern has color-only sub-sub-patterns, add color
       based on the coloring sub-expression references */
        subExecuted = false;
        for (i = 0; i < subPat->nSubPatterns; i++) {
            subSubPat = subPat->subPatterns[i];
            if (subSubPat->colorOnly) {
                if (!subExecuted) {
                    if (!subPat->startRE->ExecRE(savedStartPtr, savedStartPtr + 1, false, savedPrevChar, succChar,
                                                 delimiters, lookBehindTo, match_till)) {
                        qDebug("Internal error, failed to recover start match in parseString");
                        return false;
                    }
                    subExecuted = true;
                }
                for (subExpr = subSubPat->startSubexprs; *subExpr != -1; subExpr++)
                    recolorSubexpr(subPat->startRE, *subExpr, subSubPat->style, *string, *styleString);
            }
        }

        /* Make sure parsing progresses.  If patterns match the empty string,
           they can get stuck and hang the process */
        if (stringPtr == startingStringPtr) {
            /* Avoid stepping over the end of the string (possible for
                   zero-length matches at end of the string) */
            if (*stringPtr == '\0')
                break;
            fillStyleString(&stringPtr, &stylePtr, stringPtr + 1, pattern->style, prevChar);
        }
    }

    /* If this is an anchored match (must match on first character), and
       nothing matched, return False */
    if (anchored && stringPtr == *string)
        return false;

    /* Reached end of string, fill in the remaining text with pattern style
       (unless this was an anchored match) */
    if (!anchored)
        fillStyleString(&stringPtr, &stylePtr, *string + length, pattern->style, prevChar);

    /* Advance the string and style pointers to the end of the parsed text */
    *string = stringPtr;
    *styleString = stylePtr;
    return pattern->endRE == nullptr;
}

/*
** Incorporate changes from styleString into styleBuf, tracking changes
** in need of redisplay, and marking them for redisplay by the text
** modification callback in textDisp.c.  "firstPass2Style" is necessary
** for distinguishing pass 2 styles which compare as equal to the unfinished
** style in the original buffer, from pass1 styles which signal a change.
*/
void SyntaxHighlighter::modifyStyleBuf(TextBuffer *styleBuf, char_type *styleString, int startPos, int endPos,
                                       int firstPass2Style) {
    char_type *c, bufChar;
    int pos, modStart, modEnd, minPos = INT_MAX, maxPos = 0;
    Selection *sel = &styleBuf->BufGetPrimarySelection();

    /* Skip the range already marked for redraw */
    if (sel->selected) {
        modStart = sel->start;
        modEnd = sel->end;
    } else
        modStart = modEnd = startPos;

    /* Compare the original style buffer (outside of the modified range) with
       the new string with which it will be updated, to find the extent of
       the modifications.  Unfinished styles in the original match any
       pass 2 style */
    for (c = styleString, pos = startPos; pos < modStart && pos < endPos; c++, pos++) {
        bufChar = styleBuf->BufGetCharacter(pos);
        if (*c != bufChar &&
            !(bufChar == UNFINISHED_STYLE && (*c == PLAIN_STYLE || (unsigned char)*c >= firstPass2Style))) {
            if (pos < minPos)
                minPos = pos;
            if (pos > maxPos)
                maxPos = pos;
        }
    }
    for (c = &styleString[qMax(0, modEnd - startPos)], pos = qMax(modEnd, startPos); pos < endPos; c++, pos++) {
        bufChar = styleBuf->BufGetCharacter(pos);
        if (*c != bufChar &&
            !(bufChar == UNFINISHED_STYLE && (*c == PLAIN_STYLE || (unsigned char)*c >= firstPass2Style))) {
            if (pos < minPos)
                minPos = pos;
            if (pos + 1 > maxPos)
                maxPos = pos + 1;
        }
    }

    /* Make the modification */
    styleBuf->BufReplace(startPos, endPos, styleString);

    /* Mark or extend the range that needs to be redrawn.  Even if no
       change was made, it's important to re-establish the selection,
       because it can get damaged by the BufReplace above */
    styleBuf->BufSelect(qMin(modStart, minPos), qMax(modEnd, maxPos));
}

/*
** Advance "stringPtr" and "stylePtr" until "stringPtr" == "toPtr", filling
** "stylePtr" with style "style".  Can also optionally update the pre-string
** character, prevChar, which is fed to regular the expression matching
** routines for determining word and line boundaries at the start of the string.
*/
void SyntaxHighlighter::fillStyleString(const char_type **stringPtr, char_type **stylePtr, const char_type *toPtr, char_type style, char_type *prevChar) {
    int i, len = toPtr - *stringPtr;

    if (*stringPtr >= toPtr)
        return;

    for (i = 0; i < len; i++)
        *(*stylePtr)++ = style;
    if (prevChar)
        *prevChar = *(toPtr - 1);
    *stringPtr = toPtr;
}

/*
** Change styles in the portion of "styleString" to "style" where a particular
** sub-expression, "subExpr", of regular expression "re" applies to the
** corresponding portion of "string".
*/
void SyntaxHighlighter::recolorSubexpr(RegExp *re, int subexpr, int style, const char_type *string, char_type *styleString) {
    const char_type *stringPtr;
    char_type *stylePtr;

    stringPtr = re->startp(subexpr);
    stylePtr = &styleString[stringPtr - string];
    fillStyleString(&stringPtr, &stylePtr, re->endp(subexpr), style, nullptr);
}

/*
** Create complete syntax highlighting information from "patternSrc", using
** highlighting fonts from "window", includes pattern compilation.  If errors
** are encountered, warns user with a dialog and returns nullptr.  To free the
** allocated components of the returned data structure, use freeHighlightData.
*/
windowHighlightData *SyntaxHighlighter::createHighlightData(patternSet *patSet) {

    Q_ASSERT(patSet);

    QVector<highlightPattern> &patternSrc = patSet->patterns;
    int nPatterns = patSet->patterns.size();
    int contextLines = patSet->lineContext;
    int contextChars = patSet->charContext;
    int i, nPass1Patterns, nPass2Patterns;
    bool noPass1;
    bool noPass2;
    char_type *parentStyles;
    char_type *parentStylesPtr;
    QString parentName;
    highlightPattern *pass1PatternSrc;
    highlightPattern *pass2PatternSrc;
    highlightPattern *p1Ptr, *p2Ptr;
    styleTableEntry *styleTable;
    styleTableEntry *styleTablePtr;
    TextBuffer *styleBuf;
    HighlightDataRecord *pass1Pats, *pass2Pats;
    windowHighlightData *highlightData;

    /* The highlighting code can't handle empty pattern sets, quietly say no */
    if (nPatterns == 0) {
        return nullptr;
    }

    /* Check that the styles and parent pattern names actually exist */
    if (!NamedStyleExists("Plain")) {
		QMessageBox::warning(
			nullptr, 
			tr("Highlight Style Highlight style \"Plain\" is missing"), 
			tr("OK"));
        return nullptr;
    }

    for (i = 0; i < nPatterns; i++) {
        if (!patternSrc[i].subPatternOf.isNull() &&
            indexOfNamedPattern(patternSrc, nPatterns, patternSrc[i].subPatternOf) == -1) {

            QMessageBox::warning(
				nullptr,
				tr("Parent Pattern"), 
				tr("Parent field \"%1\" in pattern \"%2\"\ndoes not match any highlight patterns in this set").arg(patternSrc[i].subPatternOf).arg(patternSrc[i].name));


            return nullptr;
        }
    }

    for (i = 0; i < nPatterns; i++) {
        if (!NamedStyleExists(patternSrc[i].style)) {

            QMessageBox::warning(
				nullptr,
				tr("Highlight Style"),
				tr("Style \"%1\" named in pattern \"%2\"\ndoes not match any existing style").arg(patternSrc[i].style).arg(patternSrc[i].name));

            return nullptr;
        }
    }

    /* Make DEFER_PARSING flags agree with top level patterns (originally,
       individual flags had to be correct and were checked here, but dialog now
       shows this setting only on top patterns which is much less confusing) */
    for (i = 0; i < nPatterns; i++) {
        if (!patternSrc[i].subPatternOf.isNull()) {
            int parentindex;

            parentindex = findTopLevelParentIndex(patternSrc, nPatterns, i);
            if (parentindex == -1) {
					
				QMessageBox::warning(
					nullptr,
					tr("Parent Pattern"),
					tr("Pattern \"%1\" does not have valid parent").arg(patternSrc[i].name));
					
                return nullptr;
            }

            if (patternSrc[parentindex].flags & DEFER_PARSING) {
                patternSrc[i].flags |= DEFER_PARSING;
            } else {
                patternSrc[i].flags &= ~DEFER_PARSING;
            }
        }
    }

    /* Sort patterns into those to be used in pass 1 parsing, and those to
       be used in pass 2, and add default pattern (0) to each list */
    nPass1Patterns = 1;
    nPass2Patterns = 1;
    for (i = 0; i < nPatterns; i++) {
        if (patternSrc[i].flags & DEFER_PARSING) {
            nPass2Patterns++;
        } else {
            nPass1Patterns++;
        }
    }

    p1Ptr = pass1PatternSrc = new highlightPattern[nPass1Patterns];
    p2Ptr = pass2PatternSrc = new highlightPattern[nPass2Patterns];
    p1Ptr->name = p2Ptr->name = "";
    p1Ptr->startRE = p2Ptr->startRE = nullptr;
    p1Ptr->endRE = p2Ptr->endRE = nullptr;
    p1Ptr->errorRE = p2Ptr->errorRE = nullptr;
    p1Ptr->style = p2Ptr->style = "Plain";
    p1Ptr->subPatternOf = p2Ptr->subPatternOf = nullptr;
    p1Ptr->flags = p2Ptr->flags = 0;
    p1Ptr++;
    p2Ptr++;
    for (i = 0; i < nPatterns; i++) {
        if (patternSrc[i].flags & DEFER_PARSING)
            *p2Ptr++ = patternSrc[i];
        else
            *p1Ptr++ = patternSrc[i];
    }

    /* If a particular pass is empty except for the default pattern, don't
       bother compiling it or setting up styles */
    if (nPass1Patterns == 1)
        nPass1Patterns = 0;
    if (nPass2Patterns == 1)
        nPass2Patterns = 0;

    /* Compile patterns */
    if (nPass1Patterns == 0) {
        pass1Pats = nullptr;
    } else {
        pass1Pats = compilePatterns(pass1PatternSrc, nPass1Patterns);
        if (!pass1Pats) {
            return nullptr;
        }
    }

    if (nPass2Patterns == 0) {
        pass2Pats = nullptr;
    } else {
        pass2Pats = compilePatterns(pass2PatternSrc, nPass2Patterns);
        
        if (!pass2Pats) {
			delete [] pass1Pats;
            return nullptr;
        }
    }

    /* Set pattern styles.  If there are pass 2 patterns, pass 1 pattern
       0 should have a default style of UNFINISHED_STYLE.  With no pass 2
       patterns, unstyled areas of pass 1 patterns should be PLAIN_STYLE
       to avoid triggering re-parsing every time they are encountered */
    noPass1 = nPass1Patterns == 0;
    noPass2 = nPass2Patterns == 0;
    if (noPass2)
        pass1Pats[0].style = PLAIN_STYLE;
    else if (noPass1)
        pass2Pats[0].style = PLAIN_STYLE;
    else {
        pass1Pats[0].style = UNFINISHED_STYLE;
        pass2Pats[0].style = PLAIN_STYLE;
    }
    for (i = 1; i < nPass1Patterns; i++)
        pass1Pats[i].style = PLAIN_STYLE + i;
    for (i = 1; i < nPass2Patterns; i++)
        pass2Pats[i].style = PLAIN_STYLE + (noPass1 ? 0 : nPass1Patterns - 1) + i;

    /* Create table for finding parent styles */
    parentStylesPtr = parentStyles = new char_type[nPass1Patterns + nPass2Patterns + 2];
    *parentStylesPtr++ = '\0';
    *parentStylesPtr++ = '\0';
    for (i = 1; i < nPass1Patterns; i++) {
        parentName = pass1PatternSrc[i].subPatternOf;
        *parentStylesPtr++ = (parentName.isNull())
                                 ? PLAIN_STYLE
                                 : pass1Pats[indexOfNamedPattern(pass1PatternSrc, nPass1Patterns, parentName)].style;
    }
    for (i = 1; i < nPass2Patterns; i++) {
        parentName = pass2PatternSrc[i].subPatternOf;
        *parentStylesPtr++ = (parentName.isNull())
                                 ? PLAIN_STYLE
                                 : pass2Pats[indexOfNamedPattern(pass2PatternSrc, nPass2Patterns, parentName)].style;
    }

    /* Set up table for mapping colors and fonts to syntax */
    styleTable = new styleTableEntry[nPass1Patterns + nPass2Patterns + 1];
    styleTablePtr = styleTable;

    auto setStyleTablePtr = [this](styleTableEntry *p, highlightPattern *pat) {

        p->highlightName = pat->name;
        p->styleName = pat->style;
        p->colorName = ColorOfNamedStyle(pat->style);
        p->bgColorName = BgColorOfNamedStyle(pat->style);
        p->isBold = FontOfNamedStyleIsBold(pat->style);
        p->isItalic = FontOfNamedStyleIsItalic(pat->style);
        /* And now for the more physical stuff */
        p->color = X11Colors::fromString(p->colorName);
        if (!p->bgColorName.isNull()) {
            p->bgColor = X11Colors::fromString(p->bgColorName);
        } else {
            p->bgColor = p->color;
        }
        p->font = FontOfNamedStyle(pat->style);
    };

    /* PLAIN_STYLE (pass 1) */
    styleTablePtr->underline = false;
    setStyleTablePtr(styleTablePtr++, noPass1 ? &pass2PatternSrc[0] : &pass1PatternSrc[0]);
    /* PLAIN_STYLE (pass 2) */
    styleTablePtr->underline = false;
    setStyleTablePtr(styleTablePtr++, noPass2 ? &pass1PatternSrc[0] : &pass2PatternSrc[0]);
    /* explicit styles (pass 1) */
    for (i = 1; i < nPass1Patterns; i++) {
        styleTablePtr->underline = false;
        setStyleTablePtr(styleTablePtr++, &pass1PatternSrc[i]);
    }
    /* explicit styles (pass 2) */
    for (i = 1; i < nPass2Patterns; i++) {
        styleTablePtr->underline = false;
        setStyleTablePtr(styleTablePtr++, &pass2PatternSrc[i]);
    }

    /* Free the temporary sorted pattern source list */
    delete[] pass1PatternSrc;
    delete[] pass2PatternSrc;

    /* Create the style buffer */
    styleBuf = new TextBuffer();

    /* Collect all of the highlighting information in a single structure */
    highlightData = new windowHighlightData;
    highlightData->pass1Patterns = pass1Pats;
    highlightData->pass2Patterns = pass2Pats;
    highlightData->parentStyles = parentStyles;
    highlightData->styleTable = styleTable;
    highlightData->nStyles = styleTablePtr - styleTable;
    highlightData->styleBuffer = styleBuf;
    highlightData->contextRequirements.nLines = contextLines;
    highlightData->contextRequirements.nChars = contextChars;
    highlightData->patternSetForWindow = patSet;

    return highlightData;
}

/*
** Determine whether a named style exists
*/
bool SyntaxHighlighter::NamedStyleExists(const QString &styleName) {
    return lookupNamedStyle(styleName) != -1;
}

int SyntaxHighlighter::indexOfNamedPattern(highlightPattern *patList, int nPats, const QString &patName) const {
    int i;

    if (patName.isNull())
        return -1;
    for (i = 0; i < nPats; i++)
        if (patList[i].name == patName)
            return i;
    return -1;
}

int SyntaxHighlighter::indexOfNamedPattern(const QVector<highlightPattern> &patList, int nPats,
                                           const QString &patName) const {
    int i;

    if (patName.isNull())
        return -1;
    for (i = 0; i < nPats; i++)
        if (patList[i].name == patName)
            return i;
    return -1;
}

int SyntaxHighlighter::findTopLevelParentIndex(const QVector<highlightPattern> &patList, int nPats, int index) const {
    int topIndex;

    topIndex = index;
    while (!patList[topIndex].subPatternOf.isNull()) {
        topIndex = indexOfNamedPattern(patList, nPats, patList[topIndex].subPatternOf);
        if (index == topIndex)
            return -1; /* amai: circular dependency ?! */
    }
    return topIndex;
}

int SyntaxHighlighter::findTopLevelParentIndex(highlightPattern *patList, int nPats, int index) const {
    int topIndex;

    topIndex = index;
    while (!patList[topIndex].subPatternOf.isNull()) {
        topIndex = indexOfNamedPattern(patList, nPats, patList[topIndex].subPatternOf);
        if (index == topIndex)
            return -1; /* amai: circular dependency ?! */
    }
    return topIndex;
}

/*
** Find the color associated with a named style.  This routine must only be
** called with a valid styleName (call NamedStyleExists to find out whether
** styleName is valid).
*/
QString SyntaxHighlighter::ColorOfNamedStyle(const QString &styleName) const {
    int styleNo = lookupNamedStyle(styleName);

    if (styleNo < 0) {
        return "black";
    }

    return HighlightStyles[styleNo]->color;
}

/*
** Find the index into the HighlightStyles array corresponding to "styleName".
** If styleName is not found, return -1.
*/
int SyntaxHighlighter::lookupNamedStyle(const QString &styleName) const {
    int i;

    for (i = 0; i < HighlightStyles.size(); i++) {
        if (styleName == HighlightStyles[i]->name) {
            return i;
        }
    }

    return -1;
}

/*
** Find the font (font struct) associated with a named style.
** This routine must only be called with a valid styleName (call
** NamedStyleExists to find out whether styleName is valid).
*/
QFont SyntaxHighlighter::FontOfNamedStyle(const QString &styleName) {
    Q_UNUSED(styleName);
#if 0
    int styleNo=lookupNamedStyle(styleName),fontNum;
    XFontStruct *font;

    if (styleNo<0)
        return GetDefaultFontStruct(window->fontList);
    fontNum = HighlightStyles[styleNo]->font;
    if (fontNum == BOLD_FONT)
        font = window->boldFontStruct;
    else if (fontNum == ITALIC_FONT)
        font = window->italicFontStruct;
    else if (fontNum == BOLD_ITALIC_FONT)
        font = window->boldItalicFontStruct;
    else /* fontNum == PLAIN_FONT */
        font = GetDefaultFontStruct(window->fontList);

    /* If font isn't loaded, silently substitute primary font */
    return (font == nullptr) ? GetDefaultFontStruct(window->fontList) : font;
#endif
    return QFont();
}

bool SyntaxHighlighter::FontOfNamedStyleIsBold(const QString &styleName) {

    int styleNo=lookupNamedStyle(styleName);

    if (styleNo<0)
        return false;

    return HighlightStyles[styleNo]->bold;
}

bool SyntaxHighlighter::FontOfNamedStyleIsItalic(const QString &styleName) {
    int styleNo=lookupNamedStyle(styleName);

    if (styleNo<0)
        return false;

    return HighlightStyles[styleNo]->italic;
}

/*
** Find the background color associated with a named style.
*/
QString SyntaxHighlighter::BgColorOfNamedStyle(const QString &styleName) {
    Q_UNUSED(styleName);
#if 0
    int styleNo=lookupNamedStyle(styleName);

    if (styleNo<0)
        return "";
    return HighlightStyles[styleNo]->bgColor;
#endif
    return "black";
}

/*
** Transform pattern sources into the compiled highlight information
** actually used by the code.  Output is a tree of highlightDataRec structures
** containing compiled regular expressions and style information.
*/
HighlightDataRecord *SyntaxHighlighter::compilePatterns(highlightPattern *patternSrc, int nPatterns) {

    int subExprNum;
    int charsRead;


    /* Allocate memory for the compiled patterns.  The list is terminated
       by a record with style == 0. */
    auto compiledPats = new HighlightDataRecord[nPatterns + 1];
    compiledPats[nPatterns].style = 0;

    /* Build the tree of parse expressions */
    for (int i = 0; i < nPatterns; i++) {
        compiledPats[i].nSubPatterns = 0;
        compiledPats[i].nSubBranches = 0;
    }

    for (int i = 1; i < nPatterns; i++) {
        if (patternSrc[i].subPatternOf.isNull()) {
            compiledPats[0].nSubPatterns++;
        } else {
            compiledPats[indexOfNamedPattern(patternSrc, nPatterns, patternSrc[i].subPatternOf)].nSubPatterns++;
        }
    }

#if 0
    for (int i = 0; i < nPatterns; i++) {
        compiledPats[i].subPatterns = compiledPats[i].nSubPatterns == 0 ? nullptr : new HighlightDataRecord *[compiledPats[i].nSubPatterns];
    }
#endif

    for (int i = 0; i < nPatterns; i++) {
        compiledPats[i].nSubPatterns = 0;
    }

    for (int i = 1; i < nPatterns; i++) {
        if (patternSrc[i].subPatternOf.isNull()) {
            compiledPats[0].subPatterns.push_back(&compiledPats[i]);
            compiledPats[0].nSubPatterns++;
            //compiledPats[0].subPatterns[compiledPats[0].nSubPatterns++] = &compiledPats[i];
        } else {
            int parentIndex = indexOfNamedPattern(patternSrc, nPatterns, patternSrc[i].subPatternOf);

            compiledPats[parentIndex].subPatterns.push_back(&compiledPats[i]);
            compiledPats[parentIndex].nSubPatterns++;
            //compiledPats[parentIndex].subPatterns[compiledPats[parentIndex].nSubPatterns++] = &compiledPats[i];
        }
    }

    /* Process color-only sub patterns (no regular expressions to match,
       just colors and fonts for sub-expressions of the parent pattern */
    for (int i = 0; i < nPatterns; i++) {
        compiledPats[i].colorOnly = (patternSrc[i].flags & COLOR_ONLY);
        compiledPats[i].userStyleIndex = IndexOfNamedStyle(patternSrc[i].style);
        if (compiledPats[i].colorOnly && compiledPats[i].nSubPatterns != 0) {

            QMessageBox::warning(
				nullptr,
				tr("Color-only Pattern"),
				tr("Color-only pattern \"%1\" may not have subpatterns").arg(patternSrc[i].name));

            return nullptr;
        }

        int nSubExprs = 0;
        if (!patternSrc[i].startRE.isNull()) {
				
		 // TODO(eteran): hack, fixme
#ifdef USE_WCHAR		
            const char_type *ptr = patternSrc[i].startRE.toStdWString().c_str();
#else
			const char_type *ptr = patternSrc[i].startRE.toStdString().c_str();
#endif
            while (true) {
                if (*ptr == _T('&')) {
                    compiledPats[i].startSubexprs[nSubExprs++] = 0;
                    ptr++;
                } else if (_sscanf(ptr, _T("\\%d%n"), &subExprNum, &charsRead) == 1) {
                    compiledPats[i].startSubexprs[nSubExprs++] = subExprNum;
                    ptr += charsRead;
                } else {
                    break;
				}
            }
        }

        compiledPats[i].startSubexprs[nSubExprs] = -1;
        nSubExprs = 0;
        if (!patternSrc[i].endRE.isNull()) {
		
		 // TODO(eteran): hack, fixme
#ifdef USE_WCHAR		
            const char_type *ptr = patternSrc[i].endRE.toStdWString().c_str();
#else
			const char_type *ptr = patternSrc[i].endRE.toStdString().c_str();
#endif
            while (true) {
                if (*ptr == _T('&')) {
                    compiledPats[i].endSubexprs[nSubExprs++] = 0;
                    ptr++;
                } else if (_sscanf(ptr, _T("\\%d%n"), &subExprNum, &charsRead) == 1) {
                    compiledPats[i].endSubexprs[nSubExprs++] = subExprNum;
                    ptr += charsRead;
                } else {
                    break;
				}
            }
        }
        compiledPats[i].endSubexprs[nSubExprs] = -1;
    }

    /* Compile regular expressions for all highlight patterns */
    for (int i = 0; i < nPatterns; i++) {
        if (patternSrc[i].startRE.isNull() || compiledPats[i].colorOnly) {
            compiledPats[i].startRE = nullptr;
        } else {
			compiledPats[i].startRE = compileREAndWarn(patternSrc[i].startRE);
            if (!compiledPats[i].startRE) {
                return nullptr;
			}
        }
		
        if (patternSrc[i].endRE.isNull() || compiledPats[i].colorOnly) {
            compiledPats[i].endRE = nullptr;
        } else {
            compiledPats[i].endRE = compileREAndWarn(patternSrc[i].endRE);
			if (!compiledPats[i].endRE) {
                return nullptr;
			}
        }
		
        if (patternSrc[i].errorRE.isNull()) {
            compiledPats[i].errorRE = nullptr;
        } else {
			compiledPats[i].errorRE = compileREAndWarn(patternSrc[i].errorRE);
            if (!compiledPats[i].errorRE) {
                return nullptr;
			}
        }
    }

    /* Construct and compile the great hairy pattern to match the OR of the
       end pattern, the error pattern, and all of the start patterns of the
       sub-patterns */
    for (int patternNum = 0; patternNum < nPatterns; patternNum++) {
        if (patternSrc[patternNum].endRE.isNull()  && patternSrc[patternNum].errorRE.isNull() &&
            compiledPats[patternNum].nSubPatterns == 0) {
            compiledPats[patternNum].subPatternRE = nullptr;
            continue;
        }

        size_t length = (compiledPats[patternNum].colorOnly || patternSrc[patternNum].endRE.isNull()) ? 0 : patternSrc[patternNum].endRE.size() + 5;
        length += (compiledPats[patternNum].colorOnly || patternSrc[patternNum].errorRE.isNull()) ? 0 : patternSrc[patternNum].errorRE.size() + 5;

        for (int i = 0; i < compiledPats[patternNum].nSubPatterns; i++) {
            int subPatIndex = compiledPats[patternNum].subPatterns[i] - compiledPats;
            length += compiledPats[subPatIndex].colorOnly ? 0 : patternSrc[subPatIndex].startRE.size() + 5;
        }

        if (length == 0) {
            compiledPats[patternNum].subPatternRE = nullptr;
            continue;
        }

		QString bigPattern;
		
        if (!patternSrc[patternNum].endRE.isNull()) {		
			bigPattern += QString("(?:%1)|").arg(patternSrc[patternNum].endRE);
            compiledPats[patternNum].nSubBranches++;
        }
		
		
		
		
		
        if (!patternSrc[patternNum].errorRE.isNull()) {		
			bigPattern += QString("(?:%1)|").arg(patternSrc[patternNum].errorRE);
            compiledPats[patternNum].nSubBranches++;
        }
		
		
	
		
        for (int i = 0; i < compiledPats[patternNum].nSubPatterns; i++) {
            int subPatIndex = compiledPats[patternNum].subPatterns[i] - compiledPats;
            if (compiledPats[subPatIndex].colorOnly) {
                continue;
			}
			
			bigPattern += QString("(?:%1)|").arg(patternSrc[subPatIndex].startRE);
            compiledPats[patternNum].nSubBranches++;
        }
			
		bigPattern = bigPattern.left(bigPattern.size() - 1);
		

	
		
        try {
			compiledPats[patternNum].subPatternRE = new RegExp(qPrintable(bigPattern), REDFLT_STANDARD);
        } catch (const std::exception &e) {
            compiledPats[patternNum].subPatternRE = nullptr;
            qDebug("Error compiling syntax highlight patterns:\n%s", e.what());
            return nullptr;
        }

    }

    /* Copy remaining parameters from pattern template to compiled tree */
    for (int i = 0; i < nPatterns; i++) {
        compiledPats[i].flags = patternSrc[i].flags;
    }

    return compiledPats;
}

/*
** Returns a unique number of a given style name
*/
int SyntaxHighlighter::IndexOfNamedStyle(const QString &styleName) const {
    return lookupNamedStyle(styleName);
}

/*
** compile a regular expression and present a user friendly dialog on failure.
*/
RegExp *SyntaxHighlighter::compileREAndWarn(const QString &re) {
    try {
#ifdef USE_WCHAR
        return new RegExp(re.toStdWString().c_str(), REDFLT_STANDARD);
#else	
        return new RegExp(re.toStdString().c_str(), REDFLT_STANDARD);
#endif
    } catch (const std::exception &e) {

		QMessageBox::warning(
			nullptr,
			tr("Error in Regex"),
			tr("Error in syntax highlighting regular expression:\n%1").arg(e.what())
		);
			
        return nullptr;
    }
}

/*
** Find the pattern set matching the window's current language mode, or
** tell the user if it can't be done (if warn is True) and return nullptr.
*/
patternSet *SyntaxHighlighter::findPatternsForWindow(bool warn) {
    patternSet *patterns;

    /* Find the window's language mode.  If none is set, warn user */
    QString modeName = LanguageModeName(/*window->languageMode*/ 0);
    if (modeName.isNull()) {
        if (warn) {
			QMessageBox::warning(
				nullptr,
				tr("Language Mode"),
				tr("No language-specific mode has been set for this file.\n\n"
				   "To use syntax highlighting in this window, please select a\n"
				   "language from the Preferences -> Language Modes menu.\n\n"
				   "New language modes and syntax highlighting patterns can be\n"
				   "added via Preferences -> Default Settings -> Language Modes,\n"
				   "and Preferences -> Default Settings -> Syntax Highlighting."));
        }
        return nullptr;
    }

    /* Look up the appropriate pattern for the language */
    patterns = FindPatternSet(modeName);
    if (!patterns) {
        if (warn) {
			QMessageBox::warning(
				nullptr,
				tr("Language Mode"),
				tr("Syntax highlighting is not available in language\n"
				   "mode %1.\n\n"
				   "You can create new syntax highlight patterns in the\n"
				   "Preferences -> Default Settings -> Syntax Highlighting\n"
				   "dialog, or choose a different language mode from:\n"
				   "Preferences -> Language Mode.").arg(modeName));
				   
            return nullptr;
        }
    }

    return patterns;
}

/*
** Return the name of the current language mode set in "window", or nullptr
** if the current mode is "Plain".
*/
QString SyntaxHighlighter::LanguageModeName(int mode) {
    if (mode == PLAIN_LANGUAGE_MODE) {
        return QString();
    } else {
        return LanguageModes[mode]->name;
    }
}

/*
** Look through the list of pattern sets, and find the one for a particular
** language.  Returns nullptr if not found.
*/
patternSet *SyntaxHighlighter::FindPatternSet(const QString &langModeName) {

    if (langModeName.isNull()) {
        return nullptr;
    }

    for (int i = 0; i < PatternSets.size(); i++) {
        if (langModeName == PatternSets[i]->languageMode) {
            return PatternSets[i];
        }
    }

    return nullptr;
}

styleTableEntry *SyntaxHighlighter::styleEntry(int index) const {
    return &highlightData_->styleTable[index];
}

/*
** Callback to parse an "unfinished" region of the buffer.  "unfinished" means
** that the buffer has been parsed with pass 1 patterns, but this section has
** not yet been exposed, and thus never had pass 2 patterns applied.  This
** callback is invoked when the text widget's display routines encounter one
** of these unfinished regions.  "pos" is the first position encountered which
** needs re-parsing.  This routine applies pass 2 patterns to a chunk of
** the buffer of size PASS_2_REPARSE_CHUNK_SIZE beyond pos.
*/
void SyntaxHighlighter::unfinishedHighlightEncountered(const HighlightEvent *event) {

    TextBuffer *buf = event->buffer;
	
    int beginParse;
	int endParse;
	int beginSafety;
	int endSafety;
	int p;
    windowHighlightData *highlightData = highlightData_;
	
	TextBuffer *styleBuf = highlightData->styleBuffer;

    ReparseContext *context = &highlightData->contextRequirements;
    HighlightDataRecord *pass2Patterns = highlightData->pass2Patterns;
    char_type *string;
	char_type *styleString;
	char_type *stylePtr;
	char_type c;
	char_type prevChar;
    const char_type *stringPtr;
    int firstPass2Style = (unsigned char)pass2Patterns[1].style;
    
    /* If there are no pass 2 patterns to process, do nothing (but this
       should never be triggered) */
    if (!pass2Patterns)
    	return;
    
    /* Find the point at which to begin parsing to ensure that the character at
       pos is parsed correctly (beginSafety), at most one context distance back
       from pos, unless there is a pass 1 section from which to start */
    beginParse = event->pos;
    beginSafety = backwardOneContext(buf, context, beginParse);
    for (p=beginParse; p>=beginSafety; p--) {
    	c = styleBuf->BufGetCharacter(p);
    	if (c != UNFINISHED_STYLE && c != PLAIN_STYLE &&
		(unsigned char)c < firstPass2Style) {
    	    beginSafety = p + 1;
    	    break;
    	}
    }
    
    /* Decide where to stop (endParse), and the extra distance (endSafety)
       necessary to ensure that the changes at endParse are correct.  Stop at
       the end of the unfinished region, or a max. of PASS_2_REPARSE_CHUNK_SIZE
       characters forward from the requested position */
    endParse = qMin(buf->BufGetLength(), event->pos + PASS_2_REPARSE_CHUNK_SIZE);
    endSafety = forwardOneContext(buf, context, endParse);
    for (p=event->pos; p<endSafety; p++) {
    	c = styleBuf->BufGetCharacter(p);
    	if (c != UNFINISHED_STYLE && c != PLAIN_STYLE &&
		(unsigned char)c < firstPass2Style) {
    	    endParse = qMin(endParse, p);
    	    endSafety = p;
    	    break;
    	} else if (c != UNFINISHED_STYLE && p < endParse) {
    	    endParse = p;
    	    if ((unsigned char)c < firstPass2Style)
    	    	endSafety = p;
    	    else
    	    	endSafety = forwardOneContext(buf, context, endParse);
    	    break;
    	}
    }
    
    /* Copy the buffer range into a string */
    /* qDebug("callback pass2 parsing from %d thru %d w/ safety from %d thru %d\n", beginParse, endParse, beginSafety, endSafety); */
    stringPtr = string = buf->BufGetRange(beginSafety, endSafety);
    styleString = stylePtr = styleBuf->BufGetRange(beginSafety, endSafety);
    
    /* Parse it with pass 2 patterns */
    prevChar = getPrevChar(buf, beginSafety);
    parseString(pass2Patterns, &stringPtr, &stylePtr, endParse - beginSafety,
            &prevChar, false, delimiters, string, nullptr);

    /* Update the style buffer the new style information, but only between
       beginParse and endParse.  Skip the safety region */
    styleString[endParse-beginSafety] = '\0';
    styleBuf->BufReplace(beginParse, endParse, &styleString[beginParse-beginSafety]);
    delete [] styleString;
    delete [] string;
}

/*
** Returns the highlight style of the character at a given position of a
** window. To avoid breaking encapsulation, the highlight style is converted
** to a void* pointer (no other module has to know that characters are used
** to represent highlight styles; that would complicate future extensions).
** Returns NULL if the window has highlighting turned off.
** The only guarantee that this function offers, is that when the same
** pointer is returned for two positions, the corresponding characters have
** the same highlight style.
**/
void* SyntaxHighlighter::GetHighlightInfo(int pos)
{
    int style;
    HighlightDataRecord *pattern = nullptr;
    windowHighlightData *highlightData = highlightData_;

    if (!highlightData) {
        return nullptr;
    }

    /* Be careful with signed/unsigned conversions. NO conversion here! */
    style = (int)highlightData->styleBuffer->BufGetCharacter(pos);

    /* Beware of unparsed regions. */
    if (style == UNFINISHED_STYLE) {
        handleUnparsedRegion(highlightData->styleBuffer, pos);
        style = (int)highlightData->styleBuffer->BufGetCharacter(pos);
    }

    if (highlightData->pass1Patterns) {
       pattern = patternOfStyle(highlightData->pass1Patterns, style);
    }

	if (!pattern && highlightData->pass2Patterns) {
		pattern = patternOfStyle(highlightData->pass2Patterns, style);
	}

	if (!pattern) {
		return nullptr;
	}

	return reinterpret_cast<void *>(pattern->userStyleIndex);
}

void SyntaxHighlighter::handleUnparsedRegion(TextBuffer *styleBuffer, int pos) {
	HighlightEvent event;
	event.buffer = styleBuffer;
	event.pos = pos;

	unfinishedHighlightEncountered(&event);

}
