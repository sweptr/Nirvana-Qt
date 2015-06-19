
#include "TextBuffer.h"
#include "IBufferModifiedHandler.h"
#include "IPreDeleteHandler.h"
//#include "Rangeset.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <memory>
#include <cassert>

/* Initial size for the buffer gap (empty space in the buffer where text might
 * be inserted if the user is typing sequential chars) */
#define PREFERRED_GAP_SIZE 80
//#define USE_MEMCPY
//#define USE_STRCPY
//#define PURIFY

namespace {

void setSelection(Selection *sel, int start, int end) {
	sel->selected = start != end;
	sel->zeroWidth = start == end;
	sel->rectangular = false;
	sel->start = std::min(start, end);
	sel->end = std::max(start, end);
}

void setRectSelect(Selection *sel, int start, int end, int rectStart, int rectEnd) {
	sel->selected = rectStart < rectEnd;
	sel->zeroWidth = rectStart == rectEnd;
	sel->rectangular = true;
	sel->start = start;
	sel->end = end;
	sel->rectStart = rectStart;
	sel->rectEnd = rectEnd;
}

bool getSelectionPos(const Selection &sel, int *start, int *end, bool *isRect, int *rectStart, int *rectEnd) {
	/* Always fill in the parameters (zero-width can be requested too). */
	*isRect = sel.rectangular;
	*start = sel.start;
	*end = sel.end;

	if (sel.rectangular) {
		*rectStart = sel.rectStart;
		*rectEnd = sel.rectEnd;
	}

	return sel.selected;
}

/*
** Update an individual Selection for changes in the corresponding text
*/
void updateSelection(Selection *sel, int pos, int nDeleted, int nInserted) {
	if ((!sel->selected && !sel->zeroWidth) || pos > sel->end) {
		return;
	}

	if (pos + nDeleted <= sel->start) {
		sel->start += nInserted - nDeleted;
		sel->end += nInserted - nDeleted;
	} else if (pos <= sel->start && pos + nDeleted >= sel->end) {
		sel->start = pos;
		sel->end = pos;
		sel->selected = false;
		sel->zeroWidth = false;
	} else if (pos <= sel->start && pos + nDeleted < sel->end) {
		sel->start = pos;
		sel->end = nInserted + sel->end - nDeleted;
	} else if (pos < sel->end) {
		sel->end += nInserted - nDeleted;
		if (sel->end <= sel->start)
			sel->selected = false;
	}
}

#ifdef __MVS__
const char_type *ControlCodeTable[64] = {
    "nul", "soh", "stx", "etx", "sel", "ht",  "rnl", "del", "ge",  "sps", "rpt", "vt",  "ff",  "cr",  "so",  "si",
    "dle", "dc1", "dc2", "dc3", "res", "nl",  "bs",  "poc", "can", "em",  "ubs", "cu1", "ifs", "igs", "irs", "ius",
    "ds",  "sos", "fs",  "wus", "byp", "lf",  "etb", "esc", "sa",  "sfe", "sm",  "csp", "mfa", "enq", "ack", "bel",
    "x30", "x31", "syn", "ir",  "pp",  "trn", "nbs", "eot", "sbs", "it",  "rff", "cu3", "dc4", "nak", "x3e", "sub"
};
#else
const char_type *ControlCodeTable[32] = {
    "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs",  "ht",  "nl",
    "vt",  "np",  "cr",  "so",  "si",  "dle", "dc1", "dc2", "dc3", "dc4", "nak",
    "syn", "etb", "can", "em",  "sub", "esc", "fs",  "gs",  "rs",  "us"
};
#endif
}

/*
** Create an empty text buffer
*/
TextBuffer::TextBuffer() : TextBuffer(0) {
}

/*
** Create an empty text buffer of a pre-determined size (use this to
** avoid unnecessary re-allocation if you know exactly how much the buffer
** will need to hold
*/
TextBuffer::TextBuffer(int requestedSize) {
	length_ = 0;
	buf_ = new char_type[requestedSize + PREFERRED_GAP_SIZE + 1];
	buf_[requestedSize + PREFERRED_GAP_SIZE] = '\0';

	gapStart_ = 0;
	gapEnd_ = PREFERRED_GAP_SIZE;
	tabDist_ = 4;
	useTabs_ = true;
	nullSubsChar_ = '\0';
	//    rangesetTable_   = nullptr;
	cursorPosHint_ = 0;

#ifdef PURIFY
	std::fill_n(&buf_[gapStart_], gapEnd_ - gapStart_, '.');
#endif
}

/*
** Free a text buffer
*/
TextBuffer::~TextBuffer() {

	delete[] buf_;
	//	delete rangesetTable_;
}

/*
** Get the entire contents of a text buffer.  Memory is allocated to contain
** the returned string, which the caller must delete[].
*/
char_type *TextBuffer::BufGetAll() const {
	auto text = new char_type[length_ + 1];
#ifdef USE_MEMCPY
	memcpy(&text[0], buf_, gapStart_);
	memcpy(&text[gapStart_], &buf_[gapEnd_], length_ - gapStart_);
#else
	std::copy_n(buf_, gapStart_, &text[0]);
	std::copy_n(&buf_[gapEnd_], length_ - gapStart_, &text[gapStart_]);
#endif
	text[length_] = '\0';
	return text;
}

/*
** Get the entire contents of a text buffer as a single string.  The gap is
** moved so that the buffer data can be accessed as a single contiguous
** character array.
** NB DO NOT ALTER THE TEXT THROUGH THE RETURNED POINTER!
** (we make an exception in BufSubstituteNullChars() however)
** This function is intended ONLY to provide a searchable string without copying
** into a temporary buffer.
*/
const char_type *TextBuffer::BufAsString() {
	int bufLen = length_;
	int leftLen = gapStart_;
	int rightLen = bufLen - leftLen;

	/* find where best to put the gap to minimise memory movement */
	if (leftLen != 0 && rightLen != 0) {
		leftLen = (leftLen < rightLen) ? 0 : bufLen;
		moveGap(leftLen);
	}

	/* get the start position of the actual data */
	char_type *text = &buf_[(leftLen == 0) ? gapEnd_ : 0];

	/* make sure it's null-terminated */
	text[bufLen] = '\0';

	return text;
}

/*
** Replace the entire contents of the text buffer
*/
void TextBuffer::BufSetAll(const char_type *text) {
	const int length = static_cast<int>(traits_type::length(text));
	BufSetAll(text, length);
}

void TextBuffer::BufSetAll(const char_type *text, int length) {

	callPreDeleteCBs(0, length_);

	/* Save information for redisplay, and get rid of the old buffer */
	char_type *deletedText = BufGetAll();
	int deletedLength = length_;
	delete[] buf_;

	/* Start a new buffer with a gap of PREFERRED_GAP_SIZE in the center */
	buf_ = new char_type[length + PREFERRED_GAP_SIZE + 1];
	buf_[length + PREFERRED_GAP_SIZE] = '\0';
	length_ = length;
	gapStart_ = length / 2;
	gapEnd_ = gapStart_ + PREFERRED_GAP_SIZE;
#ifdef USE_MEMCPY
	memcpy(buf_, text, gapStart_);
	memcpy(&buf_[gapEnd_], &text[gapStart_], length - gapStart_);
#else
	std::copy_n(text, gapStart_, buf_);
	std::copy_n(&text[gapStart_], length - gapStart_, &buf_[gapEnd_]);
#endif
#ifdef PURIFY
	std::fill_n(&buf_[gapStart_], gapEnd_ - gapStart_, '.');
#endif

	/* Zero all of the existing selections */
	updateSelections(0, deletedLength, 0);

	/* Call the saved display routine(s) to update the screen */
	callModifyCBs(0, deletedLength, length, 0, deletedText);
	delete[] deletedText;
}

/*
** Return a copy of the text between "start" and "end" character positions
** from text buffer "buf".  Positions start at 0, and the range does not
** include the character pointed to by "end"
*/
char_type *TextBuffer::BufGetRange(int start, int end) const {
	int length;
	int part1Length;

	/* Make sure start and end are ok, and allocate memory for returned string.
	   If start is bad, return "", if end is bad, adjust it. */
	if (start < 0 || start > length_) {
		auto text = new char_type[1];
		text[0] = '\0';
		return text;
	}

	if (end < start) {
		int temp = start;
		start = end;
		end = temp;
	}

	if (end > length_) {
		end = length_;
	}

	length = end - start;
	auto text = new char_type[length + 1];

	/* Copy the text from the buffer to the returned string */
#ifdef USE_MEMCPY
	if (end <= gapStart_) {
		memcpy(text, &buf_[start], length);
	} else if (start >= gapStart_) {
		memcpy(text, &buf_[start + (gapEnd_ - gapStart_)], length);
	} else {
		part1Length = gapStart_ - start;
		memcpy(text, &buf_[start], part1Length);
		memcpy(&text[part1Length], &buf_[gapEnd_], length - part1Length);
	}
#else
	if (end <= gapStart_) {
		std::copy_n(&buf_[start], length, text);
	} else if (start >= gapStart_) {
		std::copy_n(&buf_[start + (gapEnd_ - gapStart_)], length, text);
	} else {
		part1Length = gapStart_ - start;
		std::copy_n(&buf_[start], part1Length, text);
		std::copy_n(&buf_[gapEnd_], length - part1Length, &text[part1Length]);
	}
#endif
	text[length] = '\0';
	return text;
}

/*
** Return the character at buffer position "pos".  Positions start at 0.
*/
char_type TextBuffer::BufGetCharacter(int pos) const {
	if (pos < 0 || pos >= length_) {
		return '\0';
	}

	if (pos < gapStart_) {
		return buf_[pos];
	} else {
		return buf_[pos + gapEnd_ - gapStart_];
	}
}

void TextBuffer::BufSetCharacter(int pos, char_type ch) {
	if (pos < 0 || pos >= length_) {
		return;
	}

	if (pos < gapStart_) {
		buf_[pos] = ch;
	} else {
		buf_[pos + gapEnd_ - gapStart_] = ch;
	}
}

/*
** Insert null-terminated string "text" at position "pos" in "buf"
*/
void TextBuffer::BufInsert(int pos, const char_type *text) {
	int length = static_cast<int>(traits_type::length(text));
	BufInsert(pos, text, length);
}

/*
** Insert null-terminated string "text" at position "pos" in "buf"
*/
void TextBuffer::BufInsert(int pos, const char_type *text, int length) {
	int nInserted;

	/* if pos is not contiguous to existing text, make it */
	if (pos > length_)
		pos = length_;
	if (pos < 0)
		pos = 0;

	/* Even if nothing is deleted, we must call these callbacks */
	callPreDeleteCBs(pos, 0);

	/* insert and redisplay */
	nInserted = insert(pos, text, length);
	cursorPosHint_ = pos + nInserted;
	callModifyCBs(pos, 0, nInserted, 0, nullptr);
}

