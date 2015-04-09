
#ifndef QCODE_WIDGET_H_
#define QCODE_WIDGET_H_

#include "TextBuffer.h"
#include "ICursorMoveHandler.h"
#include "IBufferModifiedHandler.h"
#include "IPreDeleteHandler.h"
#include "IHighlightHandler.h"
#include <QAbstractScrollArea>
#include <QList>

class SyntaxHighlighter;

enum ShiftDirection { SHIFT_LEFT, SHIFT_RIGHT };

enum CursorStyles { NORMAL_CURSOR, CARET_CURSOR, DIM_CURSOR, BLOCK_CURSOR, HEAVY_CURSOR };

enum PositionTypes { CURSOR_POS, CHARACTER_POS };

enum DragStates {
	NOT_CLICKED,
	PRIMARY_CLICKED,
	SECONDARY_CLICKED,
	CLICKED_IN_SELECTION,
	PRIMARY_DRAG,
	PRIMARY_RECT_DRAG,
	SECONDARY_DRAG,
	SECONDARY_RECT_DRAG,
	PRIMARY_BLOCK_DRAG,
	DRAG_CANCELED,
	MOUSE_PAN
};

enum MoveMode { MoveNoExtend, MoveExtend, MoveExtendRect };

enum PasteMode { PasteStandard, PasteColumnar };

