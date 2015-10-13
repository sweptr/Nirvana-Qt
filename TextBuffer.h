
#ifndef TEXT_BUFFER_H_
#define TEXT_BUFFER_H_

#include "Types.h"
#include "Selection.h"
#include <deque>
#include <string>

class IBufferModifiedHandler;
class IPreDeleteHandler;

/* Maximum length in characters of a tab or control character expansion
   of a single buffer character */
#define MAX_EXP_CHAR_LEN 20

// class RangesetTable;

class String {
public:
	String() : str(nullptr), len(0) {
	}
	
	// NOTE: TAKES OWNERSHIP OF STRING
	String(char_type *string, int length) : str(string), len(length) {
	}
	
	String(String &&other) : str(other.str), len(other.len) {
		other.str = nullptr;
		other.len = 0;
	}
	
	String& operator=(String &&rhs) {
		String(std::move(rhs)).swap(*this);
		return *this;
	}
	
	String(const String &) = delete;
	String& operator=(const String& ) = delete;
	
	~String() {
		delete [] str;
	}
	
public:
	char_type operator[](size_t index) const {
		return str[index];
	}
	
	char_type& operator[](size_t index) {
		return str[index];
	}
	
	char_type operator*() const {
		return *str;
	}

	char_type& operator*() {
		return *str;
	}
	
	void swap(String &other) {
		using std::swap;
		swap(str, other.str);
		swap(len, other.len);
	}
	
public:
	char_type *str;
	int        len;
};