/*
** Delete the characters between "start" and "end", and insert the
** null-terminated string "text" in their place in in "buf"
*/
void TextBuffer::BufReplace(int start, int end, const char_type *text) {
	const int length = static_cast<int>(traits_type::length(text));
	BufReplace(start, end, text, length);
}

/*
** Delete the characters between "start" and "end", and insert the
** null-terminated string "text" in their place in in "buf"
*/
void TextBuffer::BufReplace(int start, int end, const char_type *text, int length) {
	char_type *deletedText;
	int nInserted = length;

	callPreDeleteCBs(start, end - start);
	deletedText = BufGetRange(start, end);
	deleteRange(start, end);
	insert(start, text, nInserted);
	cursorPosHint_ = start + nInserted;
	callModifyCBs(start, end - start, nInserted, 0, deletedText);
	delete[] deletedText;
}

void TextBuffer::BufRemove(int start, int end) {
	char_type *deletedText;

	/* Make sure the arguments make sense */
	if (start > end) {
		int temp = start;
		start = end;
		end = temp;
	}
	if (start > length_)
		start = length_;
	if (start < 0)
		start = 0;
	if (end > length_)
		end = length_;
	if (end < 0)
		end = 0;

	callPreDeleteCBs(start, end - start);
	/* Remove and redisplay */
	deletedText = BufGetRange(start, end);
	deleteRange(start, end);
	cursorPosHint_ = start;
	callModifyCBs(start, end - start, 0, 0, deletedText);
	delete[] deletedText;
}

void TextBuffer::BufCopyFromBuf(TextBuffer *toBuf, int fromStart, int fromEnd, int toPos) {
	const int length = fromEnd - fromStart;
	int part1Length;

	/* Prepare the buffer to receive the new text.  If the new text fits in
	   the current buffer, just move the gap (if necessary) to where
	   the text should be inserted.  If the new text is too large, reallocate
	   the buffer with a gap large enough to accomodate the new text and a
	   gap of PREFERRED_GAP_SIZE */
	if (length > toBuf->gapEnd_ - toBuf->gapStart_) {
		toBuf->reallocateBuf(toPos, length + PREFERRED_GAP_SIZE);
	} else if (toPos != toBuf->gapStart_) {
		toBuf->moveGap(toPos);
	}

	/* Insert the new text (toPos now corresponds to the start of the gap) */
#ifdef USE_MEMCPY
	if (fromEnd <= gapStart_) {
		memcpy(&toBuf->buf_[toPos], &buf_[fromStart], length);
	} else if (fromStart >= gapStart_) {
		memcpy(&toBuf->buf_[toPos], &buf_[fromStart + (gapEnd_ - gapStart_)], length);
	} else {
		part1Length = gapStart_ - fromStart;
		memcpy(&toBuf->buf_[toPos], &buf_[fromStart], part1Length);
		memcpy(&toBuf->buf_[toPos + part1Length], &buf_[gapEnd_], length - part1Length);
	}
#else
	if (fromEnd <= gapStart_) {
		std::copy_n(&buf_[fromStart], length, &toBuf->buf_[toPos]);
	} else if (fromStart >= gapStart_) {
		std::copy_n(&buf_[fromStart + (gapEnd_ - gapStart_)], length, &toBuf->buf_[toPos]);
	} else {
		part1Length = gapStart_ - fromStart;
		std::copy_n(&buf_[fromStart], part1Length, &toBuf->buf_[toPos]);
		std::copy_n(&buf_[gapEnd_], length - part1Length, &toBuf->buf_[toPos + part1Length]);
	}
#endif
	toBuf->gapStart_ += length;
	toBuf->length_ += length;
	toBuf->updateSelections(toPos, 0, length);
}

/*
** Insert "text" columnwise into buffer starting at displayed character
** position "column" on the line beginning at "startPos".  Opens a rectangular
** space the width and height of "text", by moving all text to the right of
** "column" right.  If charsInserted and charsDeleted are not nullptr, the
** number of characters inserted and deleted in the operation (beginning
** at startPos) are returned in these arguments
*/
void TextBuffer::BufInsertCol(int column, int startPos, const char_type *text, int *charsInserted, int *charsDeleted) {
	int nLines, lineStartPos, nDeleted, insertDeleted, nInserted;
	char_type *deletedText;

	nLines = countLines(text);
	lineStartPos = BufStartOfLine(startPos);
	nDeleted = BufEndOfLine(BufCountForwardNLines(startPos, nLines)) - lineStartPos;
	callPreDeleteCBs(lineStartPos, nDeleted);
	deletedText = BufGetRange(lineStartPos, lineStartPos + nDeleted);
	insertCol(column, lineStartPos, text, &insertDeleted, &nInserted, &cursorPosHint_);

	assert(nDeleted == insertDeleted && "Internal consistency check ins1 failed");

	callModifyCBs(lineStartPos, nDeleted, nInserted, 0, deletedText);
	delete[] deletedText;
	if (charsInserted != nullptr)
		*charsInserted = nInserted;
	if (charsDeleted != nullptr)
		*charsDeleted = nDeleted;
}

/*
** Overlay "text" between displayed character positions "rectStart" and
** "rectEnd" on the line beginning at "startPos".  If charsInserted and
** charsDeleted are not nullptr, the number of characters inserted and deleted
** in the operation (beginning at startPos) are returned in these arguments.
** If rectEnd equals -1, the width of the inserted text is measured first.
*/
void TextBuffer::BufOverlayRect(int startPos, int rectStart, int rectEnd, const char_type *text, int *charsInserted, int *charsDeleted) {
	int nLines, lineStartPos, nDeleted, insertDeleted, nInserted;
	char_type *deletedText;

	nLines = countLines(text);
	lineStartPos = BufStartOfLine(startPos);
	if (rectEnd == -1)
		rectEnd = rectStart + textWidth(text, tabDist_, nullSubsChar_);
	lineStartPos = BufStartOfLine(startPos);
	nDeleted = BufEndOfLine(BufCountForwardNLines(startPos, nLines)) - lineStartPos;
	callPreDeleteCBs(lineStartPos, nDeleted);
	deletedText = BufGetRange(lineStartPos, lineStartPos + nDeleted);
	overlayRect(lineStartPos, rectStart, rectEnd, text, &insertDeleted, &nInserted, &cursorPosHint_);

	assert(nDeleted == insertDeleted && "Internal consistency check ovly1 failed");

	callModifyCBs(lineStartPos, nDeleted, nInserted, 0, deletedText);
	delete[] deletedText;
	if (charsInserted != nullptr)
		*charsInserted = nInserted;
	if (charsDeleted != nullptr)
		*charsDeleted = nDeleted;
}

/*
** Replace a rectangular area in buf, given by "start", "end", "rectStart",
** and "rectEnd", with "text".  If "text" is vertically longer than the
** rectangle, add extra lines to make room for it.
*/
void TextBuffer::BufReplaceRect(int start, int end, int rectStart, int rectEnd, const char_type *text, int length) {
	char_type *deletedText;
	char_type *insText = nullptr;
	int i, nInsertedLines, nDeletedLines, hint;
	int insertDeleted, insertInserted, deleteInserted;
	int linesPadded = 0;

	/* Make sure start and end refer to complete lines, since the
	   columnar delete and insert operations will replace whole lines */
	start = BufStartOfLine(start);
	end = BufEndOfLine(end);

	callPreDeleteCBs(start, end - start);

	/* If more lines will be deleted than inserted, pad the inserted text
	   with newlines to make it as long as the number of deleted lines.  This
	   will indent all of the text to the right of the rectangle to the same
	   column.  If more lines will be inserted than deleted, insert extra
	   lines in the buffer at the end of the rectangle to make room for the
	   additional lines in "text" */
	nInsertedLines = countLines(text, length);
	nDeletedLines = BufCountLines(start, end);
	if (nInsertedLines < nDeletedLines) {

		size_t insLen = traits_type::length(text);
		insText = new char_type[insLen + nDeletedLines - nInsertedLines + 1];
#ifdef USE_STRCPY
		strcpy(insText, text);
#else
		std::copy_n(text, insLen, insText);
#endif
		char_type *insPtr = insText + insLen;
		for (i = 0; i < nDeletedLines - nInsertedLines; i++) {
			*insPtr++ = '\n';
		}
		*insPtr = '\0';
	} else if (nDeletedLines < nInsertedLines) {
		linesPadded = nInsertedLines - nDeletedLines;
		for (i = 0; i < linesPadded; i++) {
			insert(end, "\n", 1);
		}
	} else /* nDeletedLines == nInsertedLines */ {
	}

	/* Save a copy of the text which will be modified for the modify CBs */
	deletedText = BufGetRange(start, end);

	/* Delete then insert */
	deleteRect(start, end, rectStart, rectEnd, &deleteInserted, &hint);
	if (insText) {
		insertCol(rectStart, start, insText, &insertDeleted, &insertInserted, &cursorPosHint_);
		delete[] insText;
	} else
		insertCol(rectStart, start, text, &insertDeleted, &insertInserted, &cursorPosHint_);

	/* Figure out how many chars were inserted and call modify callbacks */
	assert(insertDeleted == deleteInserted + linesPadded && "Internal consistency check repl1 failed\n");

	callModifyCBs(start, end - start, insertInserted, 0, deletedText);
	delete[] deletedText;
}

