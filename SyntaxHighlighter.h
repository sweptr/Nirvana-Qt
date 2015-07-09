
#ifndef SYNTAX_HIGHLIGHTER_H
#define SYNTAX_HIGHLIGHTER_H

#include "regex/RegExp.h"
#include "IBufferModifiedHandler.h"
#include "IHighlightHandler.h"
#include "Types.h"
#include <QObject>
#include <QTextCharFormat>
#include <QString>
#include <QVector>
#include <QMap>
#include <QStringList>

/* Masks for text drawing methods.  These are or'd together to form an
   integer which describes what drawing calls to use to draw a string */
#define FILL_SHIFT 8
#define SECONDARY_SHIFT 9
#define PRIMARY_SHIFT 10
#define HIGHLIGHT_SHIFT 11
#define STYLE_LOOKUP_SHIFT 0
#define BACKLIGHT_SHIFT 12

#define FILL_MASK (1 << FILL_SHIFT)
#define SECONDARY_MASK (1 << SECONDARY_SHIFT)
#define PRIMARY_MASK (1 << PRIMARY_SHIFT)
#define HIGHLIGHT_MASK (1 << HIGHLIGHT_SHIFT)
#define STYLE_LOOKUP_MASK (0xff << STYLE_LOOKUP_SHIFT)
#define BACKLIGHT_MASK (0xff << BACKLIGHT_SHIFT)

#define RANGESET_SHIFT (20)
#define RANGESET_MASK (0x3F << RANGESET_SHIFT)

/* Don't use plain 'A' or 'B' for style indices, it causes problems
   with EBCDIC coding (possibly negative offsets when subtracting 'A'). */
#define ASCII_A ((char_type)65)

/* Meanings of style buffer characters (styles). Don't use plain 'A' or 'B';
   it causes problems with EBCDIC coding (possibly negative offsets when
   subtracting 'A'). */
#define UNFINISHED_STYLE ASCII_A
#define PLAIN_STYLE (ASCII_A + 1)
#define IS_PLAIN(style) (style == PLAIN_STYLE || style == UNFINISHED_STYLE)
#define IS_STYLED(style) (style != PLAIN_STYLE && style != UNFINISHED_STYLE)

/* Maximum allowed number of styles (also limited by representation of
   styles as a byte - 'b') */
#define MAX_HIGHLIGHT_STYLES 128

struct languageModeRec {
	QString name;
	QStringList extensions;
	QString recognitionExpr;
	QString defTipsFile;
	QString delimiters;
	int wrapStyle;
	int indentStyle;
	int tabDist;
	int emTabDist;
};

struct highlightStyleRec {
	QString name;
	QString color;
	QString bgColor;
    bool italic;
    bool bold;
	int font;
};

/* Pattern specification structure */
struct highlightPattern {
	QString name;
    QString startRE;
    QString endRE;
    QString errorRE;
	QString style;
	QString subPatternOf;
	int flags;
};

/* Header for a set of patterns */
struct patternSet {
	QString languageMode;
	int lineContext;
	int charContext;
	QVector<highlightPattern> patterns;
};

struct styleTableEntry {
	QString highlightName;
	QString styleName;
	QString colorName;
	bool isBold;
	bool isItalic;
	QColor color;
	bool underline;
	QFont font;
	QString bgColorName; /* background style coloring (name may be NULL) */
	QColor bgColor;
};

/* "Compiled" version of pattern specification */
struct highlightDataRec {
	RegExp *startRE;
	RegExp *endRE;
	RegExp *errorRE;
	RegExp *subPatternRE;
	char_type style;
	int colorOnly;
	signed char startSubexprs[NSUBEXP + 1];
	signed char endSubexprs[NSUBEXP + 1];
	int flags;
	int nSubPatterns;
	int nSubBranches; /* Number of top-level branches of subPatternRE */
	int userStyleIndex;
	struct highlightDataRec **subPatterns;
};

/* Context requirements for incremental reparsing of a pattern set */
struct reparseContext {
	int nLines;
	int nChars;
};

/* Data structure attached to window to hold all syntax highlighting
   information (for both drawing and incremental reparsing) */
struct windowHighlightData {
	highlightDataRec *pass1Patterns;
	highlightDataRec *pass2Patterns;
	char_type *parentStyles;
	reparseContext contextRequirements;
	styleTableEntry *styleTable;
	int nStyles;
	TextBuffer *styleBuffer;
	patternSet *patternSetForWindow;
};