class QCodeWidget : public QAbstractScrollArea, public IBufferModifiedHandler, public IPreDeleteHandler {
	Q_OBJECT
public:
	QCodeWidget(QWidget *parent = 0);
	virtual ~QCodeWidget() override;

public:
	virtual void bufferModified(const ModifyEvent *event) override;
	virtual void preDelete(const PreDeleteEvent *event) override;

protected:
	virtual void paintEvent(QPaintEvent *event) override;
	virtual void keyPressEvent(QKeyEvent *event) override;
	virtual void keyReleaseEvent(QKeyEvent *event) override;
	virtual void resizeEvent(QResizeEvent *event) override;
	virtual void mouseDoubleClickEvent(QMouseEvent *event) override;
	virtual void mouseMoveEvent(QMouseEvent *event) override;
	virtual void mousePressEvent(QMouseEvent *event) override;
	virtual void mouseReleaseEvent(QMouseEvent *event) override;
	virtual void mouseTripleClickEvent(QMouseEvent *event);
	virtual void mouseQuadrupleClickEvent(QMouseEvent *event);

private Q_SLOTS:
	void verticalScrollBar_valueChanged(int value);
	void horizontalScrollBar_valueChanged(int value);
	void customContextMenuRequested(const QPoint &pos);

public:
	const QFont &font() const;
	void setFont(const QFont &font);

private:
	int visibleColumns() const;
	int visibleRows() const;

private:
	static bool inSelection(const Selection *sel, int pos, int lineStartPos, int dispIndex);
	static bool rangeTouchesRectSel(Selection *sel, int rangeStart, int rangeEnd);

private:
	bool TextDMoveDown(bool absolute);
	bool TextDMoveLeft();
	bool TextDMoveRight();
	bool TextDMoveUp(bool absolute);
	bool TextDPosToLineAndCol(int pos, int *lineNum, int *column);
	bool TextDPositionToXY(int pos, int *x, int *y);
	bool TextPosToLineAndCol(int pos, int *lineNum, int *column);
	bool checkReadOnly();
	bool clickTracker(QMouseEvent *event, bool inDoubleClickHandler);
	bool deleteEmulatedTab();
	bool deletePendingSelection();
	bool emptyLinesVisible();
	bool maintainingAbsTopLineNum();
	bool pendingSelection();
	bool posToVisibleLineNum(int pos, int *lineNum);
	bool spanBackward(TextBuffer *buf, int startPos, const char *searchChars, bool ignoreSpace, int *foundPos);
	bool spanForward(TextBuffer *buf, int startPos, const char *searchChars, bool ignoreSpace, int *foundPos);
	bool updateHScrollBarRange();
	bool wrapLine(TextBuffer *buf, int bufOffset, int lineStartPos, int lineEndPos, int limitPos, int *breakAt,
	              int *charsAdded);
	bool wrapUsesCharacter(int lineEndPos);
	char *createIndentString(TextBuffer *buf, int bufOffset, int lineStartPos, int lineEndPos, int *length,
	                         int *column);
	char *wrapText(const char *startLine, const char *text, int bufOffset, int wrapMargin, int *breakBefore);
	int TextDCountBackwardNLines(int startPos, int nLines);
	int TextDCountForwardNLines(int startPos, unsigned nLines, bool startPosIsLineStart);
	int TextDCountLines(int startPos, int endPos, bool startPosIsLineStart);
	int TextDEndOfLine(int pos, bool startPosIsLineStart);
	int TextDGetInsertPosition() const;
	int TextDOffsetWrappedColumn(int row, int column);
	int TextDPosOfPreferredCol(int column, int lineStartPos);
	int TextDPreferredColumn(int *visLineNum, int *lineStartPos);
	int TextDStartOfLine(int pos);
	int TextDXYToPosition(int x, int y);
	int TextPosToXY(int pos, int *x, int *y);
	int endOfWord(int pos);
	int measurePropChar(char c, int colNum, int pos);
	int measureVisLine(int visLineNum);
	int startOfWord(int pos);
	int stringWidth(const char *string, const int length, const int style);
	int styleOfPos(int lineStartPos, int lineLen, int lineIndex, int dispIndex, char thisChar);
	int visLineLength(int visLineNum);
	int xyToPos(int x, int y, PositionTypes posType);
	void CopyToClipboard();
	void FinishBlockDrag();
	void InsertClipboard(PasteMode pasteMode);
	void MovePrimarySelection(PasteMode pasteMode);
	void SendSecondarySelection(bool removeAfter);
	void TakeMotifDestination();
	void TextColPasteClipboard();
	void TextCopyClipboard();
	void TextCutClipboard();
	void TextDBlankCursor();
	void TextDInsert(const char *text);
	void TextDMakeInsertPosVisible();
	void TextDOverstrike(const char *text);
	void TextDRedisplayRect(int left, int top, int width, int height);
	void TextDSetInsertPosition(int newPos);
	void TextDSetScroll(int topLineNum, int horizOffset);
	void TextDUnblankCursor();
	void TextDXYToUnconstrainedPosition(int x, int y, int *row, int *column);
	void TextInsertAtCursor(const char *chars, bool allowPendingDelete, bool allowWrap);
	void TextPasteClipboard();
	void adjustSelection(int x, int y);
	void backwardCharacterAP(MoveMode mode);
	void backwardWordAP(MoveMode mode);
	void beginningOfFileAP(MoveMode mode);
	void beginningOfLineAP(MoveMode mode);
	void blankCursorProtrusions();
	void calcLastChar();
	void calcLineStarts(int startLine, int endLine);
	void cancelDrag();
	void checkAutoScroll(int x, int y);
	void checkAutoShowInsertPos();
	void checkMoveSelectionChange(int startPos, MoveMode mode);
	void copyClipboardAP();
	void cutClipboardAP();
	void deleteNextCharacterAP();
	void deleteNextWordAP();
	void deletePreviousCharacterAP();
	void deletePreviousWordAP();
	void drawCursor(QPainter *painter, int x, int y);
	void drawString(QPainter *painter, int style, int x, int y, int toX, char *string, int nChars);
	void endDrag();
	void endDragAP();
	void endOfFileAP(MoveMode mode);
	void endOfLineAP(MoveMode mode);
	void extendAdjustAP(QMouseEvent *event);
	void extendRangeForStyleMods(int *start, int *end);
	void findLineEnd(int startPos, bool startPosIsLineStart, int *lineEnd, int *nextLineStart);
	void findWrapRange(const char *deletedText, int pos, int nInserted, int nDeleted, int *modRangeStart,
	                   int *modRangeEnd, int *linesInserted, int *linesDeleted);
	void forwardCharacterAP(MoveMode mode);
	void forwardWordAP(MoveMode mode);
	void hideOrShowHScrollBar();
	void keyMoveExtendSelection(int origPos, bool rectangular);
	void moveDestinationAP(QMouseEvent *event);
	void moveToAP(QMouseEvent *event);
	void moveToOrEndDragAP(QMouseEvent *event);
	void newlineAP();
	void newlineAndIndentAP();
	void newlineNoIndentAP();
	void nextPageAP(MoveMode mode);
	void offsetAbsLineNum(int oldFirstChar);
	void offsetLineStarts(int newTopLineNum);
	void pasteClipboardAP(PasteMode pasteMode);
	void previousPageAP(MoveMode mode);
	void processDownAP(MoveMode mode);
	void processTabAP();
	void processUpAP(MoveMode mode);
	void redisplayLine(QPainter *painter, int visLineNum, int leftClip, int rightClip, int leftCharIndex,
	                   int rightCharIndex);
	void redrawLineNumbers(bool clearAll);
	void resetAbsLineNum();
	void ringIfNecessary(bool silent);
	void selectAllAP();
	void selectLine();
	void selectWord(int pointerX);
	void setScroll(int topLineNum, int horizOffset, bool updateVScrollBar, bool updateHScrollBar);
	void simpleInsertAtCursor(const char *chars, bool allowPendingDelete);
	void textDRedisplayRange(int start, int end);
	void updateLineStarts(int pos, int charsInserted, int charsDeleted, int linesInserted, int linesDeleted,
	                      bool *scrolled);
	void updateVScrollBarRange();
	void wrappedLineCounter(const TextBuffer *buf, int startPos, int maxPos, int maxLines, bool startPosIsLineStart,
	                        int styleBufOffset, int *retPos, int *retLines, int *retLineStart, int *retLineEnd);
	void xyToUnconstrainedPos(int x, int y, int *row, int *column, PositionTypes posType);
	void TextDGetScroll(int *topLineNum, int *horizOffset);
	void adjustSecondarySelection(int x, int y);
	void measureDeletedLines(int pos, int nDeleted);
	void forwardParagraphAP(MoveMode mode);
	void backwardParagraphAP(MoveMode mode);
	void emitCursorMoved();
	void emitUnfinishedHighlightEncountered(int pos);
	void CancelBlockDrag();
	void ShiftSelection(ShiftDirection direction, bool byTab);
	int TextGetCursorPos();
	void TextSetCursorPos(int pos);
	char *ShiftText(char *text, ShiftDirection direction, bool tabsAllowed, int tabDist, int nChars, int *newLen);
	char *shiftLineLeft(char *line, int lineLen, int tabDist, int nChars);
	char *shiftLineRight(char *line, int lineLen, bool tabsAllowed, int tabDist, int nChars);
	int nextTab(int pos, int tabDist);
	int atTabStop(int pos, int tabDist);
	void shiftRect(ShiftDirection direction, bool byTab, int selStart, int selEnd, int rectStart, int rectEnd);
	void deleteToEndOfLineAP();
	void deleteToStartOfLineAP();
	void deselectAllAP();

private Q_SLOTS:
	void clickTimeout();
	void autoScrollTimeout();

private:
	TextBuffer *buffer_;
	int cursorPos_;
	int left_;
	int top_;
	QVector<int> lineStarts_;
	int firstChar_;
	int lastChar_;
	bool continuousWrap_;
	char unfinishedStyle_;
	int cursorX_;
	int cursorY_;
	bool cursorOn_;
	CursorStyles cursorStyle_;
	int cursorPreferredCol_;
	int wrapMargin_;
	int fixedFontWidth_;
	int topLineNum_;
	int absTopLineNum_;
	bool needAbsTopLineNum_;
	int lineNumWidth_;
	bool pendingDelete_;
	int cursorToHint_;
	bool autoShowInsertPos_;
	int cursorVPadding_;
	int horizOffset_;
	int nBufferLines_;
	bool suppressResync_;
	int nLinesDeleted_;
	int emulateTabs_;
	int emTabsBeforeCursor_;
	bool autoWrapPastedText_;
	int anchor_;
	int rectAnchor_;
	const char *delimiters_;
	bool autoWrap_;
	int overstrike_;
	bool autoIndent_;
	bool smartIndent_;
	DragStates dragState_;
	int btnDownX_;
	int btnDownY_;
	bool motifDestOwner_;
	bool readOnly_;
	int nVisibleLines_;
	int mouseX_;
	int mouseY_;
	bool modifyingTabDist_;

private:
	QTimer *cursorTimer_;
	QTimer *clickTimer_;
	QTimer *autoScrollTimer_;
	int clickCount_;
	QPoint clickPos_;
	QList<IHighlightHandler *> highlightHandlers_;
	QList<ICursorMoveHandler *> cursorMoveHandlers_;
	SyntaxHighlighter *syntaxHighlighter_;
};

#endif