void TextBuffer::BufReplaceRect(int start, int end, int rectStart, int rectEnd, const char_type *text) {
	char_type *deletedText;
	char_type *insText = nullptr;
	int i, nInsertedLines, nDeletedLines, hint;
	int insertDeleted, insertInserted, deleteInserted;
	int linesPadded = 0;

	/* Make sure start and end refer to complete lines, since the
	   columnar delete and insert operations will replace whole lines */
	start = BufStartOfLine(start);
	end = BufEndOfLine(end);

	callPreDeleteCBs(start, end - start);

	/* If more lines will be deleted than inserted, pad the inserted text
	   with newlines to make it as long as the number of deleted lines.  This
	   will indent all of the text to the right of the rectangle to the same
	   column.  If more lines will be inserted than deleted, insert extra
	   lines in the buffer at the end of the rectangle to make room for the
	   additional lines in "text" */
	nInsertedLines = countLines(text);
	nDeletedLines = BufCountLines(start, end);
	if (nInsertedLines < nDeletedLines) {

		size_t insLen = traits_type::length(text);
		insText = new char_type[insLen + nDeletedLines - nInsertedLines + 1];
#ifdef USE_STRCPY
		strcpy(insText, text);
#else
		std::copy_n(text, insLen, insText);
#endif
		char_type *insPtr = insText + insLen;
		for (i = 0; i < nDeletedLines - nInsertedLines; i++) {
			*insPtr++ = '\n';
		}
		*insPtr = '\0';
	} else if (nDeletedLines < nInsertedLines) {
		linesPadded = nInsertedLines - nDeletedLines;
		for (i = 0; i < linesPadded; i++) {
			insert(end, "\n", 1);
		}
	} else /* nDeletedLines == nInsertedLines */ {
	}

	/* Save a copy of the text which will be modified for the modify CBs */
	deletedText = BufGetRange(start, end);

	/* Delete then insert */
	deleteRect(start, end, rectStart, rectEnd, &deleteInserted, &hint);
	if (insText) {
		insertCol(rectStart, start, insText, &insertDeleted, &insertInserted, &cursorPosHint_);
		delete[] insText;
	} else
		insertCol(rectStart, start, text, &insertDeleted, &insertInserted, &cursorPosHint_);

	/* Figure out how many chars were inserted and call modify callbacks */
	assert(insertDeleted == deleteInserted + linesPadded && "Internal consistency check repl1 failed\n");

	callModifyCBs(start, end - start, insertInserted, 0, deletedText);
	delete[] deletedText;
}

/*
** Remove a rectangular swath of characters between character positions start
** and end and horizontal displayed-character offsets rectStart and rectEnd.
*/
void TextBuffer::BufRemoveRect(int start, int end, int rectStart, int rectEnd) {
	char_type *deletedText;
	int nInserted;

	start = BufStartOfLine(start);
	end = BufEndOfLine(end);
	callPreDeleteCBs(start, end - start);
	deletedText = BufGetRange(start, end);
	deleteRect(start, end, rectStart, rectEnd, &nInserted, &cursorPosHint_);
	callModifyCBs(start, end - start, nInserted, 0, deletedText);
	delete[] deletedText;
}

/*
** Clear a rectangular "hole" out of the buffer between character positions
** start and end and horizontal displayed-character offsets rectStart and
** rectEnd.
*/
void TextBuffer::BufClearRect(int start, int end, int rectStart, int rectEnd) {
	int i;

	int nLines = BufCountLines(start, end);
	auto newlineString = new char_type[nLines + 1];

	for (i = 0; i < nLines; i++) {
		newlineString[i] = '\n';
	}

	newlineString[i] = '\0';
	BufOverlayRect(start, rectStart, rectEnd, newlineString, nullptr, nullptr);
	delete[] newlineString;
}

char_type *TextBuffer::BufGetTextInRect(int start, int end, int rectStart, int rectEnd) const {
	int selLeft;
	int selRight;
	int len;
	char_type *retabbedStr;

	start = BufStartOfLine(start);
	end = BufEndOfLine(end);

	auto textOut = new char_type[(end - start) + 1];
	int lineStart = start;
	char_type *outPtr = textOut;
	while (lineStart <= end) {
		findRectSelBoundariesForCopy(lineStart, rectStart, rectEnd, &selLeft, &selRight);
		char_type *const textIn = BufGetRange(selLeft, selRight);
		len = selRight - selLeft;
#ifdef USE_MEMCPY
		memcpy(outPtr, textIn, len);
#else
		std::copy_n(textIn, len, outPtr);
#endif
		delete[] textIn;
		outPtr += len;
		lineStart = BufEndOfLine(selRight) + 1;
		*outPtr++ = '\n';
	}

	if (outPtr != textOut) {
		outPtr--; /* don't leave trailing newline */
	}

	*outPtr = '\0';

	/* If necessary, realign the tabs in the Selection as if the text were
	   positioned at the left margin */
	retabbedStr = realignTabs(textOut, rectStart, 0, tabDist_, useTabs_, nullSubsChar_, &len);
	delete[] textOut;
	return retabbedStr;
}

/*
** Get the hardware tab distance used by all displays for this buffer,
** and used in computing offsets for rectangular Selection operations.
*/
int TextBuffer::BufGetTabDistance() const {
	return tabDist_;
}

/*
** Set the hardware tab distance used by all displays for this buffer,
** and used in computing offsets for rectangular Selection operations.
*/
void TextBuffer::BufSetTabDistance(int tabDist) {

	/* First call the pre-delete callbacks with the previous tab setting
	   still active. */
	callPreDeleteCBs(0, length_);

	/* Change the tab setting */
	tabDist_ = tabDist;

	/* Force any display routines to redisplay everything */
	const char_type *const deletedText = BufAsString();
	callModifyCBs(0, length_, length_, 0, deletedText);
}

void TextBuffer::BufCheckDisplay(int start, int end) {

	/* just to make sure colors in the selected region are up to date */
	callModifyCBs(start, 0, 0, end - start, nullptr);
}

void TextBuffer::BufSelect(int start, int end) {

	Selection oldSelection = primary_;

	setSelection(&primary_, start, end);
	redisplaySelection(oldSelection, primary_);
}

void TextBuffer::BufUnselect() {
	Selection oldSelection = primary_;

	primary_.selected = false;
	primary_.zeroWidth = false;
	redisplaySelection(oldSelection, primary_);
}

void TextBuffer::BufRectSelect(int start, int end, int rectStart, int rectEnd) {
	Selection oldSelection = primary_;

	setRectSelect(&primary_, start, end, rectStart, rectEnd);
	redisplaySelection(oldSelection, primary_);
}

bool TextBuffer::BufGetSelectionPos(int *start, int *end, bool *isRect, int *rectStart, int *rectEnd) const {
	return getSelectionPos(primary_, start, end, isRect, rectStart, rectEnd);
}

/* Same as above, but also returns true for empty selections */
bool TextBuffer::BufGetEmptySelectionPos(int *start, int *end, bool *isRect, int *rectStart, int *rectEnd) const {
	return getSelectionPos(primary_, start, end, isRect, rectStart, rectEnd) || primary_.zeroWidth;
}

char_type *TextBuffer::BufGetSelectionText() const {
	return getSelectionText(primary_);
}

void TextBuffer::BufRemoveSelected() {
	removeSelected(primary_);
}

void TextBuffer::BufReplaceSelected(const char_type *text) {
	replaceSelected(&primary_, text);
}

void TextBuffer::BufSecondarySelect(int start, int end) {
	Selection oldSelection = secondary_;

	setSelection(&secondary_, start, end);
	redisplaySelection(oldSelection, secondary_);
}

void TextBuffer::BufSecondaryUnselect() {
	Selection oldSelection = secondary_;

	secondary_.selected = false;
	secondary_.zeroWidth = false;
	redisplaySelection(oldSelection, secondary_);
}

void TextBuffer::BufSecRectSelect(int start, int end, int rectStart, int rectEnd) {
	Selection oldSelection = secondary_;

	setRectSelect(&secondary_, start, end, rectStart, rectEnd);
	redisplaySelection(oldSelection, secondary_);
}

bool TextBuffer::BufGetSecSelectPos(int *start, int *end, bool *isRect, int *rectStart, int *rectEnd) const {
	return getSelectionPos(secondary_, start, end, isRect, rectStart, rectEnd);
}

char_type *TextBuffer::BufGetSecSelectText() const {
	return getSelectionText(secondary_);
}

void TextBuffer::BufRemoveSecSelect() {
	removeSelected(secondary_);
}

void TextBuffer::BufReplaceSecSelect(const char_type *text) {
	replaceSelected(&secondary_, text);
}

void TextBuffer::BufHighlight(int start, int end) {
	Selection oldSelection = highlight_;

	setSelection(&highlight_, start, end);
	redisplaySelection(oldSelection, highlight_);
}

void TextBuffer::BufUnhighlight() {
	Selection oldSelection = highlight_;

	highlight_.selected = false;
	highlight_.zeroWidth = false;
	redisplaySelection(oldSelection, highlight_);
}

void TextBuffer::BufRectHighlight(int start, int end, int rectStart, int rectEnd) {
	Selection oldSelection = highlight_;

	setRectSelect(&highlight_, start, end, rectStart, rectEnd);
	redisplaySelection(oldSelection, highlight_);
}

bool TextBuffer::BufGetHighlightPos(int *start, int *end, bool *isRect, int *rectStart, int *rectEnd) const {
	return getSelectionPos(highlight_, start, end, isRect, rectStart, rectEnd);
}

/*
** Add a callback routine to be called when the buffer is modified
*/
void TextBuffer::BufAddModifyCB(IBufferModifiedHandler *handler) {
	modifyProcs_.emplace_back(handler);
}

/*
** Similar to the above, but makes sure that the callback is called before
** normal priority callbacks.
*/
void TextBuffer::BufAddHighPriorityModifyCB(IBufferModifiedHandler *handler) {
	modifyProcs_.emplace_front(handler);
}

void TextBuffer::BufRemoveModifyCB(IBufferModifiedHandler *handler) {

	auto it = std::find(modifyProcs_.begin(), modifyProcs_.end(), handler);
	if (it != modifyProcs_.end()) {
		modifyProcs_.erase(it);
	} else {
		fprintf(stderr, "Internal Error: Can't find modify CB to remove\n");
	}
}

/*
** Add a callback routine to be called before text is deleted from the buffer.
*/
void TextBuffer::BufAddPreDeleteCB(IPreDeleteHandler *handler) {
	preDeleteProcs_.emplace_back(handler);
}

void TextBuffer::BufRemovePreDeleteCB(IPreDeleteHandler *handler) {

	auto it = std::find(preDeleteProcs_.begin(), preDeleteProcs_.end(), handler);
	if (it != preDeleteProcs_.end()) {
		preDeleteProcs_.erase(it);
	} else {
		fprintf(stderr, "Internal Error: Can't find pre-delete CB to remove\n");
	}
}

/*
** Find the position of the start of the line containing position "pos"
*/
int TextBuffer::BufStartOfLine(int pos) const {
	int startPos;

	if (!searchBackward(pos, '\n', &startPos))
		return 0;
	return startPos + 1;
}

/*
** Find the position of the end of the line containing position "pos"
** (which is either a pointer to the newline character ending the line,
** or a pointer to one character beyond the end of the buffer)
*/
int TextBuffer::BufEndOfLine(int pos) const {
	int endPos;

	if (!searchForward(pos, '\n', &endPos))
		endPos = length_;
	return endPos;
}

/*
** Get a character from the text buffer expanded into it's screen
** representation (which may be several characters for a tab or a
** control code).  Returns the number of characters written to "outStr".
** "indent" is the number of characters from the start of the line
** for figuring tabs.  Output string is guranteed to be shorter or
** equal in length to MAX_EXP_CHAR_LEN
*/
int TextBuffer::BufGetExpandedChar(int pos, int indent, char_type *outStr) const {
	return BufExpandCharacter(BufGetCharacter(pos), indent, outStr, tabDist_, nullSubsChar_);
}