class SyntaxHighlighter : public QObject, public IBufferModifiedHandler, public IHighlightHandler {
	Q_OBJECT
public:
	SyntaxHighlighter();
	virtual ~SyntaxHighlighter();

public:
	virtual void bufferModified(const ModifyEvent *event) override;
    virtual void unfinishedHighlightEncountered(const HighlightEvent *event) override;

public:
	TextBuffer *styleBuffer() const;
	styleTableEntry *styleEntry(int index) const;
	void* GetHighlightInfo(int pos);

private:
	QFont FontOfNamedStyle(const QString &styleName);
	QString LanguageModeName(int mode);
	RegExp *compileREAndWarn(const QString &re);
	bool FontOfNamedStyleIsBold(const QString &styleName);
	bool FontOfNamedStyleIsItalic(const QString &styleName);
	bool NamedStyleExists(const QString &styleName);
	bool parseString(highlightDataRec *pattern, const char_type **string, char_type **styleString, int length, char_type *prevChar, bool anchored, const char_type *delimiters, const char_type *lookBehindTo, const char_type *match_till);
	char_type getPrevChar(TextBuffer *buf, int pos);
	QString BgColorOfNamedStyle(const QString &styleName);
	QString ColorOfNamedStyle(const QString &styleName) const;
	highlightDataRec *compilePatterns(highlightPattern *patternSrc, int nPatterns);
	highlightDataRec *patternOfStyle(highlightDataRec *patterns, int style) const;
	int IndexOfNamedStyle(const QString &styleName) const;
	int backwardOneContext(TextBuffer *buf, reparseContext *context, int fromPos);
	int findSafeParseRestartPos(TextBuffer *buf, windowHighlightData *highlightData, int *pos);
	int findTopLevelParentIndex(highlightPattern *patList, int nPats, int index) const;
	int findTopLevelParentIndex(const QVector<highlightPattern> &patList, int nPats, int index) const;
	int forwardOneContext(TextBuffer *buf, reparseContext *context, int fromPos);
	int indexOfNamedPattern(highlightPattern *patList, int nPats, const QString &patName) const;
	int indexOfNamedPattern(const QVector<highlightPattern> &patList, int nPats, const QString &patName) const;
	bool isParentStyle(const char_type *parentStyles, int style1, int style2);
	int lastModified(TextBuffer *styleBuf) const;
	int lookupNamedStyle(const QString &styleName) const;
	int parentStyleOf(const char_type *parentStyles, int style);
	int parseBufferRange(highlightDataRec *pass1Patterns, highlightDataRec *pass2Patterns, TextBuffer *buf, TextBuffer *styleBuf, reparseContext *contextRequirements, int beginParse, int endParse, const char_type *delimiters);
	int patternIsParsable(highlightDataRec *pattern);
	patternSet *FindPatternSet(const QString &langModeName);
	patternSet *findPatternsForWindow(bool warn);
	void fillStyleString(const char_type **stringPtr, char_type **stylePtr, const char_type *toPtr, char_type style, char_type *prevChar);
	void incrementalReparse(windowHighlightData *highlightData, TextBuffer *buf, int pos, int nInserted,
	                        const char_type *delimiters);
	void modifyStyleBuf(TextBuffer *styleBuf, char_type *styleString, int startPos, int endPos, int firstPass2Style);
	void passTwoParseString(highlightDataRec *pattern, char_type *string, char_type *styleString, int length, char_type *prevChar,
	                        const char_type *delimiters, const char_type *lookBehindTo, const char_type *match_till);
	void recolorSubexpr(RegExp *re, int subexpr, int style, const char_type *string, char_type *styleString);
	windowHighlightData *createHighlightData(patternSet *patSet);
	void handleUnparsedRegion(TextBuffer *styleBuffer, int pos);

private:
	void loadStyles(const QString &filename);
	void loadLanguages(const QString &filename);

private:
	windowHighlightData *highlightData_;

	/* Pattern sources loaded from the .nedit file or set by the user */
	QVector<patternSet *> PatternSets;

	/* list of available language modes and language specific preferences */
	QVector<languageModeRec *> LanguageModes;

	/* list of available highlight styles */
	QVector<highlightStyleRec *> HighlightStyles;
};

#endif