class TextBuffer {
public:
	TextBuffer();
	explicit TextBuffer(int requestedSize);
	~TextBuffer();

private:
	TextBuffer(const TextBuffer &) = delete;
	TextBuffer &operator=(const TextBuffer &) = delete;

public:
	static int BufExpandCharacter(char_type c, int indent, char_type *outStr, int tabDist, char_type nullSubsChar);
	static int BufCharWidth(char_type c, int indent, int tabDist, char_type nullSubsChar);

public:
	Selection &BufGetHighlight();
	Selection &BufGetPrimarySelection();
	Selection &BufGetSecondarySelection();
	bool BufGetEmptySelectionPos(int *start, int *end, bool *isRect, int *rectStart, int *rectEnd) const;
	bool BufGetHighlightPos(int *start, int *end, bool *isRect, int *rectStart, int *rectEnd) const;
	bool BufGetSecSelectPos(int *start, int *end, bool *isRect, int *rectStart, int *rectEnd) const;
	bool BufGetSelectionPos(int *start, int *end, bool *isRect, int *rectStart, int *rectEnd) const;
	bool BufGetUseTabs() const;
	bool BufSearchBackward(int startPos, const char_type *searchChars, int *foundPos) const;
	bool BufSearchForward(int startPos, const char_type *searchChars, int *foundPos) const;
	bool BufSubstituteNullChars(char_type *string, int length);
	String BufGetAll() const;
	String BufGetRange(int start, int end) const;
	String BufGetSecSelectText() const;
	String BufGetSelectionText() const;
	String BufGetTextInRect(int start, int end, int rectStart, int rectEnd) const;
	char_type BufGetCharacter(int pos) const;
	char_type BufGetNullSubsChar() const;
	const char_type *BufAsString();
	int BufCmp(int pos, int len, const char_type *cmpText) const;
	int BufCountBackwardNLines(int startPos, int nLines) const;
	int BufCountDispChars(int lineStartPos, int targetPos) const;
	int BufCountForwardDispChars(int lineStartPos, int nChars) const;
	int BufCountForwardNLines(int startPos, unsigned nLines) const;
	int BufCountLines(int startPos, int endPos) const;
	int BufEndOfLine(int pos) const;
	int BufGetCursorPosHint() const;
	int BufGetExpandedChar(int pos, int indent, char_type *outStr) const;
	int BufGetLength() const;
	int BufGetTabDistance() const;
	int BufStartOfLine(int pos) const;
	void BufAddHighPriorityModifyCB(IBufferModifiedHandler *handler);
	void BufAddModifyCB(IBufferModifiedHandler *handler);
	void BufAddPreDeleteCB(IPreDeleteHandler *handler);
	void BufCheckDisplay(int start, int end);
	void BufClearRect(int start, int end, int rectStart, int rectEnd);
	void BufCopyFromBuf(TextBuffer *toBuf, int fromStart, int fromEnd, int toPos);
	void BufHighlight(int start, int end);
	void BufInsert(int pos, const char_type *text);
	void BufInsert(int pos, const char_type *text, int length);
	void BufInsertCol(int column, int startPos, const char_type *text, int *charsInserted, int *charsDeleted);
	void BufOverlayRect(int startPos, int rectStart, int rectEnd, const char_type *text, int *charsInserted, int *charsDeleted);
	void BufRectHighlight(int start, int end, int rectStart, int rectEnd);
	void BufRectSelect(int start, int end, int rectStart, int rectEnd);
	void BufRemove(int start, int end);
	void BufRemoveModifyCB(IBufferModifiedHandler *handler);
	void BufRemovePreDeleteCB(IPreDeleteHandler *handler);
	void BufRemoveRect(int start, int end, int rectStart, int rectEnd);
	void BufRemoveSecSelect();
	void BufRemoveSelected();
	void BufReplace(int start, int end, const char_type *text);
	void BufReplace(int start, int end, const char_type *text, int length);
	void BufReplaceRect(int start, int end, int rectStart, int rectEnd, const char_type *text);
	void BufReplaceRect(int start, int end, int rectStart, int rectEnd, const char_type *text, int length);
	void BufReplaceSecSelect(const char_type *text);
	void BufReplaceSelected(const char_type *text);
	void BufSecRectSelect(int start, int end, int rectStart, int rectEnd);
	void BufSecondarySelect(int start, int end);
	void BufSecondaryUnselect();
	void BufSelect(int start, int end);
	void BufSetAll(const char_type *text);
	void BufSetAll(const char_type *text, int length);
	void BufSetCharacter(int pos, char_type ch);
	void BufSetTabDistance(int tabDist);
	void BufSetUseTabs(bool value);
	void BufUnhighlight();
	void BufUnselect();
	void BufUnsubstituteNullChars(char_type *string) const;


private:
	bool searchBackward(int startPos, char_type searchChar, int *foundPos) const;
	bool searchForward(int startPos, char_type searchChar, int *foundPos) const;
	String getSelectionText(const Selection &sel) const;
	int insert(int pos, const char_type *text);
	int insert(int pos, const char_type *text, int length);
	void callModifyCBs(int pos, int nDeleted, int nInserted, int nRestyled, const char_type *deletedText);
	void callPreDeleteCBs(int pos, int nDeleted);
	void deleteRange(int start, int end);
	void deleteRect(int start, int end, int rectStart, int rectEnd, int *replaceLen, int *endPos);
	void findRectSelBoundariesForCopy(int lineStartPos, int rectStart, int rectEnd, int *selStart, int *selEnd) const;
	void insertCol(int column, int startPos, const char_type *insText, int *nDeleted, int *nInserted, int *endPos);
	void moveGap(int pos);
	void overlayRect(int startPos, int rectStart, int rectEnd, const char_type *insText, int *nDeleted, int *nInserted, int *endPos);
	void reallocateBuf(int newGapStart, int newGapLen);
	void redisplaySelection(const Selection &oldSelection, const Selection &newSelection);
	void removeSelected(const Selection &sel);
	void replaceSelected(Selection *sel, const char_type *text);
	void updateSelections(int pos, int nDeleted, int nInserted);

private:
	static String copyLine(const char_type *text, int *lineLen);
	static String expandTabs(const char_type *text, int startIndent, int tabDist, char_type nullSubsChar, int *newLen);
	static String realignTabs(const char_type *text, int origIndent, int newIndent, int tabDist, bool useTabs, char_type nullSubsChar, int *newLength);
	static String unexpandTabs(const char_type *text, int startIndent, int tabDist, char_type nullSubsChar, int *newLen);
	static char_type chooseNullSubsChar(char_type hist[256]);
	static int countLines(const char_type *string);
	static int countLines(const char_type *string, size_t length);
	static int textWidth(const char_type *text, int tabDist, char_type nullSubsChar);
	static void addPadding(char_type *string, int startIndent, int toIndent, int tabDist, bool useTabs, char_type nullSubsChar, int *charsAdded);
	static void deleteRectFromLine(const char_type *line, int rectStart, int rectEnd, int tabDist, bool useTabs, char_type nullSubsChar, char_type *outStr, int *outLen, int *endOffset);
	static void histogramCharacters(const char_type *string, int length, char_type hist[], bool init);
	static void insertColInLine(const char_type *line, const char_type *insLine, int column, int insWidth, int tabDist, bool useTabs, char_type nullSubsChar, char_type *outStr, int *outLen, int *endOffset);
	static void overlayRectInLine(const char_type *line, const char_type *insLine, int rectStart, int rectEnd, int tabDist, bool useTabs, char_type nullSubsChar, char_type *outStr, int *outLen, int *endOffset);
	static void subsChars(char_type *string, int length, char_type fromChar, char_type toChar);

private:
	// RangesetTable *rangesetTable_;             // current range sets
	Selection highlight_; // highlighted areas
	Selection primary_;
	Selection secondary_;
	bool useTabs_;                                     // True if buffer routines are allowed to use tabs for padding
	                                                   // in rectangular operations
	std::deque<IBufferModifiedHandler *> modifyProcs_; // procedures to call when
	                                                   // buffer is modified to
	                                                   // redisplay contents
	std::deque<IPreDeleteHandler *> preDeleteProcs_;   // procedures to call before
	                                                   // text is deleted from the
	                                                   // buffer; at most one is
	                                                   // supported.
	char_type *buf_;                                        // allocated memory where the text is stored
	char_type nullSubsChar_;                                // NEdit is based on C null-terminated strings, so
	                                                   // ascii-nul characters must be substituted with
	// something else.  This is the else, but of course, things get quite messy
	// when you use it
	int cursorPosHint_; // hint for reasonable cursor position after a buffer
	                    // modification operation
	int gapEnd_;        // points to the first character after the gap
	int gapStart_;      // points to the first character of the gap
	int length_;        // length of the text in the buffer (the length of the buffer
	                    // itself must be calculated: gapEnd -
	                    // gapStart + length)
	int tabDist_;       // equiv. number of characters in a tab
};

#endif