/*
** Expand a single character from the text buffer into it's screen
** representation (which may be several characters for a tab or a
** control code).  Returns the number of characters added to "outStr".
** "indent" is the number of characters from the start of the line
** for figuring tabs.  Output string is guranteed to be shorter or
** equal in length to MAX_EXP_CHAR_LEN
*/
int TextBuffer::BufExpandCharacter(char_type c, int indent, char_type *outStr, int tabDist, char_type nullSubsChar) {
	/* Convert tabs to spaces */
	if (c == '\t') {
		int nSpaces = tabDist - (indent % tabDist);
		for (int i = 0; i < nSpaces; i++) {
			outStr[i] = ' ';
		}
		return nSpaces;
	}

	/* Convert ASCII (and EBCDIC in the __MVS__ (OS/390) case) control
	   codes to readable character sequences */
	if (c == nullSubsChar) {
		return sprintf(outStr, "<nul>");
	}
#ifdef __MVS__
	if ((static_cast<uint8_t>(c)) <= 63) {
		return sprintf(outStr, "<%s>", ControlCodeTable[static_cast<uint8_t>(c)]);
	}
#else
	if ((static_cast<uint8_t>(c)) <= 31) {
		return sprintf(outStr, "<%s>", ControlCodeTable[static_cast<uint8_t>(c)]);
	} else if (c == 127) {
		return sprintf(outStr, "<del>");
	}
#endif

	/* Otherwise, just return the character */
	*outStr = c;
	return 1;
}

/*
** Return the length in displayed characters of character "c" expanded
** for display (as discussed above in BufGetExpandedChar).  If the
** buffer for which the character width is being measured is doing null
** substitution, nullSubsChar should be passed as that character (or nul
** to ignore).
*/
int TextBuffer::BufCharWidth(char_type c, int indent, int tabDist, char_type nullSubsChar) {
	/* Note, this code must parallel that in BufExpandCharacter */
	if (c == nullSubsChar)
		return 5;
	else if (c == '\t')
		return tabDist - (indent % tabDist);
	else if ((static_cast<uint8_t>(c)) <= 31)
		return static_cast<int>(traits_type::length(ControlCodeTable[static_cast<uint8_t>(c)])) + 2;
	else if (c == 127)
		return 5;
	return 1;
}

/*
** Count the number of displayed characters between buffer position
** "lineStartPos" and "targetPos". (displayed characters are the characters
** shown on the screen to represent characters in the buffer, where tabs and
** control characters are expanded)
*/
int TextBuffer::BufCountDispChars(int lineStartPos, int targetPos) const {
	int pos, charCount = 0;
	char_type expandedChar[MAX_EXP_CHAR_LEN];

	pos = lineStartPos;
	while (pos < targetPos && pos < length_)
		charCount += BufGetExpandedChar(pos++, charCount, expandedChar);
	return charCount;
}

/*
** Count forward from buffer position "startPos" in displayed characters
** (displayed characters are the characters shown on the screen to represent
** characters in the buffer, where tabs and control characters are expanded)
*/
int TextBuffer::BufCountForwardDispChars(int lineStartPos, int nChars) const {
	int charCount = 0;

	int pos = lineStartPos;
	while (charCount < nChars && pos < length_) {
		char_type c = BufGetCharacter(pos);
		if (c == '\n')
			return pos;
		charCount += BufCharWidth(c, charCount, tabDist_, nullSubsChar_);
		pos++;
	}
	return pos;
}

/*
** Count the number of newlines between startPos and endPos in buffer "buf".
** The character at position "endPos" is not counted.
*/
int TextBuffer::BufCountLines(int startPos, int endPos) const {
	const int gapLen = gapEnd_ - gapStart_;

	int lineCount = 0;

	int pos = startPos;

	while (pos < gapStart_) {
		if (pos == endPos)
			return lineCount;
		if (buf_[pos++] == '\n')
			lineCount++;
	}

	while (pos < length_) {
		if (pos == endPos)
			return lineCount;
		if (buf_[pos++ + gapLen] == '\n')
			lineCount++;
	}
	return lineCount;
}

/*
** Find the first character of the line "nLines" forward from "startPos"
** in "buf" and return its position
*/
int TextBuffer::BufCountForwardNLines(int startPos, unsigned nLines) const {
	int pos;
	int gapLen = gapEnd_ - gapStart_;
	unsigned int lineCount = 0;

	if (nLines == 0)
		return startPos;

	pos = startPos;
	while (pos < gapStart_) {
		if (buf_[pos++] == '\n') {
			lineCount++;
			if (lineCount == nLines)
				return pos;
		}
	}

	while (pos < length_) {
		if (buf_[pos++ + gapLen] == '\n') {
			lineCount++;
			if (lineCount >= nLines)
				return pos;
		}
	}
	return pos;
}

/*
** Find the position of the first character of the line "nLines" backwards
** from "startPos" (not counting the character pointed to by "startpos" if
** that is a newline) in "buf".  nLines == 0 means find the beginning of
** the line
*/
int TextBuffer::BufCountBackwardNLines(int startPos, int nLines) const {
	int pos;
	const int gapLen = gapEnd_ - gapStart_;
	int lineCount = -1;

	pos = startPos - 1;
	if (pos <= 0)
		return 0;

	while (pos >= gapStart_) {
		if (buf_[pos + gapLen] == '\n') {
			if (++lineCount >= nLines)
				return pos + 1;
		}
		pos--;
	}
	while (pos >= 0) {
		if (buf_[pos] == '\n') {
			if (++lineCount >= nLines)
				return pos + 1;
		}
		pos--;
	}
	return 0;
}

/*
** Search forwards in buffer "buf" for characters in "searchChars", starting
** with the character "startPos", and returning the result in "foundPos"
** returns true if found, false if not.
*/
bool TextBuffer::BufSearchForward(int startPos, const char_type *searchChars, int *foundPos) const {
	int pos;
	const int gapLen = gapEnd_ - gapStart_;
	const char_type *c;

	pos = startPos;
	while (pos < gapStart_) {
#if 0
		// NOTE(eteran): i think this could be (better yet, use trait_type::find)
		if(strchr(searchChars, buf_[pos]) != nullptr) {
			*foundPos = pos;
			return true;
		}
#else
		for (c = searchChars; *c != '\0'; c++) {
			if (buf_[pos] == *c) {
				*foundPos = pos;
				return true;
			}
		}
#endif
		pos++;
	}
	while (pos < length_) {
#if 0
		// NOTE(eteran): i think this could be (better yet, use trait_type::find)
		if(strchr(searchChars, buf_[pos + gapLen]) != nullptr) {
			*foundPos = pos;
			return true;
		}
#else
		for (c = searchChars; *c != '\0'; c++) {
			if (buf_[pos + gapLen] == *c) {
				*foundPos = pos;
				return true;
			}
		}
#endif
		pos++;
	}
	*foundPos = length_;
	return false;
}

/*
** Search backwards in buffer "buf" for characters in "searchChars", starting
** with the character BEFORE "startPos", returning the result in "foundPos"
** returns true if found, false if not.
*/
bool TextBuffer::BufSearchBackward(int startPos, const char_type *searchChars, int *foundPos) const {
	int pos;
	const int gapLen = gapEnd_ - gapStart_;
	const char_type *c;

	if (startPos == 0) {
		*foundPos = 0;
		return false;
	}
	pos = startPos == 0 ? 0 : startPos - 1;
	while (pos >= gapStart_) {
		for (c = searchChars; *c != '\0'; c++) {
			if (buf_[pos + gapLen] == *c) {
				*foundPos = pos;
				return true;
			}
		}
		pos--;
	}
	while (pos >= 0) {
		for (c = searchChars; *c != '\0'; c++) {
			if (buf_[pos] == *c) {
				*foundPos = pos;
				return true;
			}
		}
		pos--;
	}
	*foundPos = 0;
	return false;
}

/*
** A horrible design flaw in NEdit (from the very start, before we knew that
** NEdit would become so popular), is that it uses C NUL terminated strings
** to hold text.  This means editing text containing NUL characters is not
** possible without special consideration.  Here is the special consideration.
** The routines below maintain a special substitution-character which stands
** in for a null, and translates strings an buffers back and forth from/to
** the substituted form, figure out what to substitute, and figure out
** when we're in over our heads and no translation is possible.
*/

/*
** The primary routine for integrating new text into a text buffer with
** substitution of another character for ascii nuls.  This substitutes null
** characters in the string in preparation for being copied or replaced
** into the buffer, and if neccessary, adjusts the buffer as well, in the
** event that the string contains the character it is currently using for
** substitution.  Returns false, if substitution is no longer possible
** because all non-printable characters are already in use.
*/
bool TextBuffer::BufSubstituteNullChars(char_type *string, int length) {
	char_type histogram[256];

	/* Find out what characters the string contains */
	histogramCharacters(string, length, histogram, true);

	/* Does the string contain the null-substitute character?  If so, re-
	   histogram the buffer text to find a character which is ok in both the
	   string and the buffer, and change the buffer's null-substitution
	   character.  If none can be found, give up and return false */
	if (histogram[static_cast<uint8_t>(nullSubsChar_)] != 0) {
		char_type *bufString;
		char_type newSubsChar;
		/* here we know we can modify the file buffer directly,
		   so we cast away constness */
		bufString = const_cast<char_type *>(BufAsString());
		histogramCharacters(bufString, length_, histogram, false);
		newSubsChar = chooseNullSubsChar(histogram);
		if (newSubsChar == '\0') {
			return false;
		}
		/* bufString points to the buffer's data, so we substitute in situ */
		subsChars(bufString, length_, nullSubsChar_, newSubsChar);
		nullSubsChar_ = newSubsChar;
	}

	/* If the string contains null characters, substitute them with the
	   buffer's null substitution character */
	if (histogram[0] != 0) {
		subsChars(string, length, '\0', nullSubsChar_);
	}
	return true;
}

/*
** Convert strings obtained from buffers which contain null characters, which
** have been substituted for by a special substitution character, back to
** a null-containing string.  There is no time penalty for calling this
** routine if no substitution has been done.
*/
void TextBuffer::BufUnsubstituteNullChars(char_type *string) const {
	if (nullSubsChar_ == '\0') {
		return;
	}

	for (char_type *c = string; *c != '\0'; c++) {
		if (*c == nullSubsChar_) {
			*c = '\0';
		}
	}
}

