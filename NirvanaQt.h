
#ifndef NIRVANA_QT_H_
#define NIRVANA_QT_H_

#include "Types.h"
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

enum UndoTypes {
	UNDO_NOOP,
	ONE_CHAR_INSERT,
	ONE_CHAR_REPLACE,
	ONE_CHAR_DELETE,
	BLOCK_INSERT,
	BLOCK_REPLACE,
	BLOCK_DELETE
};

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

/* Record on undo list */
struct UndoInfo {
	UndoInfo *next; /* pointer to the next undo record */
	UndoTypes type;
	int startPos;
	int endPos;
	int oldLen;
	char_type *oldText;
	bool inUndo;          /* flag to indicate undo command on
	                     this record in progress.  Redirects
	                     SaveUndoInfo to save the next mod-
	                     ifications on the redo list instead
	                     of the undo list. */
	bool restoresToSaved; /* flag to indicate undoing this
	                                 operation will restore file to
	                                 last saved (unmodified) state */
};

class NirvanaQt : public QAbstractScrollArea, public IBufferModifiedHandler, public IPreDeleteHandler {
	Q_OBJECT
public:
	explicit NirvanaQt(QWidget *parent = 0);
	virtual ~NirvanaQt() override;

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

public Q_SLOTS:
	void shiftRight();
	void shiftLeft();
	void shiftRightByTabs();
	void shiftLeftByTabs();
	void deleteToStartOfLine();
	void deselectAll();
	void gotoMatching();
	void selectToMatching();

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
	String ShiftText(const String &text, ShiftDirection direction, bool tabsAllowed, int tabDist, int nChars, int *newLen);
	String createIndentString(TextBuffer *buf, int bufOffset, int lineStartPos, int lineEndPos, int *length, int *column);
	String fillParagraph(char_type *text, int leftMargin, int firstLineIndent, int rightMargin, int tabDist, bool allowTabs, char_type nullSubsChar, int *filledLen);
	String fillParagraphs(char_type *text, int rightMargin, int tabDist, bool useTabs, char_type nullSubsChar, int *filledLen, int alignWithFirst);
	String makeIndentString(int indent, int tabDist, bool allowTabs, int *nChars);
	String shiftLineLeft(const char_type *line, int lineLen, int tabDist, int nChars);
	String shiftLineRight(const char_type *line, int lineLen, bool tabsAllowed, int tabDist, int nChars);
	String wrapText(const char_type *startLine, const char_type *text, int bufOffset, int wrapMargin, int *breakBefore);
	UndoTypes determineUndoType(int nInserted, int nDeleted);
	bool GetSimpleSelection(TextBuffer *buf, int *left, int *right);
	bool TextDMoveDown(bool absolute);
	bool TextDMoveLeft();
	bool TextDMoveRight();
	bool TextDMoveUp(bool absolute);
	bool TextDPosToLineAndCol(int pos, int *lineNum, int *column);
	bool TextDPositionToXY(int pos, int *x, int *y);
	bool TextPosToLineAndCol(int pos, int *lineNum, int *column);
	bool WriteBackupFile();
	bool checkReadOnly();
	bool clickTracker(QMouseEvent *event, bool inDoubleClickHandler);
	bool deleteEmulatedTab();
	bool deletePendingSelection();
	bool emptyLinesVisible();
	bool findMatchingChar(char_type toMatch, void *styleToMatch, int charPos, int startLimit, int endLimit, int *matchPos);
	bool maintainingAbsTopLineNum();
	bool pendingSelection();
	bool posToVisibleLineNum(int pos, int *lineNum);
	bool spanBackward(TextBuffer *buf, int startPos, const char_type *searchChars, bool ignoreSpace, int *foundPos);
	bool spanForward(TextBuffer *buf, int startPos, const char_type *searchChars, bool ignoreSpace, int *foundPos);
	bool updateHScrollBarRange();
	bool wrapLine(TextBuffer *buf, int bufOffset, int lineStartPos, int lineEndPos, int limitPos, int *breakAt, int *charsAdded);
	bool wrapUsesCharacter(int lineEndPos);
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
	int TextFirstVisibleLine();
	int TextFirstVisiblePos();
	int TextGetCursorPos();
	int TextLastVisiblePos();
	int TextNumVisibleLines();
	int TextPosToXY(int pos, int *x, int *y);
	int TextVisibleWidth();
	int atTabStop(int pos, int tabDist);
	int endOfWord(int pos);
	int findLeftMargin(char_type *text, int length, int tabDist);
	int findParagraphEnd(TextBuffer *buf, int startPos);
	int findParagraphStart(TextBuffer *buf, int startPos);
	int measurePropChar(char_type c, int colNum, int pos);
	int measureVisLine(int visLineNum);
	int nextTab(int pos, int tabDist);
	int startOfWord(int pos);
	int stringWidth(const char_type *string, const int length, const int style);
	int styleOfPos(int lineStartPos, int lineLen, int lineIndex, int dispIndex, char_type thisChar);
	int updateLineNumDisp();
	int visLineLength(int visLineNum);
	int xyToPos(int x, int y, PositionTypes posType);
	void CancelBlockDrag();
	void CheckForChangesToFile();
	void ClearRedoList();
	void ClearUndoList();
	void CopyToClipboard();
	void FillSelection();
	void FinishBlockDrag();
	void GotoMatchingCharacter();
	void InsertClipboard(PasteMode pasteMode);
	void MakeSelectionVisible();
	void MovePrimarySelection(PasteMode pasteMode);
	void Redo();
	void RemoveBackupFile();
	void SaveUndoInformation(int pos, int nInserted, int nDeleted, const char_type *deletedText);
	void SelectToMatchingCharacter();
	void SendSecondarySelection(bool removeAfter);
	void SetWindowModified(bool modified);
	void ShiftSelection(ShiftDirection direction, bool byTab);
	void TakeMotifDestination();
	void TextColPasteClipboard();
	void TextCopyClipboard();
	void TextCutClipboard();
	void TextDBlankCursor();
	void TextDGetScroll(int *topLineNum, int *horizOffset);
	void TextDInsert(const char_type *text);
	void TextDMakeInsertPosVisible();
	void TextDOverstrike(const char_type *text);
	void TextDRedisplayRect(int left, int top, int width, int height);
	void TextDSetInsertPosition(int newPos);
	void TextDSetScroll(int topLineNum, int horizOffset);
	void TextDUnblankCursor();
	void TextDXYToUnconstrainedPosition(int x, int y, int *row, int *column);
	void TextGetScroll(int *topLineNum, int *horizOffset);
	void TextInsertAtCursor(const char_type *chars, bool allowPendingDelete, bool allowWrap);
	void TextPasteClipboard();
	void TextSetCursorPos(int pos);
	void TextSetScroll(int topLineNum, int horizOffset);
	void Undo();
	void UpdateMarkTable(int pos, int nInserted, int nDeleted);
	void UpdateStatsLine();
	void addRedoItem(UndoInfo *redo);
	void addUndoItem(UndoInfo *undo);
	void adjustSecondarySelection(int x, int y);
	void adjustSelection(int x, int y);
	void appendDeletedText(const char_type *deletedText, int deletedLen, int direction);
	void backwardCharacterAP(MoveMode mode);
	void backwardParagraphAP(MoveMode mode);
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
	void deleteToEndOfLineAP();
	void deleteToStartOfLineAP();
	void deselectAllAP();
	void drawCursor(QPainter *painter, int x, int y);
	void drawString(QPainter *painter, int style, int x, int y, int toX, char_type *string, int nChars);
	void emitCursorMoved();
	void emitUnfinishedHighlightEncountered(int pos);
	void endDrag();
	void endDragAP();
	void endOfFileAP(MoveMode mode);
	void endOfLineAP(MoveMode mode);
	void extendAdjustAP(QMouseEvent *event);
	void extendRangeForStyleMods(int *start, int *end);
	void findLineEnd(int startPos, bool startPosIsLineStart, int *lineEnd, int *nextLineStart);
	void findWrapRange(const char_type *deletedText, int pos, int nInserted, int nDeleted, int *modRangeStart, int *modRangeEnd, int *linesInserted, int *linesDeleted);
	void forwardCharacterAP(MoveMode mode);
	void forwardParagraphAP(MoveMode mode);
	void forwardWordAP(MoveMode mode);
	void freeUndoRecord(UndoInfo *undo);
	void hideOrShowHScrollBar();
	void keyMoveExtendSelection(int origPos, bool rectangular);
	void measureDeletedLines(int pos, int nDeleted);
	void modifiedCB(int pos, int nInserted, int nDeleted, int nRestyled, const char_type *deletedText);
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
	void redisplayLine(QPainter *painter, int visLineNum, int leftClip, int rightClip, int leftCharIndex, int rightCharIndex);
	void redoAP();
	void removeRedoItem();
	void removeUndoItem();
	void resetAbsLineNum();
	void ringIfNecessary(bool silent);
	void secondaryAdjustAP(QMouseEvent *event);
	void selectAllAP();
	void selectLine();
	void selectWord(int pointerX);
	void setScroll(int topLineNum, int horizOffset, bool updateVScrollBar, bool updateHScrollBar);
	void shiftRect(ShiftDirection direction, bool byTab, int selStart, int selEnd, int rectStart, int rectEnd);
	void simpleInsertAtCursor(const char_type *chars, bool allowPendingDelete);
	void textDRedisplayRange(int start, int end);
	void trimUndoList(int maxLength);
	void undoAP();
	void updateLineStarts(int pos, int charsInserted, int charsDeleted, int linesInserted, int linesDeleted, bool *scrolled);
	void updateVScrollBarRange();
	void wrappedLineCounter(const TextBuffer *buf, int startPos, int maxPos, int maxLines, bool startPosIsLineStart, int styleBufOffset, int *retPos, int *retLines, int *retLineStart, int *retLineEnd);
	void xyToUnconstrainedPos(int x, int y, int *row, int *column, PositionTypes posType);
    int getAbsTopLineNum();
    void redrawLineNumbers(QPainter *painter, bool clearAll);

private Q_SLOTS:
	void clickTimeout();
	void autoScrollTimeout();
	void cursorTimeout();

private:
	bool matchSyntaxBased_;
	TextBuffer *buffer_;
	int cursorPos_;
	int left_;
    int lineNumLeft_;
	int top_;
	QVector<int> lineStarts_;
	int firstChar_;
	int lastChar_;
	bool continuousWrap_;
	char_type unfinishedStyle_;
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
	UndoInfo *undo_;
	UndoInfo *redo_;
	bool undoModifiesSelection_;
	int undoOpCount_; /* count of stored undo operations */
	int undoMemUsed_; /* amount of memory (in bytes) dedicated to the undo list */
	bool ignoreModify_;
	bool autoSave_;
	bool wasSelected_;
	int autoSaveCharCount_;
	int autoSaveOpCount_;
	bool fileChanged_;

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
