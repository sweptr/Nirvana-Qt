
#ifndef SYNTAX_HIGHLIGHTER_H
#define SYNTAX_HIGHLIGHTER_H

#include "regex/Regex.h"
#include "IBufferModifiedHandler.h"
#include "IHighlightHandler.h"
#include "Types.h"
#include <QObject>
#include <QTextCharFormat>
#include <QString>
#include <QVector>
#include <QMap>
#include <QStringList>

/* Maximum allowed number of styles (also limited by representation of
   styles as a byte - 'b') */
#define MAX_HIGHLIGHT_STYLES 128

/* Masks for text drawing methods.  These are or'd together to form an
   integer which describes what drawing calls to use to draw a string */
#define STYLE_LOOKUP_SHIFT 0
#define FILL_SHIFT         8
#define SECONDARY_SHIFT    9
#define PRIMARY_SHIFT      10
#define HIGHLIGHT_SHIFT    11
#define BACKLIGHT_SHIFT    12
#define RANGESET_SHIFT     20

#define STYLE_LOOKUP_MASK (0xff << STYLE_LOOKUP_SHIFT)
#define FILL_MASK         (1 << FILL_SHIFT)
#define SECONDARY_MASK    (1 << SECONDARY_SHIFT)
#define PRIMARY_MASK      (1 << PRIMARY_SHIFT)
#define HIGHLIGHT_MASK    (1 << HIGHLIGHT_SHIFT)
#define BACKLIGHT_MASK    (0xff << BACKLIGHT_SHIFT)
#define RANGESET_MASK     (0x3F << RANGESET_SHIFT)

/* Don't use plain 'A' or 'B' for style indices, it causes problems
   with EBCDIC coding (possibly negative offsets when subtracting 'A'). */
#define ASCII_A ((char_type)65)

/* Meanings of style buffer characters (styles). Don't use plain 'A' or 'B';
   it causes problems with EBCDIC coding (possibly negative offsets when
   subtracting 'A'). */
#define UNFINISHED_STYLE static_cast<char_type>(ASCII_A)
#define PLAIN_STYLE      static_cast<char_type>(ASCII_A + 1)

struct LanguageModeRec;
struct HighlightStyleRec;
struct HighlightData;
struct HighlightPattern;
struct PatternSet;
struct StyleTableEntry;
struct HighlightDataRecord;

struct StyleTableEntry {
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

/* Context requirements for incremental reparsing of a pattern set */
struct ReparseContext {
	int nLines;
	int nChars;
};

enum MatchFlags {
	FlagNone     = 0x00,
	FlagAnchored = 0x01,
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
	StyleTableEntry *styleEntry(int index) const;
	void* GetHighlightInfo(int pos);

private:
	QFont FontOfNamedStyle(const QString &styleName);
	QString LanguageModeName(int mode);
	Regex *compileREAndWarn(const QString &re);
	bool FontOfNamedStyleIsBold(const QString &styleName);
	bool FontOfNamedStyleIsItalic(const QString &styleName);
	bool NamedStyleExists(const QString &styleName);
	bool parseString(const HighlightDataRecord *pattern, const char_type **string, char_type **styleString, int length, char_type *prevChar, MatchFlags flags, const char_type *delimiters, const char_type *lookBehindTo, const char_type *match_till);
	QString BgColorOfNamedStyle(const QString &styleName);
	QString ColorOfNamedStyle(const QString &styleName) const;
	HighlightDataRecord *compilePatterns(HighlightPattern *patternSrc, int nPatterns);
	static HighlightDataRecord *patternOfStyle(HighlightDataRecord *patterns, int style);
	int IndexOfNamedStyle(const QString &styleName) const;
	int backwardOneContext(TextBuffer *buf, ReparseContext *context, int fromPos);
	int findSafeParseRestartPos(TextBuffer *buf, HighlightData *highlightData, int *pos);
	int findTopLevelParentIndex(const QVector<HighlightPattern> &patList, int nPats, int index) const;
	int forwardOneContext(TextBuffer *buf, ReparseContext *context, int fromPos);
	int indexOfNamedPattern(HighlightPattern *patList, int nPats, const QString &patName) const;
	int indexOfNamedPattern(const QVector<HighlightPattern> &patList, int nPats, const QString &patName) const;
	bool isParentStyle(const char_type *parentStyles, int style1, int style2);
	int lastModified(TextBuffer *styleBuf) const;
	HighlightStyleRec *lookupNamedStyle(const QString &styleName) const;
	int parentStyleOf(const char_type *parentStyles, int style);
	int parseBufferRange(const HighlightDataRecord *pass1Patterns, const HighlightDataRecord *pass2Patterns, TextBuffer *buf, TextBuffer *styleBuf, ReparseContext *contextRequirements, int beginParse, int endParse, const char_type *delimiters);
	int patternIsParsable(const HighlightDataRecord *pattern);
	PatternSet *FindPatternSet(const QString &langModeName);
	PatternSet *findPatternsForWindow(bool warn);
	void fillStyleString(const char_type *&stringPtr, char_type *&stylePtr, const char_type *toPtr, char_type style, char_type *prevChar);
	void incrementalReparse(HighlightData *highlightData, TextBuffer *buf, int pos, int nInserted, const char_type *delimiters);
	void modifyStyleBuf(TextBuffer *styleBuf, char_type *styleString, int startPos, int endPos, int firstPass2Style);
	void passTwoParseString(const HighlightDataRecord *pattern, char_type *string, char_type *styleString, int length, char_type *prevChar, const char_type *delimiters, const char_type *lookBehindTo, const char_type *match_till);
	void recolorSubexpr(Regex *re, int subexpr, int style, const char_type *string, char_type *styleString);
	HighlightData *createHighlightData(PatternSet *patSet);
	void handleUnparsedRegion(TextBuffer *styleBuffer, int pos);

private:
	void loadStyles(const QString &filename);
	void loadLanguages(const QString &filename);

private:
	HighlightData *highlightData_;

	/* Pattern sources loaded from the .nedit file or set by the user */
	QMap<QString, PatternSet *> patternSets_;

	/* list of available language modes and language specific preferences */
	QVector<LanguageModeRec *> languageModes_;

	/* list of available highlight styles */
	QVector<HighlightStyleRec *> highlightStyles_;
};

#endif