/*
** Compares len Bytes contained in buf starting at Position pos with
** the contens of cmpText. Returns 0 if there are no differences,
** != 0 otherwise.
**
*/
int TextBuffer::BufCmp(int pos, int len, const char_type *cmpText) const {
	int posEnd;
	int part1Length;
	int result;

	posEnd = pos + len;
	if (posEnd > length_) {
		return (1);
	}
	if (pos < 0) {
		return (-1);
	}

	if (posEnd <= gapStart_) {
		return traits_type::compare(&buf_[pos], cmpText, len);
	} else if (pos >= gapStart_) {
		return traits_type::compare(&buf_[pos + (gapEnd_ - gapStart_)], cmpText, len);
	} else {
		part1Length = gapStart_ - pos;
		result = traits_type::compare(&buf_[pos], cmpText, part1Length);
		if (result) {
			return result;
		}
		return traits_type::compare(&buf_[gapEnd_], &cmpText[part1Length], len - part1Length);
	}
}

/*
** Internal (non-redisplaying) version of BufInsert.  Returns the length of
** text inserted (this is just strlen(text), however this calculation can be
** expensive and the length will be required by any caller who will continue
** on to call redisplay).  pos must be contiguous with the existing text in
** the buffer (i.e. not past the end).
*/
int TextBuffer::insert(int pos, const char_type *text) {
	const int length = static_cast<int>(traits_type::length(text));
	return insert(pos, text, length);
}

/*
** Internal (non-redisplaying) version of BufInsert.  Returns the length of
** text inserted (this is just strlen(text), however this calculation can be
** expensive and the length will be required by any caller who will continue
** on to call redisplay).  pos must be contiguous with the existing text in
** the buffer (i.e. not past the end).
*/
int TextBuffer::insert(int pos, const char_type *text, int length) {

	/* Prepare the buffer to receive the new text.  If the new text fits in
	   the current buffer, just move the gap (if necessary) to where
	   the text should be inserted.  If the new text is too large, reallocate
	   the buffer with a gap large enough to accomodate the new text and a
	   gap of PREFERRED_GAP_SIZE */
	if (length > gapEnd_ - gapStart_)
		reallocateBuf(pos, length + PREFERRED_GAP_SIZE);
	else if (pos != gapStart_)
		moveGap(pos);

	/* Insert the new text (pos now corresponds to the start of the gap) */
#ifdef USE_MEMCPY
	memcpy(&buf_[pos], text, length);
#else
	std::copy_n(text, length, &buf_[pos]);
#endif
	gapStart_ += length;
	length_ += length;
	updateSelections(pos, 0, length);

	return length;
}

/*
** Internal (non-redisplaying) version of BufRemove.  Removes the contents
** of the buffer between start and end (and moves the gap to the site of
** the delete).
*/
void TextBuffer::deleteRange(int start, int end) {
	/* if the gap is not contiguous to the area to remove, move it there */
	if (start > gapStart_)
		moveGap(start);
	else if (end < gapStart_)
		moveGap(end);

	/* expand the gap to encompass the deleted characters */
	gapEnd_ += end - gapStart_;
	gapStart_ -= gapStart_ - start;

	/* update the length */
	length_ -= end - start;

	/* fix up any selections which might be affected by the change */
	updateSelections(start, end - start, 0);
}

/*
** Insert a column of text without calling the modify callbacks.  Note that
** in some pathological cases, inserting can actually decrease the size of
** the buffer because of spaces being coalesced into tabs.  "nDeleted" and
** "nInserted" return the number of characters deleted and inserted beginning
** at the start of the line containing "startPos".  "endPos" returns buffer
** position of the lower left edge of the inserted column (as a hint for
** routines which need to set a cursor position).
*/
void TextBuffer::insertCol(int column, int startPos, const char_type *insText, int *nDeleted, int *nInserted, int *endPos) {
	int nLines, start, end, insWidth, lineStart;
	int expReplLen, expInsLen, len, endOffset;
	char_type *outStr;
	char_type *outPtr;
	char_type *replText;
	char_type *expText;
	const char_type *insPtr;

	if (column < 0)
		column = 0;

	/* Allocate a buffer for the replacement string large enough to hold
	   possibly expanded tabs in both the inserted text and the replaced
	   area, as well as per line: 1) an additional 2*MAX_EXP_CHAR_LEN
	   characters for padding where tabs and control characters cross the
	   column of the Selection, 2) up to "column" additional spaces per
	   line for padding out to the position of "column", 3) padding up
	   to the width of the inserted text if that must be padded to align
	   the text beyond the inserted column.  (Space for additional
	   newlines if the inserted text extends beyond the end of the buffer
	   is counted with the length of insText) */
	start = BufStartOfLine(startPos);
	nLines = countLines(insText) + 1;
	insWidth = textWidth(insText, tabDist_, nullSubsChar_);
	end = BufEndOfLine(BufCountForwardNLines(start, nLines - 1));
	replText = BufGetRange(start, end);
	expText = expandTabs(replText, 0, tabDist_, nullSubsChar_, &expReplLen);
	delete[] replText;
	delete[] expText;
	expText = expandTabs(insText, 0, tabDist_, nullSubsChar_, &expInsLen);
	delete[] expText;
	outStr = new char_type[expReplLen + expInsLen + nLines * (column + insWidth + MAX_EXP_CHAR_LEN) + 1];

	/* Loop over all lines in the buffer between start and end inserting
	   text at column, splitting tabs and adding padding appropriately */
	outPtr = outStr;
	lineStart = start;
	insPtr = insText;
	while (true) {
		int lineEnd = BufEndOfLine(lineStart);
		char_type *line = BufGetRange(lineStart, lineEnd);
		char_type *insLine = copyLine(insPtr, &len);
		insPtr += len;
		insertColInLine(line, insLine, column, insWidth, tabDist_, useTabs_, nullSubsChar_, outPtr, &len, &endOffset);
		delete[] line;
		delete[] insLine;
#if 0 /* Earlier comments claimed that trailing whitespace could multiply on                                           \
      the ends of lines, but insertColInLine looks like it should never                                                \
      add space unnecessarily, and this trimming interfered with                                                       \
      paragraph filling, so lets see if it works without it. MWE */
        {
            char_type *c;
    	    for (c=outPtr+len-1; c>outPtr && (*c == ' ' || *c == '\t'); c--)
                len--;
        }
#endif
		outPtr += len;
		*outPtr++ = '\n';
		lineStart = lineEnd < length_ ? lineEnd + 1 : length_;
		if (*insPtr == '\0')
			break;
		insPtr++;
	}
	if (outPtr != outStr)
		outPtr--; /* trim back off extra newline */
	*outPtr = '\0';

	/* replace the text between start and end with the new stuff */
	deleteRange(start, end);
	insert(start, outStr);
	*nInserted = outPtr - outStr;
	*nDeleted = end - start;
	*endPos = start + (outPtr - outStr) - len + endOffset;
	delete[] outStr;
}

/*
** Delete a rectangle of text without calling the modify callbacks.  Returns
** the number of characters replacing those between start and end.  Note that
** in some pathological cases, deleting can actually increase the size of
** the buffer because of tab expansions.  "endPos" returns the buffer position
** of the point in the last line where the text was removed (as a hint for
** routines which need to position the cursor after a delete operation)
*/
void TextBuffer::deleteRect(int start, int end, int rectStart, int rectEnd, int *replaceLen, int *endPos) {
	int nLines, lineStart, len, endOffset = 0;
	char_type *outStr, *outPtr, *text, *expText;

	/* allocate a buffer for the replacement string large enough to hold
	   possibly expanded tabs as well as an additional  MAX_EXP_CHAR_LEN * 2
	   characters per line for padding where tabs and control characters cross
	   the edges of the Selection */
	start = BufStartOfLine(start);
	end = BufEndOfLine(end);
	nLines = BufCountLines(start, end) + 1;
	text = BufGetRange(start, end);
	expText = expandTabs(text, 0, tabDist_, nullSubsChar_, &len);
	delete[] text;
	delete[] expText;
	outStr = new char_type[len + nLines * MAX_EXP_CHAR_LEN * 2 + 1];

	/* loop over all lines in the buffer between start and end removing
	   the text between rectStart and rectEnd and padding appropriately */
	lineStart = start;
	outPtr = outStr;
	while (lineStart <= length_ && lineStart <= end) {
		int lineEnd = BufEndOfLine(lineStart);
		char_type *line = BufGetRange(lineStart, lineEnd);
		deleteRectFromLine(line, rectStart, rectEnd, tabDist_, useTabs_, nullSubsChar_, outPtr, &len, &endOffset);
		delete[] line;
		outPtr += len;
		*outPtr++ = '\n';
		lineStart = lineEnd + 1;
	}
	if (outPtr != outStr)
		outPtr--; /* trim back off extra newline */
	*outPtr = '\0';

	/* replace the text between start and end with the newly created string */
	deleteRange(start, end);
	insert(start, outStr);
	*replaceLen = outPtr - outStr;
	*endPos = start + (outPtr - outStr) - len + endOffset;
	delete[] outStr;
}

/*
** Overlay a rectangular area of text without calling the modify callbacks.
** "nDeleted" and "nInserted" return the number of characters deleted and
** inserted beginning at the start of the line containing "startPos".
** "endPos" returns buffer position of the lower left edge of the inserted
** column (as a hint for routines which need to set a cursor position).
*/
void TextBuffer::overlayRect(int startPos, int rectStart, int rectEnd, const char_type *insText, int *nDeleted,
                             int *nInserted, int *endPos) {
    int lineStart;
	int expInsLen, len, endOffset;
	char_type *c;
	char_type *outStr;
	char_type *outPtr;
	char_type *expText;
	const char_type *insPtr;

	/* Allocate a buffer for the replacement string large enough to hold
	   possibly expanded tabs in the inserted text, as well as per line: 1)
	   an additional 2*MAX_EXP_CHAR_LEN characters for padding where tabs
	   and control characters cross the column of the Selection, 2) up to
	   "column" additional spaces per line for padding out to the position
	   of "column", 3) padding up to the width of the inserted text if that
	   must be padded to align the text beyond the inserted column.  (Space
	   for additional newlines if the inserted text extends beyond the end
	   of the buffer is counted with the length of insText) */
	int start = BufStartOfLine(startPos);
	int nLines = countLines(insText) + 1;
	int end = BufEndOfLine(BufCountForwardNLines(start, nLines - 1));
	expText = expandTabs(insText, 0, tabDist_, nullSubsChar_, &expInsLen);
	delete[] expText;
	outStr = new char_type[end - start + expInsLen + nLines * (rectEnd + MAX_EXP_CHAR_LEN) + 1];

	/* Loop over all lines in the buffer between start and end overlaying the
	   text between rectStart and rectEnd and padding appropriately.  Trim
	   trailing space from line (whitespace at the ends of lines otherwise
	   tends to multiply, since additional padding is added to maintain it */
	outPtr = outStr;
	lineStart = start;
	insPtr = insText;
	while (true) {
		int lineEnd = BufEndOfLine(lineStart);
		char_type *line = BufGetRange(lineStart, lineEnd);
		char_type *insLine = copyLine(insPtr, &len);
		insPtr += len;
		overlayRectInLine(line, insLine, rectStart, rectEnd, tabDist_, useTabs_, nullSubsChar_, outPtr, &len,
						  &endOffset);
		delete[] line;
		delete[] insLine;
		for (c = outPtr + len - 1; c > outPtr && (*c == ' ' || *c == '\t'); c--)
			len--;
		outPtr += len;
		*outPtr++ = '\n';
		lineStart = lineEnd < length_ ? lineEnd + 1 : length_;
		if (*insPtr == '\0')
			break;
		insPtr++;
	}
	if (outPtr != outStr)
		outPtr--; /* trim back off extra newline */
	*outPtr = '\0';

	/* replace the text between start and end with the new stuff */
	deleteRange(start, end);
	insert(start, outStr);
	*nInserted = outPtr - outStr;
	*nDeleted = end - start;
	*endPos = start + (outPtr - outStr) - len + endOffset;
	delete[] outStr;
}

char_type *TextBuffer::getSelectionText(const Selection &sel) const {
	int start;
	int end;
	bool isRect;
	int rectStart;
	int rectEnd;

	/* If there's no Selection, return an allocated empty string */
	if (!getSelectionPos(sel, &start, &end, &isRect, &rectStart, &rectEnd)) {
		auto text = new char_type[1];
		*text = '\0';
		return text;
	}

	/* If the Selection is not rectangular, return the selected range */
	if (isRect) {
		return BufGetTextInRect(start, end, rectStart, rectEnd);
	} else {
		return BufGetRange(start, end);
	}
}

void TextBuffer::removeSelected(const Selection &sel) {
	int start;
	int end;
	bool isRect;
	int rectStart;
	int rectEnd;

	if (!getSelectionPos(sel, &start, &end, &isRect, &rectStart, &rectEnd)) {
		return;
	}

	if (isRect) {
		BufRemoveRect(start, end, rectStart, rectEnd);
	} else {
		BufRemove(start, end);
	}
}

void TextBuffer::replaceSelected(Selection *sel, const char_type *text) {
	int start;
	int end;
	bool isRect;
	int rectStart;
	int rectEnd;
	Selection oldSelection = *sel;

	/* If there's no Selection, return */
	if (!getSelectionPos(*sel, &start, &end, &isRect, &rectStart, &rectEnd)) {
		return;
	}

	/* Do the appropriate type of replace */
	if (isRect) {
		BufReplaceRect(start, end, rectStart, rectEnd, text);
	} else {
		BufReplace(start, end, text);
	}

	/* Unselect (happens automatically in BufReplace, but BufReplaceRect
	   can't detect when the contents of a Selection goes away) */
	sel->selected = false;
	redisplaySelection(oldSelection, *sel);
}

/*
** Call the stored modify callback procedure(s) for this buffer to update the
** changed area(s) on the screen and any other listeners.
*/
void TextBuffer::callModifyCBs(int pos, int nDeleted, int nInserted, int nRestyled, const char_type *deletedText) {
	ModifyEvent event;
	event.pos = pos;
	event.nDeleted = nDeleted;
	event.nInserted = nInserted;
	event.nRestyled = nRestyled;
	event.deletedText = deletedText;
	event.buffer = this;

	for (const auto &handler : modifyProcs_) {
		handler->bufferModified(&event);
	}
}

/*
** Call the stored pre-delete callback procedure(s) for this buffer to update
** the changed area(s) on the screen and any other listeners.
*/
void TextBuffer::callPreDeleteCBs(int pos, int nDeleted) {

	PreDeleteEvent event;
	event.pos = pos;
	event.nDeleted = nDeleted;
	event.buffer = this;

	for (const auto &handler : preDeleteProcs_) {
		handler->preDelete(&event);
	}
}

/*
** Call the stored redisplay procedure(s) for this buffer to update the
** screen for a change in a Selection.
*/
void TextBuffer::redisplaySelection(const Selection &oldSelection, const Selection &newSelection) {

	int ch1Start;
	int ch1End;
	int ch2Start;
	int ch2End;

	/* If either Selection is rectangular, add an additional character to
	   the end of the Selection to request the redraw routines to wipe out
	   the parts of the Selection beyond the end of the line */
	int oldStart = oldSelection.start;
	int newStart = newSelection.start;
	int oldEnd = oldSelection.end;
	int newEnd = newSelection.end;

	if (oldSelection.rectangular) {
		++oldEnd;
	}

	if (newSelection.rectangular) {
		++newEnd;
	}

	/* If the old or new Selection is unselected, just redisplay the
	   single area that is (was) selected and return */
	if (!oldSelection.selected && !newSelection.selected) {
		return;
	}

	if (!oldSelection.selected) {
		callModifyCBs(newStart, 0, 0, newEnd - newStart, nullptr);
		return;
	}
	if (!newSelection.selected) {
		callModifyCBs(oldStart, 0, 0, oldEnd - oldStart, nullptr);
		return;
	}

	/* If the Selection changed from normal to rectangular or visa versa, or
	   if a rectangular Selection changed boundaries, redisplay everything */
	if ((oldSelection.rectangular && !newSelection.rectangular) ||
	    (!oldSelection.rectangular && newSelection.rectangular) ||
	    (oldSelection.rectangular &&
	     ((oldSelection.rectStart != newSelection.rectStart) || (oldSelection.rectEnd != newSelection.rectEnd)))) {

		callModifyCBs(std::min(oldStart, newStart), 0, 0, std::max(oldEnd, newEnd) - std::min(oldStart, newStart),
		              nullptr);

		return;
	}

	/* If the selections are non-contiguous, do two separate updates
	   and return */
	if (oldEnd < newStart || newEnd < oldStart) {
		callModifyCBs(oldStart, 0, 0, oldEnd - oldStart, nullptr);
		callModifyCBs(newStart, 0, 0, newEnd - newStart, nullptr);
		return;
	}

	/* Otherwise, separate into 3 separate regions: ch1, and ch2 (the two
	   changed areas), and the unchanged area of their intersection,
	   and update only the changed area(s) */
	ch1Start = std::min(oldStart, newStart);
	ch2End = std::max(oldEnd, newEnd);
	ch1End = std::max(oldStart, newStart);
	ch2Start = std::min(oldEnd, newEnd);

	if (ch1Start != ch1End) {
		callModifyCBs(ch1Start, 0, 0, ch1End - ch1Start, nullptr);
	}

	if (ch2Start != ch2End) {
		callModifyCBs(ch2Start, 0, 0, ch2End - ch2Start, nullptr);
	}
}

void TextBuffer::moveGap(int pos) {
	const int gapLen = gapEnd_ - gapStart_;

#ifdef USE_MEMCPY
	if (pos > gapStart_) {
		memmove(&buf_[gapStart_], &buf_[gapEnd_], pos - gapStart_);
	} else {
		memmove(&buf_[pos + gapLen], &buf_[pos], gapStart_ - pos);
	}
#else
	if (pos > gapStart_) {
		//assert((gapEnd_ + pos - gapStart_) < gapEnd_);
		std::copy_backward(&buf_[gapEnd_], &buf_[gapEnd_ + pos - gapStart_], &buf_[gapStart_ + pos - gapStart_]);
	} else {
		std::copy(&buf_[pos], &buf_[gapStart_], &buf_[pos + gapLen]);
	}
#endif

	gapEnd_ += pos - gapStart_;
	gapStart_ += pos - gapStart_;
}

/*
** reallocate the text storage in "buf" to have a gap starting at "newGapStart"
** and a gap size of "newGapLen", preserving the buffer's current contents.
*/
void TextBuffer::reallocateBuf(int newGapStart, int newGapLen) {

	auto newBuf = new char_type[length_ + newGapLen + 1];
	newBuf[length_ + PREFERRED_GAP_SIZE] = '\0';
	int newGapEnd = newGapStart + newGapLen;
#ifdef USE_MEMCPY
	if (newGapStart <= gapStart_) {
		memcpy(newBuf, buf_, newGapStart);
		memcpy(&newBuf[newGapEnd], &buf_[newGapStart], gapStart_ - newGapStart);
		memcpy(&newBuf[newGapEnd + gapStart_ - newGapStart], &buf_[gapEnd_], length_ - gapStart_);
	} else { /* newGapStart > gapStart_ */
		memcpy(newBuf, buf_, gapStart_);
		memcpy(&newBuf[gapStart_], &buf_[gapEnd_], newGapStart - gapStart_);
		memcpy(&newBuf[newGapEnd], &buf_[gapEnd_ + newGapStart - gapStart_], length_ - newGapStart);
	}
#else
	if (newGapStart <= gapStart_) {
		std::copy_n(buf_, newGapStart, newBuf);
		std::copy_n(&buf_[newGapStart], gapStart_ - newGapStart, &newBuf[newGapEnd]);
		std::copy_n(&buf_[gapEnd_], length_ - gapStart_, &newBuf[newGapEnd + gapStart_ - newGapStart]);
	} else { /* newGapStart > gapStart_ */
		std::copy_n(buf_, gapStart_, newBuf);
		std::copy_n(&buf_[gapEnd_], newGapStart - gapStart_, &newBuf[gapStart_]);
		std::copy_n(&buf_[gapEnd_ + newGapStart - gapStart_], length_ - newGapStart, &newBuf[newGapEnd]);
	}
#endif
	delete[] buf_;
	buf_ = newBuf;
	gapStart_ = newGapStart;
	gapEnd_ = newGapEnd;
#ifdef PURIFY
	std::fill_n(&buf_[gapStart_], gapEnd_ - gapStart_, '.');
#endif
}

/*
** Update all of the selections in "buf" for changes in the buffer's text
*/
void TextBuffer::updateSelections(int pos, int nDeleted, int nInserted) {
	updateSelection(&primary_, pos, nDeleted, nInserted);
	updateSelection(&secondary_, pos, nDeleted, nInserted);
	updateSelection(&highlight_, pos, nDeleted, nInserted);
}

/*
** Search forwards in buffer "buf" for character "searchChar", starting
** with the character "startPos", and returning the result in "foundPos"
** returns true if found, false if not.  (The difference between this and
** BufSearchForward is that it's optimized for single characters.  The
** overall performance of the text widget is dependent on its ability to
** count lines quickly, hence searching for a single character: newline)
*/
bool TextBuffer::searchForward(int startPos, char_type searchChar, int *foundPos) const {
	int pos, gapLen = gapEnd_ - gapStart_;

	pos = startPos;
	while (pos < gapStart_) {
		if (buf_[pos] == searchChar) {
			*foundPos = pos;
			return true;
		}
		pos++;
	}
	while (pos < length_) {
		if (buf_[pos + gapLen] == searchChar) {
			*foundPos = pos;
			return true;
		}
		pos++;
	}
	*foundPos = length_;
	return false;
}

/*
** Search backwards in buffer "buf" for character "searchChar", starting
** with the character BEFORE "startPos", returning the result in "foundPos"
** returns true if found, false if not.  (The difference between this and
** BufSearchBackward is that it's optimized for single characters.  The
** overall performance of the text widget is dependent on its ability to
** count lines quickly, hence searching for a single character: newline)
*/
bool TextBuffer::searchBackward(int startPos, char_type searchChar, int *foundPos) const {
	int pos, gapLen = gapEnd_ - gapStart_;

	if (startPos == 0) {
		*foundPos = 0;
		return false;
	}
	pos = startPos == 0 ? 0 : startPos - 1;
	while (pos >= gapStart_) {
		if (buf_[pos + gapLen] == searchChar) {
			*foundPos = pos;
			return true;
		}
		pos--;
	}
	while (pos >= 0) {
		if (buf_[pos] == searchChar) {
			*foundPos = pos;
			return true;
		}
		pos--;
	}
	*foundPos = 0;
	return false;
}

/*
** Find the first and last character position in a line withing a rectangular
** Selection (for copying).  Includes tabs which cross rectStart, but not
** control characters which do so.  Leaves off tabs which cross rectEnd.
**
** Technically, the calling routine should convert tab characters which
** cross the right boundary of the Selection to spaces which line up with
** the edge of the Selection.  Unfortunately, the additional memory
** management required in the parent routine to allow for the changes
** in string size is not worth all the extra work just for a couple of
** shifted characters, so if a tab protrudes, just lop it off and hope
** that there are other characters in the Selection to establish the right
** margin for subsequent columnar pastes of this data.
*/
void TextBuffer::findRectSelBoundariesForCopy(int lineStartPos, int rectStart, int rectEnd, int *selStart,
                                              int *selEnd) const {
	int pos, width, indent = 0;
	char_type c;

	/* find the start of the Selection */
	for (pos = lineStartPos; pos < length_; pos++) {
		c = BufGetCharacter(pos);
		if (c == '\n')
			break;
		width = BufCharWidth(c, indent, tabDist_, nullSubsChar_);
		if (indent + width > rectStart) {
			if (indent != rectStart && c != '\t') {
				pos++;
				indent += width;
			}
			break;
		}
		indent += width;
	}
	*selStart = pos;

	/* find the end */
	for (; pos < length_; pos++) {
		c = BufGetCharacter(pos);
		if (c == '\n') {
			break;
		}
		width = BufCharWidth(c, indent, tabDist_, nullSubsChar_);
		indent += width;
		if (indent > rectEnd) {
			if (indent - width != rectEnd && c != '\t')
				pos++;
			break;
		}
	}
	*selEnd = pos;
}

int TextBuffer::BufGetLength() const {
	return length_;
}

char_type TextBuffer::BufGetNullSubsChar() const {
	return nullSubsChar_;
}

Selection &TextBuffer::BufGetPrimarySelection() {
	return primary_;
}

Selection &TextBuffer::BufGetSecondarySelection() {
	return secondary_;
}

Selection &TextBuffer::BufGetHighlight() {
	return highlight_;
}

int TextBuffer::BufGetCursorPosHint() const {
	return cursorPosHint_;
}

bool TextBuffer::BufGetUseTabs() const {
	return useTabs_;
}

void TextBuffer::BufSetUseTabs(bool value) {
	useTabs_ = value;
}

/*
** Overlay characters from single-line string "insLine" on single-line string
** "line" between displayed character offsets "rectStart" and "rectEnd".
** "outLen" returns the number of characters written to "outStr", "endOffset"
** returns the number of characters from the beginning of the string to
** the right edge of the inserted text (as a hint for routines which need
** to position the cursor).
**
** This code does not handle control characters very well, but oh well.
*/
void TextBuffer::overlayRectInLine(const char_type *line, const char_type *insLine, int rectStart, int rectEnd, int tabDist, bool useTabs, char_type nullSubsChar, char_type *outStr, int *outLen, int *endOffset) {
	char_type *outPtr, *retabbedStr;
	const char_type *linePtr;
	int inIndent, outIndent, len, postRectIndent;

	/* copy the line up to "rectStart" or just before the character that
		contains it*/
	outPtr = outStr;
	inIndent = outIndent = 0;
	for (linePtr = line; *linePtr != '\0'; linePtr++) {
		len = BufCharWidth(*linePtr, inIndent, tabDist, nullSubsChar);
		if (inIndent + len > rectStart)
			break;
		inIndent += len;
		outIndent += len;
		*outPtr++ = *linePtr;
	}

	/* If "rectStart" falls in the middle of a character, and the character
	   is a tab, leave it off and leave the outIndent short and it will get
	   padded later.  If it's a control character, insert it and adjust
	   outIndent accordingly. */
	if (inIndent < rectStart && *linePtr != '\0') {
		if (*linePtr == '\t') {
			/* Skip past the tab */
			linePtr++;
			inIndent += len;
		} else {
			*outPtr++ = *linePtr++;
			outIndent += len;
			inIndent += len;
		}
	}

	/* skip the characters between rectStart and rectEnd */
	for (; *linePtr != '\0' && inIndent < rectEnd; linePtr++)
		inIndent += BufCharWidth(*linePtr, inIndent, tabDist, nullSubsChar);
	postRectIndent = inIndent;

	/* After this inIndent is dead and linePtr is supposed to point at the
		character just past the last character that will be altered by
		the overlay, whether that's a \t or otherwise.  postRectIndent is
		the position at which that character is supposed to appear */

	/* If there's no text after rectStart and no text to insert, that's all */
	if (*insLine == '\0' && *linePtr == '\0') {
		*outLen = *endOffset = outPtr - outStr;
		return;
	}

	/* pad out to rectStart if text is too short */
	if (outIndent < rectStart) {
		addPadding(outPtr, outIndent, rectStart, tabDist, useTabs, nullSubsChar, &len);
		outPtr += len;
	}
	outIndent = rectStart;

	/* Copy the text from "insLine" (if any), recalculating the tabs as if
	   the inserted string began at column 0 to its new column destination */
	if (*insLine != '\0') {
		retabbedStr = realignTabs(insLine, 0, rectStart, tabDist, useTabs, nullSubsChar, &len);
		for (char_type *c = retabbedStr; *c != '\0'; c++) {
			*outPtr++ = *c;
			len = BufCharWidth(*c, outIndent, tabDist, nullSubsChar);
			outIndent += len;
		}
		delete[] retabbedStr;
	}

	/* If the original line did not extend past "rectStart", that's all */
	if (*linePtr == '\0') {
		*outLen = *endOffset = outPtr - outStr;
		return;
	}

	/* Pad out to rectEnd + (additional original offset
	   due to non-breaking character at right boundary) */
	addPadding(outPtr, outIndent, postRectIndent, tabDist, useTabs, nullSubsChar, &len);
	outPtr += len;
	outIndent = postRectIndent;

	int lineLength = static_cast<int>(traits_type::length(linePtr));

	/* copy the text beyond "rectEnd" */
#ifdef USE_STRCPY
	strcpy(outPtr, linePtr);
#else
	std::copy_n(linePtr, lineLength, outPtr);
	outPtr[lineLength] = '\0';
#endif
	*endOffset = outPtr - outStr;
	*outLen = (outPtr - outStr) + lineLength;
}

/*
** Copy from "text" to end up to but not including newline (or end of "text")
** and return the copy as the function value, and the length of the line in
** "lineLen"
*/
char_type *TextBuffer::copyLine(const char_type *text, int *lineLen) {

	assert(text);
	assert(lineLen);

	int len = 0;
	for (const char_type *c = text; *c != '\0' && *c != '\n'; c++) {
		len++;
	}

	auto outStr = new char_type[len + 1];
#ifdef USE_STRCPY
	strncpy(outStr, text, len);
#else
	std::copy_n(text, len, outStr);
#endif
	outStr[len] = '\0';

	*lineLen = len;
	return outStr;
}

/*
** Count the number of newlines in a null-terminated text string;
*/
int TextBuffer::countLines(const char_type *string) {
	int lineCount = 0;

	for (const char_type *c = string; *c != '\0'; c++) {
		if (*c == '\n') {
			lineCount++;
		}
	}

	return lineCount;
}

/*
** Count the number of newlines in a null-terminated text string;
*/
int TextBuffer::countLines(const char_type *string, size_t length) {
	return static_cast<int>(std::count(string, string + length, '\n'));
}

/*
** Measure the width in displayed characters of string "text"
*/
int TextBuffer::textWidth(const char_type *text, int tabDist, char_type nullSubsChar) {
	int width = 0;
	int maxWidth = 0;

	for (const char_type *c = text; *c != '\0'; c++) {
		if (*c == '\n') {
			maxWidth = std::max(maxWidth, width);
			width = 0;
		} else {
			width += BufCharWidth(*c, width, tabDist, nullSubsChar);
		}
	}

	return std::max(maxWidth, width);
}

/*
** Create a pseudo-histogram of the characters in a string (don't actually
** count, because we don't want overflow, just mark the character's presence
** with a 1).  If init is true, initialize the histogram before acumulating.
** if not, add the new data to an existing histogram.
*/
void TextBuffer::histogramCharacters(const char_type *string, int length, char_type hist[256], bool init) {
	if (init) {
		for (int i = 0; i < 256; i++) {
			hist[i] = 0;
		}
	}

	for (const char_type *c = string; c < &string[length]; c++) {
		hist[*reinterpret_cast<const uint8_t *>(c)] |= 1;
	}
}

/*
** Substitute fromChar with toChar in string.
*/
void TextBuffer::subsChars(char_type *string, int length, char_type fromChar, char_type toChar) {
	for (char_type  *c = string; c < &string[length]; c++) {
		if (*c == fromChar) {
			*c = toChar;
		}
	}
}

/*
** Search through ascii control characters in histogram in order of least
** likelihood of use, find an unused character to use as a stand-in for a
** null.  If the character set is full (no available characters outside of
** the printable set, return the null character.
*/
char_type TextBuffer::chooseNullSubsChar(char_type hist[256]) {
#define N_REPLACEMENTS 25
	static char_type replacements[N_REPLACEMENTS] = {1,  2,  3,  4,  5,  6,  14, 15, 16, 17, 18, 19, 20,
												21, 22, 23, 24, 25, 26, 28, 29, 30, 31, 11, 7};
	int i;
	for (i = 0; i < N_REPLACEMENTS; i++)
		if (hist[static_cast<uint8_t>(replacements[i])] == 0)
			return replacements[i];
	return '\0';
}

/*
** Expand tabs to spaces for a block of text.  The additional parameter
** "startIndent" if nonzero, indicates that the text is a rectangular Selection
** beginning at column "startIndent"
*/
char_type *TextBuffer::expandTabs(const char_type *text, int startIndent, int tabDist, char_type nullSubsChar, int *newLen) {
	char_type *outStr, *outPtr;
	const char_type *c;
	int indent, len, outLen = 0;

	/* rehearse the expansion to figure out length for output string */
	indent = startIndent;
	for (c = text; *c != '\0'; c++) {
		if (*c == '\t') {
			len = BufCharWidth(*c, indent, tabDist, nullSubsChar);
			outLen += len;
			indent += len;
		} else if (*c == '\n') {
			indent = startIndent;
			outLen++;
		} else {
			indent += BufCharWidth(*c, indent, tabDist, nullSubsChar);
			outLen++;
		}
	}

	/* do the expansion */
	outStr = new char_type[outLen + 1];
	outPtr = outStr;
	indent = startIndent;
	for (c = text; *c != '\0'; c++) {
		if (*c == '\t') {
			len = BufExpandCharacter(*c, indent, outPtr, tabDist, nullSubsChar);
			outPtr += len;
			indent += len;
		} else if (*c == '\n') {
			indent = startIndent;
			*outPtr++ = *c;
		} else {
			indent += BufCharWidth(*c, indent, tabDist, nullSubsChar);
			*outPtr++ = *c;
		}
	}
	outStr[outLen] = '\0';
	*newLen = outLen;
	return outStr;
}

/*
** Convert sequences of spaces into tabs.  The threshold for conversion is
** when 3 or more spaces can be converted into a single tab, this avoids
** converting double spaces after a period withing a block of text.
*/
char_type *TextBuffer::unexpandTabs(const char_type *text, int startIndent, int tabDist, char_type nullSubsChar, int *newLen) {
	char_type *outStr, *outPtr, expandedChar[MAX_EXP_CHAR_LEN];
	const char_type *c;
	int indent;
	int len;

	outStr = new char_type[traits_type::length(text) + 1];
	outPtr = outStr;
	indent = startIndent;
	for (c = text; *c != '\0';) {
		if (*c == ' ') {
			len = BufExpandCharacter('\t', indent, expandedChar, tabDist, nullSubsChar);
			if (len >= 3 && !traits_type::compare(c, expandedChar, len)) {
				c += len;
				*outPtr++ = '\t';
				indent += len;
			} else {
				*outPtr++ = *c++;
				indent++;
			}
		} else if (*c == '\n') {
			indent = startIndent;
			*outPtr++ = *c++;
		} else {
			*outPtr++ = *c++;
			indent++;
		}
	}
	*outPtr = '\0';
	*newLen = outPtr - outStr;
	return outStr;
}

/*
** Adjust the space and tab characters from string "text" so that non-white
** characters remain stationary when the text is shifted from starting at
** "origIndent" to starting at "newIndent".  Returns an allocated string
** which must be freed by the caller with delete[].
*/
char_type *TextBuffer::realignTabs(const char_type *text, int origIndent, int newIndent, int tabDist, bool useTabs, char_type nullSubsChar, int *newLength) {


	/* If the tabs settings are the same, retain original tabs */
	if (origIndent % tabDist == newIndent % tabDist) {
		int len = static_cast<int>(traits_type::length(text));
		auto outStr = new char_type[len + 1];
#ifdef USE_STRCPY
		strcpy(outStr, text);
#else
		std::copy_n(text, len, outStr);
		outStr[len] = '\0';
#endif
		*newLength = len;
		return outStr;
	}

	/* If the tab settings are not the same, brutally convert tabs to
	   spaces, then back to tabs in the new position */
	int len;
	char_type *expStr = expandTabs(text, origIndent, tabDist, nullSubsChar, &len);
	if (!useTabs) {
		*newLength = len;
		return expStr;
	}

	auto outStr = unexpandTabs(expStr, newIndent, tabDist, nullSubsChar, newLength);
	delete[] expStr;
	return outStr;
}

/*
** Insert characters from single-line string "insLine" in single-line string
** "line" at "column", leaving "insWidth" space before continuing line.
** "outLen" returns the number of characters written to "outStr", "endOffset"
** returns the number of characters from the beginning of the string to
** the right edge of the inserted text (as a hint for routines which need
** to position the cursor).
*/
void TextBuffer::insertColInLine(const char_type *line, const char_type *insLine, int column, int insWidth, int tabDist, bool useTabs, char_type nullSubsChar, char_type *outStr, int *outLen, int *endOffset) {
	const char_type *linePtr;
	int toIndent, len, postColIndent;

	/* copy the line up to "column" */
	char_type *outPtr = outStr;
	int indent = 0;
	for (linePtr = line; *linePtr != '\0'; linePtr++) {
		len = BufCharWidth(*linePtr, indent, tabDist, nullSubsChar);
		if (indent + len > column)
			break;
		indent += len;
		*outPtr++ = *linePtr;
	}

	/* If "column" falls in the middle of a character, and the character is a
	   tab, leave it off and leave the indent short and it will get padded
	   later.  If it's a control character, insert it and adjust indent
	   accordingly. */
	if (indent < column && *linePtr != '\0') {
		postColIndent = indent + len;
		if (*linePtr == '\t')
			linePtr++;
		else {
			*outPtr++ = *linePtr++;
			indent += len;
		}
	} else
		postColIndent = indent;

	/* If there's no text after the column and no text to insert, that's all */
	if (*insLine == '\0' && *linePtr == '\0') {
		*outLen = *endOffset = outPtr - outStr;
		return;
	}

	/* pad out to column if text is too short */
	if (indent < column) {
		addPadding(outPtr, indent, column, tabDist, useTabs, nullSubsChar, &len);
		outPtr += len;
		indent = column;
	}

	/* Copy the text from "insLine" (if any), recalculating the tabs as if
	   the inserted string began at column 0 to its new column destination */
	if (*insLine != '\0') {
		char_type *retabbedStr = realignTabs(insLine, 0, indent, tabDist, useTabs, nullSubsChar, &len);

		for (char_type *c = retabbedStr; *c != '\0'; c++) {
			*outPtr++ = *c;
			len = BufCharWidth(*c, indent, tabDist, nullSubsChar);
			indent += len;
		}

		delete[] retabbedStr;
	}

	/* If the original line did not extend past "column", that's all */
	if (*linePtr == '\0') {
		*outLen = *endOffset = outPtr - outStr;
		return;
	}

	/* Pad out to column + width of inserted text + (additional original
	   offset due to non-breaking character at column) */
	toIndent = column + insWidth + postColIndent - column;
	addPadding(outPtr, indent, toIndent, tabDist, useTabs, nullSubsChar, &len);
	outPtr += len;
	indent = toIndent;

	/* realign tabs for text beyond "column" and write it out */
	char_type *retabbedStr = realignTabs(linePtr, postColIndent, indent, tabDist, useTabs, nullSubsChar, &len);
#ifdef USE_STRCPY
	strcpy(outPtr, retabbedStr);
#else
	std::copy_n(retabbedStr, len, outPtr);
	outPtr[len] = '\0';
#endif
	delete[] retabbedStr;

	*endOffset = outPtr - outStr;
	*outLen = (outPtr - outStr) + len;
}

/*
** Remove characters in single-line string "line" between displayed positions
** "rectStart" and "rectEnd", and write the result to "outStr", which is
** assumed to be large enough to hold the returned string.  Note that in
** certain cases, it is possible for the string to get longer due to
** expansion of tabs.  "endOffset" returns the number of characters from
** the beginning of the string to the point where the characters were
** deleted (as a hint for routines which need to position the cursor).
*/
void TextBuffer::deleteRectFromLine(const char_type *line, int rectStart, int rectEnd, int tabDist, bool useTabs, char_type nullSubsChar, char_type *outStr, int *outLen, int *endOffset) {
	int indent, preRectIndent, postRectIndent, len;
	const char_type *c;
	char_type *outPtr;
	char_type *retabbedStr;

	/* copy the line up to rectStart */
	outPtr = outStr;
	indent = 0;
	for (c = line; *c != '\0'; c++) {
		if (indent > rectStart)
			break;
		len = BufCharWidth(*c, indent, tabDist, nullSubsChar);
		if (indent + len > rectStart && (indent == rectStart || *c == '\t'))
			break;
		indent += len;
		*outPtr++ = *c;
	}
	preRectIndent = indent;

	/* skip the characters between rectStart and rectEnd */
	for (; *c != '\0' && indent < rectEnd; c++) {
		indent += BufCharWidth(*c, indent, tabDist, nullSubsChar);
	}
	postRectIndent = indent;

	/* If the line ended before rectEnd, there's nothing more to do */
	if (*c == '\0') {
		*outPtr = '\0';
		*outLen = *endOffset = outPtr - outStr;
		return;
	}

	/* fill in any space left by removed tabs or control characters
	   which straddled the boundaries */
	indent = std::max(rectStart + postRectIndent - rectEnd, preRectIndent);
	addPadding(outPtr, preRectIndent, indent, tabDist, useTabs, nullSubsChar, &len);
	outPtr += len;

	/* Copy the rest of the line.  If the indentation has changed, preserve
	   the position of non-whitespace characters by converting tabs to
	   spaces, then back to tabs with the correct offset */
	retabbedStr = realignTabs(c, postRectIndent, indent, tabDist, useTabs, nullSubsChar, &len);
#ifdef USE_STRCPY
	strcpy(outPtr, retabbedStr);
#else
	std::copy_n(retabbedStr, len, outPtr);
	outPtr[len] = '\0';
#endif
	delete[] retabbedStr;
	*endOffset = outPtr - outStr;
	*outLen = (outPtr - outStr) + len;
}

void TextBuffer::addPadding(char_type *string, int startIndent, int toIndent, int tabDist, bool useTabs, char_type nullSubsChar, int *charsAdded) {
	int indent = startIndent;
	char_type *outPtr = string;
	if (useTabs) {
		while (indent < toIndent) {
			int len = BufCharWidth('\t', indent, tabDist, nullSubsChar);
			if (len > 1 && indent + len <= toIndent) {
				*outPtr++ = '\t';
				indent += len;
			} else {
				*outPtr++ = ' ';
				indent++;
			}
		}
	} else {
		while (indent < toIndent) {
			*outPtr++ = ' ';
			indent++;
		}
	}
	*charsAdded = outPtr - string;
}
