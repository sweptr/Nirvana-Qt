
#include "NirvanaQt.h"
#include "SyntaxHighlighter.h"
#include "X11Colors.h"
#include <QApplication>
#include <QClipboard>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QShortcut>
#include <QTextLayout>
#include <QTimer>
#include <QtDebug>

namespace {

const QColor CursorColor = Qt::black;
const QColor DefaultBackground = QColor("#D5D1CF");
const QString DefaultFont = "Courier";

/* Number of pixels of motion from the initial (grab-focus) button press
   required to begin recognizing a mouse drag for the purpose of making a
   selection */
const int SelectThreshold = 5;

const int CursorInterval = 500;
const int DefaultFontSize = 12;
const int DefaultWidth = 80;
const int DefaultHeight = 20;
const int MaxDisplayLineLength = 1024;
const int NoCursorHint = -1;

/*
 * Count the number of newlines in a null-terminated text string;
 */
int countLines(const char_type *string) {
    if (!string) {
        return 0;
    }

    int lineCount = 0;

    for (const char_type *c = string; *c != '\0'; ++c) {
        if (*c == '\n') {
            ++lineCount;
        }
    }

    return lineCount;
}

#define UNDO_OP_LIMIT 400 /* normal limit for length of undo list */
#define FORWARD 1
#define REVERSE 2
#define N_MATCH_CHARS 13
#define N_FLASH_CHARS 6
#ifndef VMS
#define AUTOSAVE_CHAR_LIMIT                                                                                            \
    30 /* number of characters user can type before                                                                    \
NEdit generates a new backup file */
#else
#define AUTOSAVE_CHAR_LIMIT 80 /* set higher on VMS becaus saving is slower */
#endif                         /*VMS*/
#define AUTOSAVE_OP_LIMIT                                                                                              \
    8 /* number of distinct editing operations user                                                                    \
can do before NEdit gens. new backup file */

/* The accumulated list of undo operations can potentially consume huge
   amounts of memory.  These tuning parameters determine how much undo infor-
   mation is retained.  Normally, the list is kept between UNDO_OP_LIMIT and
   UNDO_OP_TRIMTO in length (when the list reaches UNDO_OP_LIMIT, it is
   trimmed to UNDO_OP_TRIMTO then allowed to grow back to UNDO_OP_LIMIT).
   When there are very large amounts of saved text held in the list,
   UNDO_WORRY_LIMIT and UNDO_PURGE_LIMIT take over and cause the list to
   be trimmed back further to keep its size down. */
#define UNDO_PURGE_LIMIT                                                                                               \
    15000000                /* If undo list gets this large (in bytes),                                                \
               trim it to length of UNDO_PURGE_TRIMTO */
#define UNDO_PURGE_TRIMTO 1 /* Amount to trim the undo list in a purge */
#define UNDO_WORRY_LIMIT                                                                                               \
    2000000 /* If undo list gets this large (in bytes),                                                                \
trim it to length of UNDO_WORRY_TRIMTO */
#define UNDO_WORRY_TRIMTO                                                                                              \
    5                     /* Amount to trim the undo list when memory                                                  \
                 use begins to get serious */
#define UNDO_OP_LIMIT 400 /* normal limit for length of undo list */
#define UNDO_OP_TRIMTO                                                                                                 \
    200 /* size undo list is normally trimmed to                                                                       \
when it exceeds UNDO_OP_TRIMTO in length */

enum SearchDirection { SEARCH_FORWARD, SEARCH_BACKWARD };

struct charMatchTable {
    char_type c;
    char_type match;
    SearchDirection direction;
};

charMatchTable MatchingChars[N_MATCH_CHARS] = {
    {'{', '}', SEARCH_FORWARD},
    {'}', '{', SEARCH_BACKWARD},
    {'(', ')', SEARCH_FORWARD},
    {')', '(', SEARCH_BACKWARD},
    {'[', ']', SEARCH_FORWARD},
    {']', '[', SEARCH_BACKWARD},
    {'<', '>', SEARCH_FORWARD},
    {'>', '<', SEARCH_BACKWARD},
    {'/', '/', SEARCH_FORWARD},
    {'"', '"', SEARCH_FORWARD},
    {'\'', '\'', SEARCH_FORWARD},
    {'`', '`', SEARCH_FORWARD},
    {'\\', '\\', SEARCH_FORWARD},
};

bool isModifier(QKeyEvent *e) {
    if (!e)
        return false;
    switch (e->key()) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
        return true;
    default:
        return false;
    }
}

bool isPrintableText(const QString &text) {
    return !text.isEmpty() && (text.at(0).isPrint() || text.at(0) == QLatin1Char('\t'));
}
}

//------------------------------------------------------------------------------
// Name: NirvanaQt
//------------------------------------------------------------------------------
NirvanaQt::NirvanaQt(QWidget *parent)
    : QAbstractScrollArea(parent), cursorTimer_(new QTimer(this)), clickTimer_(new QTimer(this)),
      autoScrollTimer_(new QTimer(this)) {

    QPalette pal(viewport()->palette());

    // set black background
    pal.setColor(QPalette::Base, DefaultBackground);
    viewport()->setAutoFillBackground(true);
    viewport()->setBackgroundRole(QPalette::Base);
    viewport()->setPalette(pal);

    setFont(QFont(DefaultFont, DefaultFontSize));

    setContextMenuPolicy(Qt::CustomContextMenu);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    connect(verticalScrollBar(), SIGNAL(valueChanged(int)), this, SLOT(verticalScrollBar_valueChanged(int)));
    connect(horizontalScrollBar(), SIGNAL(valueChanged(int)), this, SLOT(horizontalScrollBar_valueChanged(int)));
    connect(this, SIGNAL(customContextMenuRequested(const QPoint &)), this,
            SLOT(customContextMenuRequested(const QPoint &)));

    connect(cursorTimer_, SIGNAL(timeout()), this, SLOT(cursorTimeout()));

    clickTimer_->setSingleShot(true);
    connect(clickTimer_, SIGNAL(timeout()), this, SLOT(clickTimeout()));

    autoScrollTimer_->setSingleShot(true);
    connect(autoScrollTimer_, SIGNAL(timeout()), this, SLOT(autoScrollTimeout()));

    buffer_ = new TextBuffer();
    syntaxHighlighter_ = new SyntaxHighlighter();
    absTopLineNum_ = 1;
    anchor_ = -1;
    autoIndent_ = false;
    autoShowInsertPos_ = true;
    autoWrapPastedText_ = false;
    autoWrap_ = false;
    btnDownX_ = -1;
    btnDownY_ = -1;
    clickCount_ = 0;
    continuousWrap_ = false;
    cursorOn_ = true;
    cursorPos_ = 0;
    cursorPreferredCol_ = -1;
    cursorStyle_ = NORMAL_CURSOR;
    cursorToHint_ = NoCursorHint;
    cursorVPadding_ = 0;
    cursorX_ = 0;
    cursorY_ = 0;
    delimiters_ = _T("(.,/\\`'!|@#%^&*()-=+{}[]\":;<>?~ \t\n)");
    dragState_ = NOT_CLICKED;
    emTabsBeforeCursor_ = 0;
    emulateTabs_ = 0;
    firstChar_ = 0;
    fixedFontWidth_ = viewport()->fontMetrics().width('X'); // TODO(eteran): properly detect variable width fonts
    horizOffset_ = 0;
    lastChar_ = 0;
    left_ = 5;
    lineNumWidth_ = 0; // TODO(eteran): doesn't left_ seem a bit redundant to this? I would assume that they would basically be in sync most of the time, I suppose we may want to space them a bit?
    lineNumLeft_ = 0;
    motifDestOwner_ = false;
    mouseX_ = 0;
    mouseY_ = 0;
    nBufferLines_ = 0;
    nLinesDeleted_ = 0;
    nVisibleLines_ = visibleRows();
    needAbsTopLineNum_ = false;
    overstrike_ = 0;
    pendingDelete_ = true; // TODO(eteran): what code flips this?
    readOnly_ = false;
    rectAnchor_ = -1;
    smartIndent_ = false;
    suppressResync_ = false;
    topLineNum_ = 1;
    top_ = 0;
    unfinishedStyle_ = ASCII_A;
    wrapMargin_ = 0;
    modifyingTabDist_ = false;
    matchSyntaxBased_ = false;
    undo_ = nullptr;
    redo_ = nullptr;
    undoModifiesSelection_ = true;
    undoOpCount_ = 0;
    undoMemUsed_ = 0;
    ignoreModify_ = false;
    autoSave_ = false;
    wasSelected_ = false;
    autoSaveCharCount_ = 0;
    autoSaveOpCount_ = 0;
    fileChanged_ = false;


    lineStarts_.resize(nVisibleLines_);

    /* Attach the callback to the text buffer for receiving modification
       information */
    if (buffer_) {

        if (syntaxHighlighter_) {
            highlightHandlers_.push_back(syntaxHighlighter_);

            buffer_->BufAddModifyCB(syntaxHighlighter_); // TODO(eteran): move this to
                                                         // the SyntaxHighlighter
                                                         // contructor?
        }
        buffer_->BufAddModifyCB(this);
        buffer_->BufAddPreDeleteCB(this);
    }

    // set default size
    resize(DefaultWidth * fixedFontWidth_,
           DefaultHeight * (viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent()));

    cursorTimer_->start(CursorInterval);
}

//------------------------------------------------------------------------------
// Name: ~NirvanaQt
//------------------------------------------------------------------------------
NirvanaQt::~NirvanaQt() {
    delete buffer_;
}

//------------------------------------------------------------------------------
// Name: clickTimeout
//------------------------------------------------------------------------------
void NirvanaQt::cursorTimeout() {
    cursorOn_ = !cursorOn_;
    textDRedisplayRange(cursorPos_ - 1, cursorPos_ + 1);
}

//------------------------------------------------------------------------------
// Name: clickTimeout
//------------------------------------------------------------------------------
void NirvanaQt::clickTimeout() {
    clickCount_ = 0;
}

//------------------------------------------------------------------------------
// Name: font
//------------------------------------------------------------------------------
const QFont &NirvanaQt::font() const {
    return QAbstractScrollArea::font();
}

//------------------------------------------------------------------------------
// Name: setFont
//------------------------------------------------------------------------------
void NirvanaQt::setFont(const QFont &font) {
    QAbstractScrollArea::setFont(font);

    if (font.fixedPitch()) {
        fixedFontWidth_ = viewport()->fontMetrics().width('X');
    } else {
        fixedFontWidth_ = -1;
    }

    setViewportMargins(fixedFontWidth_ / 2, 0, 0, 0);
}

//------------------------------------------------------------------------------
// Name: paintEvent
//------------------------------------------------------------------------------
void NirvanaQt::paintEvent(QPaintEvent *event) {

    QPainter painter(viewport());

    const int fontHeight = viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent();
    const int y1 = event->rect().top() / fontHeight;
    const int y2 = event->rect().bottom() / fontHeight;
    const int x1 = event->rect().left();
    const int x2 = event->rect().right();

    redrawLineNumbers(&painter, true);

    // make it so we don't override things like the line number area
    painter.setClipRect(QRectF(left_, top_, viewport()->width() - left_, viewport()->height() - top_));

    for (int i = y1; i < y2; ++i) {

        if (i < lineStarts_.size()) {
            redisplayLine(&painter, i, x1, x2, 0, INT_MAX);
        }
    }


}

//------------------------------------------------------------------------------
// Name: keyPressEvent
//------------------------------------------------------------------------------
void NirvanaQt::keyPressEvent(QKeyEvent *event) {

    if (isModifier(event)) {
        return;
    }

    if (event->matches(QKeySequence::Copy)) {
        copyClipboardAP();
    } else if (event->matches(QKeySequence::Cut)) {
        cutClipboardAP();
    } else if (event->matches(QKeySequence::Paste)) {
        pasteClipboardAP(PasteStandard);
    } else if (event->matches(QKeySequence::SelectNextChar)) {
        forwardCharacterAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectPreviousChar)) {
        backwardCharacterAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectNextLine)) {
        processDownAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectPreviousLine)) {
        processUpAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectNextWord)) {
        forwardWordAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectPreviousWord)) {
        backwardWordAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectNextPage)) {
        nextPageAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectPreviousPage)) {
        previousPageAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectStartOfLine)) {
        beginningOfLineAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectEndOfLine)) {
        endOfLineAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectStartOfDocument)) {
        beginningOfFileAP(MoveExtend);
    } else if (event->matches(QKeySequence::SelectEndOfDocument)) {
        endOfFileAP(MoveExtend);
    } else if ((event->matches(QKeySequence::SelectAll)) ||
               (event->key() == Qt::Key_Slash && event->modifiers() == Qt::ControlModifier)) {
        selectAllAP();
    } else if (event->matches(QKeySequence::Undo)) {
        undoAP();
    } else if (event->matches(QKeySequence::Redo)) {
        redoAP();
    } else if (event->matches(QKeySequence::Delete)) {
        deleteNextCharacterAP();
    } else if (event->matches(QKeySequence::DeleteStartOfWord)) {
        deletePreviousWordAP();
    } else if (event->matches(QKeySequence::DeleteEndOfWord)) {
#if 0
        deleteNextWordAP();
#else
        deleteToEndOfLineAP();
#endif
    } else if (event->matches(QKeySequence::MoveToNextChar)) {
        forwardCharacterAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToPreviousChar)) {
        backwardCharacterAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToNextLine)) {
        processDownAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToPreviousLine)) {
        processUpAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToEndOfDocument)) {
        endOfFileAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToEndOfLine)) {
        endOfLineAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToStartOfDocument)) {
        beginningOfFileAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToStartOfLine)) {
        beginningOfLineAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToNextWord)) {
        forwardWordAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToPreviousWord)) {
        backwardWordAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToNextPage)) {
        nextPageAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::MoveToPreviousPage)) {
        previousPageAP(MoveNoExtend);
    } else if (event->matches(QKeySequence::InsertParagraphSeparator)) {
        // Normal Newline
        newlineAP();
    } else if (event->matches(QKeySequence::InsertLineSeparator)) {
        // Shift + Return
        newlineNoIndentAP();
    } else if ((event->key() == Qt::Key_9) && (event->modifiers() == Qt::ControlModifier)) {
        shiftLeft();
    } else if ((event->key() == Qt::Key_0) && (event->modifiers() == Qt::ControlModifier)) {
        shiftRight();
    } else if ((event->key() == Qt::Key_ParenLeft) &&
               (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))) {
        shiftLeftByTabs();
    } else if ((event->key() == Qt::Key_ParenRight) &&
               (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))) {
        // NOTE(eteran): on windows "Ctrl+Shift+0" conflicts with language switching shortcuts
        // https://bugreports.qt.io/browse/QTBUG-7463
        shiftRightByTabs();
    } else if ((event->key() == Qt::Key_U) && (event->modifiers() == Qt::ControlModifier)) {
        deleteToStartOfLineAP();
    } else if ((event->key() == Qt::Key_Backslash) && (event->modifiers() == Qt::ControlModifier)) {
        deselectAllAP();
    } else if ((event->key() == Qt::Key_M) && (event->modifiers() == Qt::ControlModifier)) {
        gotoMatching();
    } else if ((event->key() == Qt::Key_M) && (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))) {
        selectToMatching();
    } else if ((event->key() == Qt::Key_Up) && (event->modifiers() == Qt::ControlModifier)) {
        backwardParagraphAP(MoveNoExtend);
    } else if ((event->key() == Qt::Key_Up) && (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))) {
        backwardParagraphAP(MoveExtend);
    } else if ((event->key() == Qt::Key_Up) && (event->modifiers() == (Qt::AltModifier | Qt::ShiftModifier))) {
        processUpAP(MoveExtendRect);
    } else if ((event->key() == Qt::Key_Down) && (event->modifiers() == Qt::ControlModifier)) {
        forwardParagraphAP(MoveNoExtend);
    } else if ((event->key() == Qt::Key_Down) && (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))) {
        forwardParagraphAP(MoveExtend);
    } else if ((event->key() == Qt::Key_Down) && (event->modifiers() == (Qt::AltModifier | Qt::ShiftModifier))) {
        processDownAP(MoveExtendRect);
    } else if ((event->key() == Qt::Key_Left) && (event->modifiers() == (Qt::AltModifier | Qt::ShiftModifier))) {
        backwardCharacterAP(MoveExtendRect);
    } else if ((event->key() == Qt::Key_Left) && (event->modifiers() == (Qt::AltModifier | Qt::ShiftModifier))) {
        forwardCharacterAP(MoveExtendRect);
    } else if ((event->key() == Qt::Key_Tab) && (event->modifiers() == Qt::ControlModifier)) {
        qDebug() << "TODO(eteran): implement this";
    } else if (event->key() == Qt::Key_Tab) {
        processTabAP();
    } else if (event->key() == Qt::Key_Return && (event->modifiers() == Qt::ControlModifier)) {
        // standard "enter"
        newlineAndIndentAP();
    } else if (event->key() == Qt::Key_Enter && (event->modifiers() == Qt::ControlModifier)) {
        // number pad "enter"
        qDebug() << "TODO(eteran): what does this do in nedit?";
    } else if (event->key() == Qt::Key_Backspace) {
        deletePreviousCharacterAP();
    } else {
        QString s = event->text();

        // TODO(eteran): nedit actually allows you to type some unprintable characters,
        // should we allow this here? Do some special handling elsewhere? Forbid it since
        // it is arguably a mis-feature?
        if (isPrintableText(s)) {
            TextInsertAtCursor(qPrintable(s), true, false);
        }
    }

    viewport()->update();
}

void NirvanaQt::deleteToStartOfLine() {
    deleteToStartOfLineAP();
}

void NirvanaQt::deselectAll() {
    deselectAllAP();
}

void NirvanaQt::shiftRight() {
    ShiftSelection(SHIFT_RIGHT, false);
}

void NirvanaQt::shiftLeft() {
    ShiftSelection(SHIFT_LEFT, false);
}

void NirvanaQt::shiftRightByTabs() {
    ShiftSelection(SHIFT_RIGHT, true);
}

void NirvanaQt::shiftLeftByTabs() {
    ShiftSelection(SHIFT_LEFT, true);
}

//------------------------------------------------------------------------------
// Name: keyReleaseEvent
//------------------------------------------------------------------------------
void NirvanaQt::keyReleaseEvent(QKeyEvent *event) {
    Q_UNUSED(event);
}

//------------------------------------------------------------------------------
// Name: visibleRows
//------------------------------------------------------------------------------
int NirvanaQt::visibleRows() const {
    const int h = viewport()->height();
    const int count =
        static_cast<int>((h / (viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent())) + 0.5);
    return count;
}

//------------------------------------------------------------------------------
// Name: visibleColumns
//------------------------------------------------------------------------------
int NirvanaQt::visibleColumns() const {
    const int w = viewport()->width();
    const int count = static_cast<int>((w / fixedFontWidth_) + 0.5);
    return count;
}

//------------------------------------------------------------------------------
// Name: resizeEvent
//------------------------------------------------------------------------------
void NirvanaQt::resizeEvent(QResizeEvent *event) {

    nVisibleLines_ = visibleRows();

    bool redrawAll = false;
    const int oldWidth = event->oldSize().width();
    const int oldHeight = event->oldSize().height();
    const int oldVisibleLines = static_cast<int>(
        (oldHeight / (viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent())) + 0.5);

    /* In continuous wrap mode, a change in width affects the total number of
       lines in the buffer, and can leave the top line number incorrect, and
       the top character no longer pointing at a valid line start */
    if (continuousWrap_ && wrapMargin_ == 0 && viewport()->width() != oldWidth) {
        int oldFirstChar = firstChar_;
        nBufferLines_ = TextDCountLines(0, buffer_->BufGetLength(), true);
        firstChar_ = TextDStartOfLine(firstChar_);
        topLineNum_ = TextDCountLines(0, firstChar_, true) + 1;
        redrawAll = true;
        offsetAbsLineNum(oldFirstChar);
    }

    /* reallocate and update the line starts array, which may have changed
       size and/or contents. (contents can change in continuous wrap mode
       when the width changes, even without a change in height) */
    lineStarts_.resize(nVisibleLines_);

    calcLineStarts(0, nVisibleLines_);
    calcLastChar();

#if 0
    /* if the window became shorter, there may be partially drawn
       text left at the bottom edge, which must be cleaned up */
    if (oldVisibleLines > newVisibleLines && exactHeight != height) {
        XClearArea(textD->left, textD->top + exactHeight,  textD->width, height - exactHeight, false);
    }
#endif

    /* if the window became taller, there may be an opportunity to display
       more text by scrolling down */
    if (oldVisibleLines < nVisibleLines_ && topLineNum_ + nVisibleLines_ > nBufferLines_) {
        setScroll(qMax(1, nBufferLines_ - nVisibleLines_ + 2 + cursorVPadding_), horizOffset_, false, false);
    }

    /* Update the scroll bar page increment size (as well as other scroll
       bar parameters.  If updating the horizontal range caused scrolling,
       redraw */
    updateVScrollBarRange();
    if (updateHScrollBarRange()) {
        redrawAll = true;
    }

    /* If a full redraw is needed */
    if (redrawAll) {
        viewport()->update();
    }

    /* Decide if the horizontal scroll bar needs to be visible */
    hideOrShowHScrollBar();

    /* Refresh the line number display to draw more line numbers, or
       erase extras */
#if 0
    redrawLineNumbers(true);
#else
    // TODO(eteran): force a redraw of the line numbers!
#endif

#if 0
    /* Redraw the calltip */
    TextDRedrawCalltip(0);
#endif
}

/*
 *
 */
bool NirvanaQt::clickTracker(QMouseEvent *event, bool inDoubleClickHandler) {
    // track mouse click count
    clickTimer_->start(QApplication::doubleClickInterval());

    if (clickCount_ < 4 && clickPos_ == event->pos()) {
        clickCount_++;
    } else {
        clickCount_ = 0;
    }

    clickPos_ = event->pos();

    switch (clickCount_) {
    case 1:
        return true;
    case 2:
        if (inDoubleClickHandler) {
            return true;
        } else {
            mouseDoubleClickEvent(event);
            return false;
        }
        break;
    case 3:
        mouseTripleClickEvent(event);
        return false;
    case 4:
        mouseQuadrupleClickEvent(event);
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------
// Name: mousePressEvent
//------------------------------------------------------------------------------
void NirvanaQt::mousePressEvent(QMouseEvent *event) {

    if (!clickTracker(event, false)) {
        return;
    }

    if (event->button() == Qt::LeftButton) {

        int row;
        int column;

        /* Indicate state for future events, PRIMARY_CLICKED indicates that
           the proper initialization has been done for primary dragging and/or
           multi-clicking.  Also record the timestamp for multi-click processing */
        dragState_ = PRIMARY_CLICKED;

        /* Become owner of the MOTIF_DESTINATION selection, making this widget
           the designated recipient of secondary quick actions in Motif XmText
           widgets and in other NEdit text widgets */
        TakeMotifDestination();

        /* Clear any existing selections */
        buffer_->BufUnselect();

        /* Move the cursor to the pointer location */
        moveDestinationAP(event);

        /* Record the site of the initial button press and the initial character
           position so subsequent motion events and clicking can decide when and
           where to begin a primary selection */
        btnDownX_ = event->x();
        btnDownY_ = event->y();
        anchor_ = TextDGetInsertPosition();

        TextDXYToUnconstrainedPosition(event->x(), event->y(), &row, &column);
        column = TextDOffsetWrappedColumn(row, column);
        rectAnchor_ = column;

        // TODO(eteran): should this be needed?
        viewport()->update();
    } else if (event->button() == Qt::MiddleButton) {
        qDebug() << "Middle Click";
    }
}

/*
 *
 */
void NirvanaQt::moveToOrEndDragAP(QMouseEvent *event) {
    int dragState = dragState_;

    if (dragState != PRIMARY_BLOCK_DRAG) {
        moveToAP(event);
        return;
    }

    FinishBlockDrag();
}

/*
 *
 */
void NirvanaQt::endDragAP() {
    if (dragState_ == PRIMARY_BLOCK_DRAG) {
        FinishBlockDrag();
    } else {
        endDrag();
    }
}

//------------------------------------------------------------------------------
// Name: mouseReleaseEvent
//------------------------------------------------------------------------------
void NirvanaQt::mouseReleaseEvent(QMouseEvent *event) {
    Q_UNUSED(event);
    endDragAP();
    viewport()->update();
}

//------------------------------------------------------------------------------
// Name: mouseMoveEvent
//------------------------------------------------------------------------------
void NirvanaQt::mouseMoveEvent(QMouseEvent *event) {
    extendAdjustAP(event);
    viewport()->update();
}

//------------------------------------------------------------------------------
// Name: mouseDoubleClickEvent
//------------------------------------------------------------------------------
void NirvanaQt::mouseDoubleClickEvent(QMouseEvent *event) {

    if (event->button() == Qt::LeftButton) {
        // track mouse click count
        if (!clickTracker(event, true)) {
            return;
        }

        selectWord(event->x());
        emitCursorMoved();
        viewport()->update();
    }
}

//------------------------------------------------------------------------------
// Name: mouseTripleClickEvent
//------------------------------------------------------------------------------
void NirvanaQt::mouseTripleClickEvent(QMouseEvent *event) {

    if (event->button() == Qt::LeftButton) {
        selectLine();
        emitCursorMoved();
        viewport()->update();
    }
}

//------------------------------------------------------------------------------
// Name: mouseQuadrupleClickEvent
//------------------------------------------------------------------------------
void NirvanaQt::mouseQuadrupleClickEvent(QMouseEvent *event) {

    if (event->button() == Qt::LeftButton) {
        buffer_->BufSelect(0, buffer_->BufGetLength());
        viewport()->update();
    }
}

//------------------------------------------------------------------------------
// Name: customContextMenuRequested
//------------------------------------------------------------------------------
void NirvanaQt::customContextMenuRequested(const QPoint &pos) {
    auto menu = new QMenu(this);
    menu->addAction(tr("Undo"));
    menu->addAction(tr("Redo"));
    menu->addAction(tr("Cut"));
    menu->addAction(tr("Copy"));
    menu->addAction(tr("Paste"));
    menu->exec(mapToParent(pos));
}

/*
** Line breaks in continuous wrap mode usually happen at newlines or
** whitespace.  This line-terminating character is not included in line
** width measurements and has a special status as a non-visible character.
** However, lines with no whitespace are wrapped without the benefit of a
** line terminating character, and this distinction causes endless trouble
** with all of the text display code which was originally written without
** continuous wrap mode and always expects to wrap at a newline character.
**
** Given the position of the end of the line, as returned by TextDEndOfLine
** or BufEndOfLine, this returns true if there is a line terminating
** character, and false if there's not.  On the last character in the
** buffer, this function can't tell for certain whether a trailing space was
** used as a wrap point, and just guesses that it wasn't.  So if an exact
** accounting is necessary, don't use this function.
*/
bool NirvanaQt::wrapUsesCharacter(int lineEndPos) {
    if (!continuousWrap_ || lineEndPos == buffer_->BufGetLength())
        return true;

    char_type c = buffer_->BufGetCharacter(lineEndPos);
    return c == '\n' || ((c == '\t' || c == ' ') && lineEndPos + 1 != buffer_->BufGetLength());
}

/*
** Return the length of a line (number of displayable characters) by examining
** entries in the line starts array rather than by scanning for newlines
*/
int NirvanaQt::visLineLength(int visLineNum) {
    int lineStartPos = lineStarts_[visLineNum];

    if (lineStartPos == -1) {
        return 0;
    }

    if (visLineNum + 1 >= nVisibleLines_) {
        return lastChar_ - lineStartPos;
    }

    int nextLineStart = lineStarts_[visLineNum + 1];

    if (nextLineStart == -1) {
        return lastChar_ - lineStartPos;
    }

    if (wrapUsesCharacter(nextLineStart - 1)) {
        return nextLineStart - 1 - lineStartPos;
    }

    return nextLineStart - lineStartPos;
}

/*
** Redisplay the text on a single line represented by "visLineNum" (the
** number of lines down from the top of the display), limited by
** "leftClip" and "rightClip" window coordinates and "leftCharIndex" and
** "rightCharIndex" character positions (not including the character at
** position "rightCharIndex").
**
** The cursor is also drawn if it appears on the line.
*/
void NirvanaQt::redisplayLine(QPainter *painter, int visLineNum, int leftClip, int rightClip, int leftCharIndex, int rightCharIndex) {
    int startX;
    int charIndex;
    int lineLen;
    int charWidth;
    int charLen;
    int startIndex;
    int style;
    int outStartIndex;
    int cursorX = 0;
    bool hasCursor = false;
    int dispIndexOffset;
    char_type expandedChar[MAX_EXP_CHAR_LEN];
    char_type outStr[MaxDisplayLineLength];
    char_type *lineStr;

    /* If line is not displayed, skip it */
    if (visLineNum < 0 || visLineNum >= nVisibleLines_) {
        return;
    }

    /* Shrink the clipping range to the active display area */
    leftClip = qMax(left_, leftClip);
    rightClip = qMin(rightClip, left_ + viewport()->width());

    if (leftClip > rightClip) {
        return;
    }

    /* Calculate y coordinate of the string to draw */
    int y = top_ + visLineNum * (viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent());

    /* Get the text, length, and  buffer position of the line to display */
    int lineStartPos = lineStarts_[visLineNum];
    if (lineStartPos == -1) {
        lineLen = 0;
        lineStr = nullptr;
    } else {
        lineLen = visLineLength(visLineNum);
        lineStr = buffer_->BufGetRange(lineStartPos, lineStartPos + lineLen);
    }

    /* Space beyond the end of the line is still counted in units of characters
     * of a standardized character width (this is done mostly because style
     * changes based on character position can still occur in this region due
     * to rectangular selections).  stdCharWidth must be non-zero to prevent a
     * potential infinite loop if x does not advance
     */
    int stdCharWidth = fixedFontWidth_;
    Q_ASSERT(stdCharWidth > 0 && "Internal Error, bad font measurement");

    /* Rectangular selections are based on "real" line starts (after a newline
     * or start of buffer).  Calculate the difference between the last newline
     * position and the line start we're using.  Since scanning back to find a
     * newline is expensive, only do so if there's actually a rectangular
     * selection which needs it
     */
    if (continuousWrap_ &&
        (rangeTouchesRectSel(&buffer_->BufGetPrimarySelection(), lineStartPos, lineStartPos + lineLen) ||
         rangeTouchesRectSel(&buffer_->BufGetSecondarySelection(), lineStartPos, lineStartPos + lineLen) ||
         rangeTouchesRectSel(&buffer_->BufGetHighlight(), lineStartPos, lineStartPos + lineLen))) {

        dispIndexOffset = buffer_->BufCountDispChars(buffer_->BufStartOfLine(lineStartPos), lineStartPos);
    } else {
        dispIndexOffset = 0;
    }

    /* Step through character positions from the beginning of the line (even if
     * that's off the left edge of the displayed area) to find the first
     * character position that's not clipped, and the x coordinate for drawing
     * that character
     */
    int x = left_ - horizOffset_;
    int outIndex = 0;

    for (charIndex = 0;; charIndex++) {
        char_type baseChar = '\0';
        charLen = charIndex >= lineLen ? 1 : TextBuffer::BufExpandCharacter(baseChar = lineStr[charIndex], outIndex,
                                                                            expandedChar, buffer_->BufGetTabDistance(),
                                                                            buffer_->BufGetNullSubsChar());
        style = styleOfPos(lineStartPos, lineLen, charIndex, outIndex + dispIndexOffset, baseChar);
        charWidth = charIndex >= lineLen ? stdCharWidth : stringWidth(expandedChar, charLen, style);

        if (x + charWidth >= leftClip && charIndex >= leftCharIndex) {
            startIndex = charIndex;
            outStartIndex = outIndex;
            startX = x;
            break;
        }

        x += charWidth;
        outIndex += charLen;
    }

    /* Scan character positions from the beginning of the clipping range, and
     * draw parts whenever the style changes (also note if the cursor is on
     * this line, and where it should be drawn to take advantage of the x
     * position which we've gone to so much trouble to calculate)
     */
    char_type *outPtr = outStr;
    outIndex = outStartIndex;
    x = startX;
    for (charIndex = startIndex; charIndex < rightCharIndex; charIndex++) {
        if (lineStartPos + charIndex == cursorPos_) {
            if (charIndex < lineLen || (charIndex == lineLen && cursorPos_ >= buffer_->BufGetLength())) {
                hasCursor = true;
                cursorX = x - 1;
            } else if (charIndex == lineLen) {
                if (wrapUsesCharacter(cursorPos_)) {
                    hasCursor = true;
                    cursorX = x - 1;
                }
            }
        }

        char_type baseChar = '\0';
        charLen = charIndex >= lineLen ? 1 : TextBuffer::BufExpandCharacter(baseChar = lineStr[charIndex], outIndex,
                                                                            expandedChar, buffer_->BufGetTabDistance(),
                                                                            buffer_->BufGetNullSubsChar());
        int charStyle = styleOfPos(lineStartPos, lineLen, charIndex, outIndex + dispIndexOffset, baseChar);

        for (int i = 0; i < charLen; i++) {
            if (i != 0 && charIndex < lineLen && lineStr[charIndex] == '\t') {
                charStyle = styleOfPos(lineStartPos, lineLen, charIndex, outIndex + dispIndexOffset, '\t');
            }

            if (charStyle != style) {
                drawString(painter, style, startX, y, x, outStr, outPtr - outStr);
                outPtr = outStr;
                startX = x;
                style = charStyle;
            }

            if (charIndex < lineLen) {
                *outPtr = expandedChar[i];
                charWidth = stringWidth(&expandedChar[i], 1, charStyle);
            } else {
                charWidth = stdCharWidth;
            }

            outPtr++;
            x += charWidth;
            outIndex++;
        }

        if (outPtr - outStr + MAX_EXP_CHAR_LEN >= MaxDisplayLineLength || x >= rightClip) {
            break;
        }
    }

    /* Draw the remaining style segment */
    drawString(painter, style, startX, y, x, outStr, outPtr - outStr);

    /* Draw the cursor if part of it appeared on the redisplayed part of
    this line.  Also check for the cases which are not caught as the
    line is scanned above: when the cursor appears at the very end
    of the redisplayed section. */
    int y_orig = cursorY_;
    if (cursorOn_) {
        if (hasCursor) {
            drawCursor(painter, cursorX, y);
        } else if (charIndex < lineLen && (lineStartPos + charIndex + 1 == cursorPos_) && x == rightClip) {
            if (cursorPos_ >= buffer_->BufGetLength()) {
                drawCursor(painter, x - 1, y);
            } else {
                if (wrapUsesCharacter(cursorPos_)) {
                    drawCursor(painter, x - 1, y);
                }
            }
        } else if ((lineStartPos + rightCharIndex) == cursorPos_) {
            drawCursor(painter, x - 1, y);
        }
    }

    /* If the y position of the cursor has changed, redraw the calltip */
    if (hasCursor && (y_orig != cursorY_ || y_orig != y)) {
#if 0
        TextDRedrawCalltip(0);
#endif
    }

    delete[] lineStr;
}

/*
** Return true if the selection "sel" is rectangular, and touches a
** buffer position withing "rangeStart" to "rangeEnd"
*/
bool NirvanaQt::rangeTouchesRectSel(Selection *sel, int rangeStart, int rangeEnd) {
    return sel->selected && sel->rectangular && sel->end >= rangeStart && sel->start <= rangeEnd;
}

/*
** Find the width of a string in the font of a particular style
*/
int NirvanaQt::stringWidth(const char_type *string, const int length, const int style) {
#if 0
    XFontStruct *fs;

    if (style & STYLE_LOOKUP_MASK)
        fs = textD->styleTable[(style & STYLE_LOOKUP_MASK) - ASCII_A].font;
    else
        fs = textD->fontStruct;
    return XTextWidth(fs, (char_type *) string, (int) length);
#else
    Q_UNUSED(style);
    // TODO(eteran): take into account style?
    return viewport()->fontMetrics().width(QString::fromLatin1(string, length));
#endif
}

/*
** Determine the drawing method to use to draw a specific character from "buf".
** "lineStartPos" gives the character index where the line begins, "lineIndex",
** the number of characters past the beginning of the line, and "dispIndex",
** the number of displayed characters past the beginning of the line.  Passing
** lineStartPos of -1 returns the drawing style for "no text".
**
** Why not just: styleOfPos(pos)?  Because style applies to blank areas
** of the window beyond the text boundaries, and because this routine must also
** decide whether a position is inside of a rectangular selection, and do so
** efficiently, without re-counting character positions from the start of the
** line.
**
** Note that style is a somewhat incorrect name, drawing method would
** be more appropriate.
*/
int NirvanaQt::styleOfPos(int lineStartPos, int lineLen, int lineIndex, int dispIndex, char_type thisChar) {

    Q_UNUSED(thisChar);

    int pos;
    int style = 0;
    TextBuffer *styleBuffer = syntaxHighlighter_->styleBuffer();

    if (lineStartPos == -1 || !buffer_) {
        return FILL_MASK;
    }

    pos = lineStartPos + qMin(lineIndex, lineLen);

    if (lineIndex >= lineLen) {
        style = FILL_MASK;
    } else if (styleBuffer) {
        style = static_cast<unsigned char>(styleBuffer->BufGetCharacter(pos));
        if (style == unfinishedStyle_) {
            /* encountered "unfinished" style, trigger parsing */
            emitUnfinishedHighlightEncountered(pos);
            style = static_cast<unsigned char>(styleBuffer->BufGetCharacter(pos));
        }
    }

    if (inSelection(&buffer_->BufGetPrimarySelection(), pos, lineStartPos, dispIndex)) {
        style |= PRIMARY_MASK;
    }

    if (inSelection(&buffer_->BufGetHighlight(), pos, lineStartPos, dispIndex)) {
        style |= HIGHLIGHT_MASK;
    }

    if (inSelection(&buffer_->BufGetSecondarySelection(), pos, lineStartPos, dispIndex)) {
        style |= SECONDARY_MASK;
    }

/* store in the RANGESET_MASK portion of style the rangeset index for pos */
#if 0
    if (buf->rangesetTable) {
        int rangesetIndex = RangesetIndex1ofPos(buf->rangesetTable, pos, true);
        style |= ((rangesetIndex << RANGESET_SHIFT) & RANGESET_MASK);
    }

    /* store in the BACKLIGHT_MASK portion of style the background color class
     * of the character thisChar
     */
    if (textD->bgClass) {
        style |= (textD->bgClass[static_cast<unsigned char>(thisChar)] << BACKLIGHT_SHIFT);
    }
#endif
    return style;
}

/*
** Return true if position "pos" with indentation "dispIndex" is in
** selection "sel"
*/
bool NirvanaQt::inSelection(const Selection *sel, int pos, int lineStartPos, int dispIndex) {
    return sel->selected && ((!sel->rectangular && pos >= sel->start && pos < sel->end) ||
                             (sel->rectangular && pos >= sel->start && lineStartPos <= sel->end &&
                              dispIndex >= sel->rectStart && dispIndex < sel->rectEnd));
}

/*
** Draw a string or blank area according to parameter "style", using the
** appropriate colors and drawing method for that style, with top left
** corner at x, y.  If style says to draw text, use "string" as source of
** characters, and draw "nChars", if style is FILL, erase
** rectangle where text would have drawn from x to toX and from y to
** the maximum y extent of the current font(s).
*/
void NirvanaQt::drawString(QPainter *painter, int style, int x, int y, int toX, char_type *string, int nChars) {
    QRectF rect(x, y, toX - x, (viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent()));

    painter->save();

    /* Background color priority order is:
        1 Primary(Selection),
        2 Highlight(Parens),
        3 Rangeset,
        4 SyntaxHighlightStyle,
        5 Backlight (if NOT fill),
        6 DefaultBackground
    */

    /* Draw blank area rather than text, if that was the request */
    if (style & FILL_MASK) {
        /* wipes out to right hand edge of widget */
        if (toX >= left_) {
            // TODO(eteran): pick the color and border of the fill from the style!

            if (style & PRIMARY_MASK) {
                painter->setPen(viewport()->palette().highlightedText().color());
                painter->fillRect(rect, viewport()->palette().highlight());
            } else if (style & HIGHLIGHT_MASK) {
                painter->setPen(viewport()->palette().highlightedText().color());
                painter->fillRect(rect, Qt::lightGray);
            } else if (style & RANGESET_MASK) {
                painter->setPen(viewport()->palette().highlightedText().color());
                painter->fillRect(rect, Qt::green);
            }
        }
    } else if (nChars > 0) {

        // TODO(eteran): pick the colors from the style!

        if (style & PRIMARY_MASK) {
            painter->setPen(viewport()->palette().highlightedText().color());
            painter->fillRect(rect, viewport()->palette().highlight());
        } else if (style & HIGHLIGHT_MASK) {
            painter->setPen(viewport()->palette().highlightedText().color());
            painter->fillRect(rect, Qt::lightGray);
        } else if (style & RANGESET_MASK) {
            painter->setPen(viewport()->palette().highlightedText().color());
            painter->fillRect(rect, Qt::green);
        } else if (style & BACKLIGHT_MASK) {
            painter->setPen(viewport()->palette().highlightedText().color());
            painter->fillRect(rect, Qt::darkYellow);
        }

        QString s = QString::fromLatin1(string, nChars);

        int textStyle = (style & STYLE_LOOKUP_MASK);
        if (textStyle != 0) {
            int styleIndex = textStyle - ASCII_A;
            if (styleTableEntry *entry = syntaxHighlighter_->styleEntry(styleIndex)) {
                painter->setPen(entry->color);
                QFont f = font();
                f.setBold(entry->isBold);
                f.setItalic(entry->isItalic);
                painter->setFont(f);
            }
        }

        painter->drawText(x, y, toX, (viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent()),
                          Qt::TextSingleLine | Qt::TextDontClip, s);
    }

    painter->restore();
}

/*
** Draw a cursor with top center at x, y.
*/
void NirvanaQt::drawCursor(QPainter *painter, int x, int y) {

    int fontHeight = viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent();
    int bot = y + fontHeight - 1;

    if (viewport()->width() == 0 || x < -1 || x > left_ + viewport()->width()) {
        return;
    }

    QPainterPath path;

    /* For cursors other than the block, make them around 2/3 of a character
     * width, rounded to an even number of pixels so that X will draw an
     * odd number centered on the stem at x.
     */
    const int cursorWidth = (fixedFontWidth_ / 3) * 2;
    const int left = x - cursorWidth / 2;
    const int right = left + cursorWidth;

    /* Create segments and draw cursor */
    if (cursorStyle_ == CARET_CURSOR) {
        const int midY = bot - (viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent()) / 5;

        path.moveTo(left, bot);
        path.lineTo(x, midY);
        path.moveTo(x, midY);
        path.lineTo(right, bot);
        path.moveTo(left, bot);
        path.lineTo(x, midY - 1);
        path.moveTo(x, midY - 1);
        path.lineTo(right, bot);

    } else if (cursorStyle_ == NORMAL_CURSOR) {

        path.moveTo(left, y);
        path.lineTo(right, y);
        path.moveTo(x, y);
        path.lineTo(x, bot);
        path.moveTo(left, bot);
        path.lineTo(right, bot);

    } else if (cursorStyle_ == HEAVY_CURSOR) {

        path.moveTo(x - 1, y);
        path.lineTo(x - 1, bot);
        path.moveTo(x, y);
        path.lineTo(x, bot);
        path.moveTo(x + 1, y);
        path.lineTo(x + 1, bot);
        path.moveTo(left, y);
        path.lineTo(right, y);
        path.moveTo(left, bot);
        path.lineTo(right, bot);

    } else if (cursorStyle_ == DIM_CURSOR) {

        const int midY = y + fontHeight / 2;
        path.moveTo(x, y);
        path.lineTo(x, y);
        path.moveTo(x, midY);
        path.lineTo(x, midY);
        path.moveTo(x, bot);
        path.lineTo(x, bot);

    } else if (cursorStyle_ == BLOCK_CURSOR) {

        const int right = x + fixedFontWidth_;
        path.moveTo(x, y);
        path.lineTo(right, y);
        path.moveTo(right, y);
        path.lineTo(right, bot);
        path.moveTo(right, bot);
        path.lineTo(x, bot);
        path.moveTo(x, bot);
        path.lineTo(x, y);
    }

    painter->save();
    painter->setClipping(false);
    painter->setPen(CursorColor);
    painter->drawPath(path);
    painter->setClipping(true);
    painter->restore();

    /* Save the last position drawn */
    cursorX_ = x;
    cursorY_ = y;
}

/*
** Scan through the text in the "textD"'s buffer and recalculate the line
** starts array values beginning at index "startLine" and continuing through
** (including) "endLine".  It assumes that the line starts entry preceding
** "startLine" (or firstChar_ if startLine is 0) is good, and re-counts
** newlines to fill in the requested entries.  Out of range values for
** "startLine" and "endLine" are acceptable.
*/
void NirvanaQt::calcLineStarts(int startLine, int endLine) {
    const int bufLen = buffer_->BufGetLength();
    int line;
    int nVis = nVisibleLines_;
    int *lineStarts = lineStarts_.data();

    /* Clean up (possibly) messy input parameters */
    if (nVis == 0) {
        return;
    }

    if (endLine < 0) {
        endLine = 0;
    }

    if (endLine >= nVis) {
        endLine = nVis - 1;
    }

    if (startLine < 0) {
        startLine = 0;
    }

    if (startLine >= nVis) {
        startLine = nVis - 1;
    }

    if (startLine > endLine) {
        return;
    }

    /* Find the last known good line number -> position mapping */
    if (startLine == 0) {
        lineStarts[0] = firstChar_;
        startLine = 1;
    }

    int startPos = lineStarts[startLine - 1];

    /* If the starting position is already past the end of the text,
    fill in -1's (means no text on line) and return */
    if (startPos == -1) {
        for (line = startLine; line <= endLine; line++) {
            lineStarts[line] = -1;
        }
        return;
    }

    /* Loop searching for ends of lines and storing the positions of the
    start of the next line in lineStarts */
    for (line = startLine; line <= endLine; line++) {

        int lineEnd;
        int nextLineStart;
        findLineEnd(startPos, true, &lineEnd, &nextLineStart);
        startPos = nextLineStart;
        if (startPos >= bufLen) {
            /* If the buffer ends with a newline or line break, put
            buf->BufGetLength() in the next line start position (instead of
            a -1 which is the normal marker for an empty line) to
            indicate that the cursor may safely be displayed there */
            if (line == 0 || (lineStarts[line - 1] != bufLen && lineEnd != nextLineStart)) {
                lineStarts[line] = bufLen;
                line++;
            }
            break;
        }
        lineStarts[line] = startPos;
    }

    /* Set any entries beyond the end of the text to -1 */
    for (; line <= endLine; line++) {
        lineStarts[line] = -1;
    }
}

/*
** Finds both the end of the current line and the start of the next line.  Why?
** In continuous wrap mode, if you need to know both, figuring out one from the
** other can be expensive or error prone.  The problem comes when there's a
** trailing space or tab just before the end of the buffer.  To translate an
** end of line value to or from the next lines start value, you need to know
** whether the trailing space or tab is being used as a line break or just a
** normal character, and to find that out would otherwise require counting all
** the way back to the beginning of the line.
*/
void NirvanaQt::findLineEnd(int startPos, bool startPosIsLineStart, int *lineEnd, int *nextLineStart) {

    Q_UNUSED(startPosIsLineStart);

    /* if we're not wrapping use more efficient BufEndOfLine */
    if (!continuousWrap_) {
        *lineEnd = buffer_->BufEndOfLine(startPos);
        *nextLineStart = qMin(buffer_->BufGetLength(), *lineEnd + 1);
        return;
    }

    int retLines;
    int retLineStart;
    /* use the wrapped line counter routine to count forward one line */
    wrappedLineCounter(buffer_, startPos, buffer_->BufGetLength(), 1, startPosIsLineStart, 0, nextLineStart, &retLines,
                       &retLineStart, lineEnd);
    return;
}

/*
** Given a textDisp with a complete, up-to-date lineStarts array, update
** the lastChar entry to point to the last buffer position displayed.
*/
void NirvanaQt::calcLastChar() {
    int i = nVisibleLines_ - 1;
    while (i > 0 && lineStarts_[i] == -1) {
        i--;
    }

    lastChar_ = i < 0 ? 0 : TextDEndOfLine(lineStarts_[i], true);
}

/*
** Same as BufEndOfLine, but takes in to account line breaks when wrapping
** is turned on.  If the caller knows that startPos is at a line start, it
** can pass "startPosIsLineStart" as true to make the call more efficient
** by avoiding the additional step of scanning back to the last newline.
**
** Note that the definition of the end of a line is less clear when continuous
** wrap is on.  With continuous wrap off, it's just a pointer to the newline
** that ends the line.  When it's on, it's the character beyond the last
** DISPLAYABLE character on the line, where a whitespace character which has
** been "converted" to a newline for wrapping is not considered displayable.
** Also note that, a line can be wrapped at a non-whitespace character if the
** line had no whitespace.  In this case, this routine returns a pointer to
** the start of the next line.  This is also consistent with the model used by
** visLineLength.
*/
int NirvanaQt::TextDEndOfLine(int pos, bool startPosIsLineStart) {
    /* If we're not wrapping use more efficient BufEndOfLine */
    if (!continuousWrap_) {
        return buffer_->BufEndOfLine(pos);
    }

    if (pos == buffer_->BufGetLength()) {
        return pos;
    }

    int retLines;
    int retPos;
    int retLineStart;
    int retLineEnd;

    wrappedLineCounter(buffer_, pos, buffer_->BufGetLength(), 1, startPosIsLineStart, 0, &retPos, &retLines,
                       &retLineStart, &retLineEnd);
    return retLineEnd;
}

/*
** Cursor movement functions
*/
bool NirvanaQt::TextDMoveRight() {
    if (cursorPos_ >= buffer_->BufGetLength()) {
        return false;
    }

    TextDSetInsertPosition(cursorPos_ + 1);
    return true;
}

bool NirvanaQt::TextDMoveLeft() {
    if (cursorPos_ <= 0)
        return false;
    TextDSetInsertPosition(cursorPos_ - 1);
    return true;
}

bool NirvanaQt::TextDMoveUp(bool absolute) {
    int lineStartPos, column, prevLineStartPos, newPos, visLineNum;

    /* Find the position of the start of the line.  Use the line starts array
       if possible, to avoid unbounded line-counting in continuous wrap mode */
    if (absolute) {
        lineStartPos = buffer_->BufStartOfLine(cursorPos_);
        visLineNum = -1;
    } else if (posToVisibleLineNum(cursorPos_, &visLineNum))
        lineStartPos = lineStarts_[visLineNum];
    else {
        lineStartPos = TextDStartOfLine(cursorPos_);
        visLineNum = -1;
    }
    if (lineStartPos == 0)
        return false;

    /* Decide what column to move to, if there's a preferred column use that */
    column = cursorPreferredCol_ >= 0 ? cursorPreferredCol_ : buffer_->BufCountDispChars(lineStartPos, cursorPos_);

    /* count forward from the start of the previous line to reach the column */
    if (absolute) {
        prevLineStartPos = buffer_->BufCountBackwardNLines(lineStartPos, 1);
    } else if (visLineNum != -1 && visLineNum != 0) {
        prevLineStartPos = lineStarts_[visLineNum - 1];
    } else {
        prevLineStartPos = TextDCountBackwardNLines(lineStartPos, 1);
    }

    newPos = buffer_->BufCountForwardDispChars(prevLineStartPos, column);
    if (continuousWrap_ && !absolute)
        newPos = qMin(newPos, TextDEndOfLine(prevLineStartPos, true));

    /* move the cursor */
    TextDSetInsertPosition(newPos);

    /* if a preferred column wasn't aleady established, establish it */
    cursorPreferredCol_ = column;

    return true;
}

bool NirvanaQt::TextDMoveDown(bool absolute) {
    int lineStartPos;
    int column;
    int nextLineStartPos;
    int newPos;
    int visLineNum;

    if (cursorPos_ == buffer_->BufGetLength()) {
        return false;
    }

    if (absolute) {
        lineStartPos = buffer_->BufStartOfLine(cursorPos_);
        visLineNum = -1;
    } else if (posToVisibleLineNum(cursorPos_, &visLineNum)) {
        lineStartPos = lineStarts_[visLineNum];
    } else {
        lineStartPos = TextDStartOfLine(cursorPos_);
        visLineNum = -1;
    }

    column = cursorPreferredCol_ >= 0 ? cursorPreferredCol_ : buffer_->BufCountDispChars(lineStartPos, cursorPos_);

    if (absolute) {
        nextLineStartPos = buffer_->BufCountForwardNLines(lineStartPos, 1);
    } else {
        nextLineStartPos = TextDCountForwardNLines(lineStartPos, 1, true);
    }

    newPos = buffer_->BufCountForwardDispChars(nextLineStartPos, column);

    if (continuousWrap_ && !absolute) {
        newPos = qMin(newPos, TextDEndOfLine(nextLineStartPos, true));
    }

    TextDSetInsertPosition(newPos);
    cursorPreferredCol_ = column;

    return true;
}

/*
** Set the position of the text insertion cursor for text display "textD"
*/
void NirvanaQt::TextDSetInsertPosition(int newPos) {
    /* make sure new position is ok, do nothing if it hasn't changed */
    if (newPos == cursorPos_) {
        return;
    }

    newPos = qBound(0, newPos, buffer_->BufGetLength());

    /* cursor movement cancels vertical cursor motion column */
    cursorPreferredCol_ = -1;

    /* erase the cursor at it's previous position */
    TextDBlankCursor();

    /* draw it at its new position */
    cursorPos_ = newPos;
    cursorOn_ = true;
    cursorTimer_->start(); // restart the timer so we have a full interval
                           // until it tries to blank the cursor again
    textDRedisplayRange(cursorPos_ - 1, cursorPos_ + 1);
}

/*
** Same as BufStartOfLine, but returns the character after last wrap point
** rather than the last newline.
*/
int NirvanaQt::TextDStartOfLine(int pos) {
    /* If we're not wrapping, use the more efficient BufStartOfLine */
    if (!continuousWrap_) {
        return buffer_->BufStartOfLine(pos);
    }

    int retLineStart;

    int retLines;
    int retPos;
    int retLineEnd;

    wrappedLineCounter(buffer_, buffer_->BufStartOfLine(pos), pos, INT_MAX, true, 0, &retPos, &retLines, &retLineStart,
                       &retLineEnd);
    return retLineStart;
}

/*
** Find the line number of position "pos" relative to the first line of
** displayed text. Returns false if the line is not displayed.
*/
bool NirvanaQt::posToVisibleLineNum(int pos, int *lineNum) {
    if (pos < firstChar_) {
        return false;
    }

    if (pos > lastChar_) {
        if (emptyLinesVisible()) {
            if (lastChar_ < buffer_->BufGetLength()) {
                if (!posToVisibleLineNum(lastChar_, lineNum)) {
                    qDebug() << "Consistency check ptvl failed";
                    return false;
                }
                return ++(*lineNum) <= nVisibleLines_ - 1;
            } else {
                posToVisibleLineNum(qMax(lastChar_ - 1, 0), lineNum);
                return true;
            }
        }
        return false;
    }

    for (int i = nVisibleLines_ - 1; i >= 0; i--) {
        if (lineStarts_[i] != -1 && pos >= lineStarts_[i]) {
            *lineNum = i;
            return true;
        }
    }

    return false;
}

/*
** Same as BufCountBackwardNLines, but takes in to account line breaks when
** wrapping is turned on.
*/
int NirvanaQt::TextDCountBackwardNLines(int startPos, int nLines) {

    /* If we're not wrapping, use the more efficient BufCountBackwardNLines */
    if (!continuousWrap_) {
        return buffer_->BufCountBackwardNLines(startPos, nLines);
    }

    int pos = startPos;
    while (true) {
        int lineStart = buffer_->BufStartOfLine(pos);

        int retLines;
        int retPos;
        int retLineStart;
        int retLineEnd;
        wrappedLineCounter(buffer_, lineStart, pos, INT_MAX, true, 0, &retPos, &retLines, &retLineStart, &retLineEnd);

        if (retLines > nLines) {
            return TextDCountForwardNLines(lineStart, retLines - nLines, true);
        }

        nLines -= retLines;
        pos = lineStart - 1;

        if (pos < 0) {
            return 0;
        }

        nLines -= 1;
    }
}

/*
** Same as BufCountForwardNLines, but takes in to account line breaks when
** wrapping is turned on. If the caller knows that startPos is at a line start,
** it can pass "startPosIsLineStart" as true to make the call more efficient
** by avoiding the additional step of scanning back to the last newline.
*/
int NirvanaQt::TextDCountForwardNLines(int startPos, unsigned nLines, bool startPosIsLineStart) {
    int retLines, retPos, retLineStart, retLineEnd;

    /* if we're not wrapping use more efficient BufCountForwardNLines */
    if (!continuousWrap_) {
        return buffer_->BufCountForwardNLines(startPos, nLines);
    }

    /* wrappedLineCounter can't handle the 0 lines case */
    if (nLines == 0) {
        return startPos;
    }

    /* use the common line counting routine to count forward */
    wrappedLineCounter(buffer_, startPos, buffer_->BufGetLength(), nLines, startPosIsLineStart, 0, &retPos, &retLines,
                       &retLineStart, &retLineEnd);

    return retPos;
}

/*
** Count forward from startPos to either maxPos or maxLines (whichever is
** reached first), and return all relevant positions and line count.
** The provided textBuffer may differ from the actual text buffer of the
** widget. In that case it must be a (partial) copy of the actual text buffer
** and the styleBufOffset argument must indicate the starting position of the
** copy, to take into account the correct style information.
**
** Returned values:
**
**   retPos:	    Position where counting ended.  When counting lines, the
**  	    	    position returned is the start of the line "maxLines"
**  	    	    lines beyond "startPos".
**   retLines:	    Number of line breaks counted
**   retLineStart:  Start of the line where counting ended
**   retLineEnd:    End position of the last line traversed
*/
void NirvanaQt::wrappedLineCounter(const TextBuffer *buf, int startPos, int maxPos, int maxLines,
                                   bool startPosIsLineStart, int styleBufOffset, int *retPos, int *retLines,
                                   int *retLineStart, int *retLineEnd) {
    int lineStart;
    int newLineStart = 0;
    int b;
    int p;
    int colNum;
    int wrapMargin;
    int maxWidth;
    int width;
    int countPixels;
    int i;
    int foundBreak;
    int nLines = 0;
    int tabDist = buffer_->BufGetTabDistance();
    char_type nullptrSubsChar = buffer_->BufGetNullSubsChar();

    /* If the font is fixed, or there's a wrap margin set, it's more efficient
       to measure in columns, than to count pixels.  Determine if we can count
       in columns (countPixels == false) or must count pixels (countPixels ==
       true), and set the wrap target for either pixels or columns */
    if (fixedFontWidth_ != -1 || wrapMargin_ != 0) {
        countPixels = false;
        wrapMargin = wrapMargin_ != 0 ? wrapMargin_ : viewport()->width() / fixedFontWidth_;
        maxWidth = INT_MAX;
    } else {
        countPixels = true;
        wrapMargin = INT_MAX;
        maxWidth = viewport()->width();
    }

    /* Find the start of the line if the start pos is not marked as a
       line start. */
    if (startPosIsLineStart) {
        lineStart = startPos;
    } else {
        lineStart = TextDStartOfLine(startPos);
    }

    /*
    ** Loop until position exceeds maxPos or line count exceeds maxLines.
    ** (actually, contines beyond maxPos to end of line containing maxPos,
    ** in case later characters cause a word wrap back before maxPos)
    */
    colNum = 0;
    width = 0;
    for (p = lineStart; p < buf->BufGetLength(); p++) {
        unsigned char c = buf->BufGetCharacter(p);

        /* If the character was a newline, count the line and start over,
           otherwise, add it to the width and column counts */
        if (c == '\n') {
            if (p >= maxPos) {
                *retPos = maxPos;
                *retLines = nLines;
                *retLineStart = lineStart;
                *retLineEnd = maxPos;
                return;
            }
            nLines++;
            if (nLines >= maxLines) {
                *retPos = p + 1;
                *retLines = nLines;
                *retLineStart = p + 1;
                *retLineEnd = p;
                return;
            }
            lineStart = p + 1;
            colNum = 0;
            width = 0;
        } else {
            colNum += TextBuffer::BufCharWidth(c, colNum, tabDist, nullptrSubsChar);
            if (countPixels)
                width += measurePropChar(c, colNum, p + styleBufOffset);
        }

        /* If character exceeded wrap margin, find the break point
           and wrap there */
        if (colNum > wrapMargin || width > maxWidth) {
            foundBreak = false;
            for (b = p; b >= lineStart; b--) {
                c = buf->BufGetCharacter(b);
                if (c == '\t' || c == ' ') {
                    newLineStart = b + 1;
                    if (countPixels) {
                        colNum = 0;
                        width = 0;
                        for (i = b + 1; i < p + 1; i++) {
                            width += measurePropChar(buf->BufGetCharacter(i), colNum, i + styleBufOffset);
                            colNum++;
                        }
                    } else
                        colNum = buf->BufCountDispChars(b + 1, p + 1);
                    foundBreak = true;
                    break;
                }
            }
            if (!foundBreak) { /* no whitespace, just break at margin */
                newLineStart = qMax(p, lineStart + 1);
                colNum = TextBuffer::BufCharWidth(c, colNum, tabDist, nullptrSubsChar);
                if (countPixels)
                    width = measurePropChar(c, colNum, p + styleBufOffset);
            }
            if (p >= maxPos) {
                *retPos = maxPos;
                *retLines = maxPos < newLineStart ? nLines : nLines + 1;
                *retLineStart = maxPos < newLineStart ? lineStart : newLineStart;
                *retLineEnd = maxPos;
                return;
            }
            nLines++;
            if (nLines >= maxLines) {
                *retPos = foundBreak ? b + 1 : qMax(p, lineStart + 1);
                *retLines = nLines;
                *retLineStart = lineStart;
                *retLineEnd = foundBreak ? b : p;
                return;
            }
            lineStart = newLineStart;
        }
    }

    /* reached end of buffer before reaching pos or line target */
    *retPos = buf->BufGetLength();
    *retLines = nLines;
    *retLineStart = lineStart;
    *retLineEnd = buf->BufGetLength();
}

/*
** Return true if there are lines visible with no corresponding buffer text
*/
bool NirvanaQt::emptyLinesVisible() {
    return nVisibleLines_ > 0 && lineStarts_[nVisibleLines_ - 1] == -1;
}

/*
** Measure the width in pixels of a character "c" at a particular column
** "colNum" and buffer position "pos".  This is for measuring characters in
** proportional or mixed-width highlighting fonts.
**
** A note about proportional and mixed-width fonts: the mixed width and
** proportional font code in nedit does not get much use in general editing,
** because nedit doesn't allow per-language-mode fonts, and editing programs
** in a proportional font is usually a bad idea, so very few users would
** choose a proportional font as a default.  There are still probably mixed-
** width syntax highlighting cases where things don't redraw properly for
** insertion/deletion, though static display and wrapping and resizing
** should now be solid because they are now used for online help display.
*/
int NirvanaQt::measurePropChar(char_type c, int colNum, int pos) {
    int style;
    char_type expChar[MAX_EXP_CHAR_LEN];
    TextBuffer *styleBuf = syntaxHighlighter_->styleBuffer();

    int charLen =
        TextBuffer::BufExpandCharacter(c, colNum, expChar, buffer_->BufGetTabDistance(), buffer_->BufGetNullSubsChar());
    if (styleBuf == nullptr) {
        style = 0;
    } else {
        style = (unsigned char)styleBuf->BufGetCharacter(pos);
        if (style == unfinishedStyle_) {
            /* encountered "unfinished" style, trigger parsing */
            emitUnfinishedHighlightEncountered(pos);
            style = (unsigned char)styleBuf->BufGetCharacter(pos);
        }
    }

    return stringWidth(expChar, charLen, style);
}

void NirvanaQt::TextDBlankCursor() {
    if (!cursorOn_) {
        return;
    }

    blankCursorProtrusions();
    cursorOn_ = false;
    textDRedisplayRange(cursorPos_ - 1, cursorPos_ + 1);
}

void NirvanaQt::TextDUnblankCursor() {
    if (!cursorOn_) {
        cursorOn_ = true;
        textDRedisplayRange(cursorPos_ - 1, cursorPos_ + 1);
    }
}

/*
** Refresh all of the text between buffer positions "start" and "end"
** not including the character at the position "end".
** If end points beyond the end of the buffer, refresh the whole display
** after pos, including blank lines which are not technically part of
** any range of characters.
*/
void NirvanaQt::textDRedisplayRange(int start, int end) {

    Q_UNUSED(start);
    Q_UNUSED(end);
    viewport()->update();
#if 0
    int i, startLine, lastLine, startIndex, endIndex;

    /* If the range is outside of the displayed text, just return */
    if (end < firstChar_ || (start > lastChar_ && !emptyLinesVisible(textD))) {
        return;
    }

    /* Clean up the starting and ending values */
    start = qBound(0, start, buffer_->BufGetLength());
    end   = qBound(0, end, buffer_->BufGetLength());

    /* Get the starting and ending lines */
    if (start < firstChar_) {
        start = firstChar_;
    }

    if (!posToVisibleLineNum(textD, start, &startLine)) {
        startLine = nVisibleLines_ - 1;
    }

    if (end >= lastChar_) {
        lastLine = nVisibleLines_ - 1;
    } else {
        if (!posToVisibleLineNum(textD, end, &lastLine)) {
            /* shouldn't happen */
            lastLine = nVisibleLines_ - 1;
        }
    }

    /* Get the starting and ending positions within the lines */
    startIndex = (lineStarts_[startLine] == -1) ? 0 : start - lineStarts_[startLine];
    if (end >= lastChar_)
    {
        /*  Request to redisplay beyond lastChar_, so tell
            redisplayLine() to display everything to infy.  */
        endIndex = INT_MAX;
    } else if (lineStarts_[lastLine] == -1) {
        /*  Here, lastLine is determined by posToVisibleLineNum() (see
            if/else above) but deemed to be out of display according to
            lineStarts_. */
        endIndex = 0;
    } else {
        endIndex = end - lineStarts_[lastLine];
    }

    /* Reset the clipping rectangles for the drawing GCs which are shared
       using XtAllocateGC, and may have changed since the last use */
    resetClipRectangles();

    /* If the starting and ending lines are the same, redisplay the single
       line between "start" and "end" */
    if (startLine == lastLine) {
        redisplayLine(startLine, 0, INT_MAX, startIndex, endIndex);
        return;
    }

    /* Redisplay the first line from "start" */
    redisplayLine(startLine, 0, INT_MAX, startIndex, INT_MAX);

    /* Redisplay the lines in between at their full width */
    for (i=startLine+1; i<lastLine; i++)
    redisplayLine(i, 0, INT_MAX, 0, INT_MAX);

    /* Redisplay the last line to "end" */
    redisplayLine(lastLine, 0, INT_MAX, 0, endIndex);
#endif
}

/*
** When the cursor is at the left or right edge of the text, part of it
** sticks off into the clipped region beyond the text.  Normal redrawing
** can not overwrite this protruding part of the cursor, so it must be
** erased independently by calling this routine.
*/
void NirvanaQt::blankCursorProtrusions() {
// TODO(eteran): this may not be necesary anymore in light of Qt's
// drawing API
#if 0
    int x, width, cursorX = textD->cursorX, cursorY = textD->cursorY;
    int fontWidth = textD->fontStruct->max_bounds.width;
    int fontHeight = textD->ascent + textD->descent;
    int cursorWidth, left = left_, right = left + viewport()->width();

    cursorWidth = (fontWidth/3) * 2;
    if (cursorX >= left-1 && cursorX <= left + cursorWidth/2 - 1) {
        x = cursorX - cursorWidth/2;
        width = left - x;
    } else if (cursorX >= right - cursorWidth/2 && cursorX <= right) {
        x = right;
        width = cursorX + cursorWidth/2 + 2 - right;
    } else
        return;

    XClearArea(XtDisplay(textD->w), XtWindow(textD->w), x, cursorY, width, fontHeight, false);
#endif
}

/*
** Translate window coordinates to the nearest row and column number for
** positioning the cursor.  This, of course, makes no sense when the font
** is proportional, since there are no absolute columns.
*/
void NirvanaQt::TextDXYToUnconstrainedPosition(int x, int y, int *row, int *column) {
    xyToUnconstrainedPos(x, y, row, column, CURSOR_POS);
}

/*
** Translate window coordinates to the nearest row and column number for
** positioning the cursor.  This, of course, makes no sense when the font is
** proportional, since there are no absolute columns.  The parameter posType
** specifies how to interpret the position: CURSOR_POS means translate the
** coordinates to the nearest position between characters, and CHARACTER_POS
** means translate the position to the nearest character cell.
*/
void NirvanaQt::xyToUnconstrainedPos(int x, int y, int *row, int *column, PositionTypes posType) {

    int fontHeight = viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent();
    int fontWidth = fixedFontWidth_;

    /* Find the visible line number corresponding to the y coordinate */
    *row = qBound(0, (y - top_) / fontHeight, nVisibleLines_ - 1);

    *column = ((x - left_) + horizontalScrollBar()->value() + (posType == CURSOR_POS ? fontWidth / 2 : 0)) / fontWidth;

    if (*column < 0)
        *column = 0;
}

/*
** Translate a position into a line number (if the position is visible,
** if it's not, return false
*/
bool NirvanaQt::TextPosToLineAndCol(int pos, int *lineNum, int *column) {
    return TextDPosToLineAndCol(pos, lineNum, column);
}

/*
** If the text widget is maintaining a line number count appropriate to "pos"
** return the line and column numbers of pos, otherwise return false.  If
** continuous wrap mode is on, returns the absolute line number (as opposed to
** the wrapped line number which is used for scrolling).  THIS ROUTINE ONLY
** WORKS FOR DISPLAYED LINES AND, IN CONTINUOUS WRAP MODE, ONLY WHEN THE
** ABSOLUTE LINE NUMBER IS BEING MAINTAINED.  Otherwise, it returns false.
*/
bool NirvanaQt::TextDPosToLineAndCol(int pos, int *lineNum, int *column) {

    /* In continuous wrap mode, the absolute (non-wrapped) line count is
       maintained separately, as needed.  Only return it if we're actually
       keeping track of it and pos is in the displayed text */
    if (continuousWrap_) {

        if (!maintainingAbsTopLineNum() || pos < firstChar_ || pos > lastChar_) {
            return false;
        }

        *lineNum = absTopLineNum_ + buffer_->BufCountLines(firstChar_, pos);
        *column = buffer_->BufCountDispChars(buffer_->BufStartOfLine(pos), pos);
        return true;
    }

    /* Only return the data if pos is within the displayed text */
    if (!posToVisibleLineNum(pos, lineNum))
        return false;

    *column = buffer_->BufCountDispChars(lineStarts_[*lineNum], pos);
    *lineNum += topLineNum_;
    return true;
}

/*
** Return true if a separate absolute top line number is being maintained
** (for displaying line numbers or showing in the statistics line).
*/
bool NirvanaQt::maintainingAbsTopLineNum() {
    return continuousWrap_ && (lineNumWidth_ != 0 || needAbsTopLineNum_);
}

void NirvanaQt::selectAllAP() {
    // cancelDrag(w);
    buffer_->BufSelect(0, buffer_->BufGetLength());
}

/*
** Insert text "chars" at the cursor position, respecting pending delete
** selections, overstrike, and handling cursor repositioning as if the text
** had been typed.  If autoWrap is on wraps the text to fit within the wrap
** margin, auto-indenting where the line was wrapped (but nowhere else).
** "allowPendingDelete" controls whether primary selections in the widget are
** treated as pending delete selections (true), or ignored (false). "event"
** is optional and is just passed on to the cursor movement callbacks.
*/
void NirvanaQt::TextInsertAtCursor(const char_type *chars, bool allowPendingDelete, bool allowWrap) {
    const char_type *c;
    char_type *lineStartText;
    int breakAt = 0;

    /* Don't wrap if auto-wrap is off or suppressed, or it's just a newline */
    if (!allowWrap || !autoWrap_ || (chars[0] == '\n' && chars[1] == '\0')) {
        simpleInsertAtCursor(chars, allowPendingDelete);
        return;
    }

    /* If this is going to be a pending delete operation, the real insert
       position is the start of the selection.  This will make rectangular
       selections wrap strangely, but this routine should rarely be used for
       them, and even more rarely when they need to be wrapped. */
    const int replaceSel = allowPendingDelete && pendingSelection();
    const int cursorPos = replaceSel ? buffer_->BufGetPrimarySelection().start : TextDGetInsertPosition();

    /* If the text is only one line and doesn't need to be wrapped, just insert
       it and be done (for efficiency only, this routine is called for each
       character typed). (Of course, it may not be significantly more efficient
       than the more general code below it, so it may be a waste of time!) */
    int wrapMargin = wrapMargin_ != 0 ? wrapMargin_ : viewport()->width() / fixedFontWidth_;
    int lineStartPos = buffer_->BufStartOfLine(cursorPos);
    int colNum = buffer_->BufCountDispChars(lineStartPos, cursorPos);

    for (c = chars; *c != '\0' && *c != '\n'; c++) {
        colNum += TextBuffer::BufCharWidth(*c, colNum, buffer_->BufGetTabDistance(), buffer_->BufGetNullSubsChar());
    }

    const bool singleLine = *c == '\0';
    if (colNum < wrapMargin && singleLine) {
        simpleInsertAtCursor(chars, true);
        return;
    }

    /* Wrap the text */
    lineStartText = buffer_->BufGetRange(lineStartPos, cursorPos);
    char_type *const wrappedText = wrapText(lineStartText, chars, lineStartPos, wrapMargin, replaceSel ? nullptr : &breakAt);
    delete[] lineStartText;

    /* Insert the text.  Where possible, use TextDInsert which is optimized
       for less redraw. */
    if (replaceSel) {
        buffer_->BufReplaceSelected(wrappedText);
        TextDSetInsertPosition(buffer_->BufGetCursorPosHint());
    } else if (overstrike_) {
        if (breakAt == 0 && singleLine)
            TextDOverstrike(wrappedText);
        else {
            buffer_->BufReplace(cursorPos - breakAt, cursorPos, wrappedText);
            TextDSetInsertPosition(buffer_->BufGetCursorPosHint());
        }
    } else {
        if (breakAt == 0) {
            TextDInsert(wrappedText);
        } else {
            buffer_->BufReplace(cursorPos - breakAt, cursorPos, wrappedText);
            TextDSetInsertPosition(buffer_->BufGetCursorPosHint());
        }
    }
    delete[] wrappedText;
    checkAutoShowInsertPos();
    emitCursorMoved();
}

int NirvanaQt::TextDGetInsertPosition() const {
    return cursorPos_;
}

/*
** Insert text "chars" at the cursor position, as if the text had been
** typed.  Same as TextInsertAtCursor, but without the complicated auto-wrap
** scanning and re-formatting.
*/
void NirvanaQt::simpleInsertAtCursor(const char_type *chars, bool allowPendingDelete) {
    const char_type *c;

    if (allowPendingDelete && pendingSelection()) {
        buffer_->BufReplaceSelected(chars);
        TextDSetInsertPosition(buffer_->BufGetCursorPosHint());
    } else if (overstrike_) {
        for (c = chars; *c != '\0' && *c != '\n'; c++)
            ;
        if (*c == '\n') {
            TextDInsert(chars);
        } else {
            TextDOverstrike(chars);
        }
    } else {
        TextDInsert(chars);
    }

    checkAutoShowInsertPos();
    emitCursorMoved();
}

/*
** Return true if pending delete is on and there's a selection contiguous
** with the cursor ready to be deleted.  These criteria are used to decide
** if typing a character or inserting something should delete the selection
** first.
*/
bool NirvanaQt::pendingSelection() {
    Selection *sel = &buffer_->BufGetPrimarySelection();
    int pos = TextDGetInsertPosition();

    return pendingDelete_ && sel->selected && pos >= sel->start && pos <= sel->end;
}

/*
** Insert "text" (which must not contain newlines), overstriking the current
** cursor location.
*/
void NirvanaQt::TextDOverstrike(const char_type *text) {
    int startPos = cursorPos_;

    const int lineStart = buffer_->BufStartOfLine(startPos);
    const int textLen = static_cast<int>(traits_type::length(text));
    int i;
    int p;
    int endPos;
    const char_type *c;
    char_type *paddedText = nullptr;

    /* determine how many displayed character positions are covered */
    int startIndent = buffer_->BufCountDispChars(lineStart, startPos);
    int indent = startIndent;
    for (c = text; *c != '\0'; c++) {
        indent += TextBuffer::BufCharWidth(*c, indent, buffer_->BufGetTabDistance(), buffer_->BufGetNullSubsChar());
    }
    int endIndent = indent;

    /* find which characters to remove, and if necessary generate additional
       padding to make up for removed control characters at the end */
    indent = startIndent;
    for (p = startPos;; p++) {
        if (p == buffer_->BufGetLength())
            break;
        char_type ch = buffer_->BufGetCharacter(p);
        if (ch == '\n')
            break;
        indent += TextBuffer::BufCharWidth(ch, indent, buffer_->BufGetTabDistance(), buffer_->BufGetNullSubsChar());
        if (indent == endIndent) {
            p++;
            break;
        } else if (indent > endIndent) {
            if (ch != '\t') {
                p++;
                paddedText = new char_type[textLen + MAX_EXP_CHAR_LEN + 1];
                strcpy(paddedText, text);
                for (i = 0; i < indent - endIndent; i++) {
                    paddedText[textLen + i] = ' ';
                }
                paddedText[textLen + i] = '\0';
            }
            break;
        }
    }
    endPos = p;

    cursorToHint_ = startPos + textLen;
    buffer_->BufReplace(startPos, endPos, paddedText == nullptr ? text : paddedText);
    cursorToHint_ = NoCursorHint;
    delete[] paddedText;
}

/*
** Insert "text" at the current cursor location.  This has the same
** effect as inserting the text into the buffer using BufInsert and
** then moving the insert position after the newly inserted text, except
** that it's optimized to do less redrawing.
*/
void NirvanaQt::TextDInsert(const char_type *text) {
    int pos = cursorPos_;
    int length = static_cast<int>(traits_type::length(text));
    cursorToHint_ = pos + length;
    buffer_->BufInsert(pos, text, length);
    cursorToHint_ = NoCursorHint;
}

void NirvanaQt::checkAutoShowInsertPos() {
    if (autoShowInsertPos_) {
        TextDMakeInsertPosVisible();
    }
}

/*
** Scroll the display to bring insertion cursor into view.
**
** Note: it would be nice to be able to do this without counting lines twice
** (setScroll counts them too) and/or to count from the most efficient
** starting point, but the efficiency of this routine is not as important to
** the overall performance of the text display.
*/
void NirvanaQt::TextDMakeInsertPosVisible() {

    int x;
    int y;
    int cursorPos = cursorPos_;
    int linesFromTop = 0;
    int cursorVPadding = (int)cursorVPadding_;

    int hOffset = horizOffset_;
    int topLine = topLineNum_;

    /* Don't do padding if this is a mouse operation */
    bool do_padding = (dragState_ == NOT_CLICKED) && (cursorVPadding_ > 0);

    /* Find the new top line number */
    if (cursorPos < firstChar_) {
        topLine -= TextDCountLines(cursorPos, firstChar_, false);
        /* linesFromTop = 0; */
    } else if (cursorPos > lastChar_ && !emptyLinesVisible()) {
        topLine += TextDCountLines(lastChar_ - (wrapUsesCharacter(lastChar_) ? 0 : 1), cursorPos, false);
        linesFromTop = nVisibleLines_ - 1;
    } else if (cursorPos == lastChar_ && !emptyLinesVisible() && !wrapUsesCharacter(lastChar_)) {
        topLine++;
        linesFromTop = nVisibleLines_ - 1;
    } else {
        /* Avoid extra counting if cursorVPadding is disabled */
        if (do_padding) {
            linesFromTop = TextDCountLines(firstChar_, cursorPos, true);
        }
    }
    if (topLine < 1) {
        qDebug() << "Internal consistency check tl1 failed";
        topLine = 1;
    }

    if (do_padding) {
        /* Keep the cursor away from the top or bottom of screen. */
        if (nVisibleLines_ <= 2 * (int)cursorVPadding) {
            topLine += (linesFromTop - nVisibleLines_ / 2);
            topLine = qMax(topLine, 1);
        } else if (linesFromTop < (int)cursorVPadding) {
            topLine -= (cursorVPadding - linesFromTop);
            topLine = qMax(topLine, 1);
        } else if (linesFromTop > nVisibleLines_ - (int)cursorVPadding - 1) {
            topLine += (linesFromTop - (nVisibleLines_ - cursorVPadding - 1));
        }
    }

    /* Find the new setting for horizontal offset (this is a bit ungraceful).
       If the line is visible, just use TextDPositionToXY to get the position
       to scroll to, otherwise, do the vertical scrolling first, then the
       horizontal */
    if (!TextDPositionToXY(cursorPos, &x, &y)) {
        setScroll(topLine, hOffset, true, true);
        if (!TextDPositionToXY(cursorPos, &x, &y)) {
            return; /* Give up, it's not worth it (but why does it fail?) */
        }
    }
    if (x > left_ + viewport()->width()) {
        hOffset += x - (left_ + viewport()->width());
    } else if (x < left_) {
        hOffset += x - left_;
    }

    /* Do the scroll */
    setScroll(topLine, hOffset, true, true);
}

/*
** Wrap multi-line text in argument "text" to be inserted at the end of the
** text on line "startLine" and return the result.  If "breakBefore" is
** non-nullptr, allow wrapping to extend back into "startLine", in which case
** the returned text will include the wrapped part of "startLine", and
** "breakBefore" will return the number of characters at the end of
** "startLine" that were absorbed into the returned string.  "breakBefore"
** will return zero if no characters were absorbed into the returned string.
** The buffer offset of text in the widget's text buffer is needed so that
** smart indent (which can be triggered by wrapping) can search back farther
** in the buffer than just the text in startLine.
*/
char_type *NirvanaQt::wrapText(const char_type *startLine, const char_type *text, int bufOffset, int wrapMargin,
                          int *breakBefore) {
    int startLineLen = static_cast<int>(traits_type::length(startLine));
    int breakAt;
    int charsAdded;
    int firstBreak = -1;
    int tabDist = buffer_->BufGetTabDistance();
    char_type c;
    char_type *wrappedText;

    /* Create a temporary text buffer and load it with the strings */
    auto wrapBuf = new TextBuffer();
    wrapBuf->BufInsert(0, startLine, startLineLen);
    wrapBuf->BufInsert(wrapBuf->BufGetLength(), text);

    /* Scan the buffer for long lines and apply wrapLine when wrapMargin is
       exceeded.  limitPos enforces no breaks in the "startLine" part of the
       string (if requested), and prevents re-scanning of long unbreakable
       lines for each character beyond the margin */
    int colNum = 0;
    int pos = 0;
    int lineStartPos = 0;
    int limitPos = breakBefore == nullptr ? startLineLen : 0;

    while (pos < wrapBuf->BufGetLength()) {
        c = wrapBuf->BufGetCharacter(pos);
        if (c == '\n') {
            lineStartPos = limitPos = pos + 1;
            colNum = 0;
        } else {
            colNum += TextBuffer::BufCharWidth(c, colNum, tabDist, buffer_->BufGetNullSubsChar());
            if (colNum > wrapMargin) {
                if (!wrapLine(wrapBuf, bufOffset, lineStartPos, pos, limitPos, &breakAt, &charsAdded)) {
                    limitPos = qMax(pos, limitPos);
                } else {
                    lineStartPos = limitPos = breakAt + 1;
                    pos += charsAdded;
                    colNum = wrapBuf->BufCountDispChars(lineStartPos, pos + 1);
                    if (firstBreak == -1)
                        firstBreak = breakAt;
                }
            }
        }
        pos++;
    }

    /* Return the wrapped text, possibly including part of startLine */
    if (breakBefore == nullptr)
        wrappedText = wrapBuf->BufGetRange(startLineLen, wrapBuf->BufGetLength());
    else {
        *breakBefore = firstBreak != -1 && firstBreak < startLineLen ? startLineLen - firstBreak : 0;
        wrappedText = wrapBuf->BufGetRange(startLineLen - *breakBefore, wrapBuf->BufGetLength());
    }
    delete wrapBuf;
    return wrappedText;
}

/*
** Wraps the end of a line beginning at lineStartPos and ending at lineEndPos
** in "buf", at the last white-space on the line >= limitPos.  (The implicit
** assumption is that just the last character of the line exceeds the wrap
** margin, and anywhere on the line we can wrap is correct).  Returns false if
** unable to wrap the line.  "breakAt", returns the character position at
** which the line was broken,
**
** Auto-wrapping can also trigger auto-indent.  The additional parameter
** bufOffset is needed when auto-indent is set to smart indent and the smart
** indent routines need to scan far back in the buffer.  "charsAdded" returns
** the number of characters added to acheive the auto-indent.  wrapMargin is
** used to decide whether auto-indent should be skipped because the indent
** string itself would exceed the wrap margin.
*/
bool NirvanaQt::wrapLine(TextBuffer *buf, int bufOffset, int lineStartPos, int lineEndPos, int limitPos, int *breakAt,
                         int *charsAdded) {
    int p;
    int length;
    int column;
    char_type c;

    /* Scan backward for whitespace or BOL.  If BOL, return false, no
     * whitespace in line at which to wrap
     */
    for (p = lineEndPos;; p--) {
        if (p < lineStartPos || p < limitPos) {
            return false;
        }

        c = buf->BufGetCharacter(p);
        if (c == '\t' || c == ' ') {
            break;
        }
    }

    /* Create an auto-indent string to insert to do wrap.  If the auto
    indent string reaches the wrap position, slice the auto-indent
    back off and return to the left margin */
    const char_type *indentStr;
    if (autoIndent_ || smartIndent_) {
        char_type *indentString = createIndentString(buf, bufOffset, lineStartPos, lineEndPos, &length, &column);
        if (column >= p - lineStartPos) {
            indentString[1] = '\0';
        }
        indentStr = indentString;
    } else {
        indentStr = "\n";
        length = 1;
    }

    /* Replace the whitespace character with the auto-indent string
    and return the stats */
    buf->BufReplace(p, p + 1, indentStr);
    if (autoIndent_ || smartIndent_) {
        delete[] indentStr;
    }

    *breakAt = p;
    *charsAdded = length - 1;
    return true;
}

/*
** Create and return an auto-indent string to add a newline at lineEndPos to a
** line starting at lineStartPos in buf.  "buf" may or may not be the real
** text buffer for the widget.  If it is not the widget's text buffer it's
** offset position from the real buffer must be specified in "bufOffset" to
** allow the smart-indent routines to scan back as far as necessary. The
** string length is returned in "length" (or "length" can be passed as nullptr,
** and the indent column is returned in "column" (if non nullptr).
*/
char_type *NirvanaQt::createIndentString(TextBuffer *buf, int bufOffset, int lineStartPos, int lineEndPos, int *length,
                                    int *column) {
    int pos;
    int indent = -1;
    int tabDist = buffer_->BufGetTabDistance();
    int i;
    bool useTabs = buffer_->BufGetUseTabs();
    char_type *indentPtr;
    char_type *indentStr;
    char_type c;

    /* If smart indent is on, call the smart indent callback.  It is not
       called when multi-line changes are being made (lineStartPos != 0),
       because smart indent needs to search back an indeterminate distance
       through the buffer, and reconciling that with wrapping changes made,
       but not yet committed in the buffer, would make programming smart
       indent more difficult for users and make everything more complicated */
    if (smartIndent_ && (lineStartPos == 0 || buf == buffer_)) {
        Q_UNUSED(bufOffset);
#if 0
        smartIndentCBStruct smartIndent;
        smartIndent.reason        = NEWLINE_INDENT_NEEDED;
        smartIndent.pos           = lineEndPos + bufOffset;
        smartIndent.indentRequest = 0;
        smartIndent.charsTyped    = nullptr;
        XtCallCallbacks((Widget)tw, textNsmartIndentCallback, (XtPointer)&smartIndent);
        indent = smartIndent.indentRequest;
#endif
    }

    /* If smart indent wasn't used, measure the indent distance of the line */
    if (indent == -1) {
        indent = 0;
        for (pos = lineStartPos; pos < lineEndPos; pos++) {
            c = buf->BufGetCharacter(pos);
            if (c != ' ' && c != '\t')
                break;
            if (c == '\t')
                indent += tabDist - (indent % tabDist);
            else
                indent++;
        }
    }

    /* Allocate and create a string of tabs and spaces to achieve the indent */
    indentPtr = indentStr = new char_type[indent + 2];
    *indentPtr++ = '\n';
    if (useTabs) {
        for (i = 0; i < indent / tabDist; i++)
            *indentPtr++ = '\t';
        for (i = 0; i < indent % tabDist; i++)
            *indentPtr++ = ' ';
    } else {
        for (i = 0; i < indent; i++)
            *indentPtr++ = ' ';
    }
    *indentPtr = '\0';

    /* Return any requested stats */
    if (length != nullptr)
        *length = indentPtr - indentStr;

    if (column != nullptr)
        *column = indent;

    return indentStr;
}

/*
** Same as BufCountLines, but takes in to account wrapping if wrapping is
** turned on.  If the caller knows that startPos is at a line start, it
** can pass "startPosIsLineStart" as true to make the call more efficient
** by avoiding the additional step of scanning back to the last newline.
*/
int NirvanaQt::TextDCountLines(int startPos, int endPos, bool startPosIsLineStart) {
    int retLines, retPos, retLineStart, retLineEnd;

    /* If we're not wrapping use simple (and more efficient) BufCountLines */
    if (!continuousWrap_)
        return buffer_->BufCountLines(startPos, endPos);

    wrappedLineCounter(buffer_, startPos, endPos, INT_MAX, startPosIsLineStart, 0, &retPos, &retLines, &retLineStart,
                       &retLineEnd);
    return retLines;
}

/*
** Translate a buffer text position to the XY location where the center
** of the cursor would be positioned to point to that character.  Returns
** false if the position is not displayed because it is VERTICALLY out
** of view.  If the position is horizontally out of view, returns the
** x coordinate where the position would be if it were visible.
*/
bool NirvanaQt::TextDPositionToXY(int pos, int *x, int *y) {
    int charIndex, lineStartPos, fontHeight, lineLen;
    int visLineNum, charLen, outIndex, xStep;
    char_type *lineStr, expandedChar[MAX_EXP_CHAR_LEN];

    /* If position is not displayed, return false */
    if (pos < firstChar_ || (pos > lastChar_ && !emptyLinesVisible()))
        return false;

    /* Calculate y coordinate */
    if (!posToVisibleLineNum(pos, &visLineNum))
        return false;

    fontHeight = viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent();
    *y = top_ + visLineNum * fontHeight + fontHeight / 2;

    /* Get the text, length, and  buffer position of the line. If the position
       is beyond the end of the buffer and should be at the first position on
       the first empty line, don't try to get or scan the text  */
    lineStartPos = lineStarts_[visLineNum];
    if (lineStartPos == -1) {
        *x = left_ - horizOffset_;
        return true;
    }
    lineLen = visLineLength(visLineNum);
    lineStr = buffer_->BufGetRange(lineStartPos, lineStartPos + lineLen);

    /* Step through character positions from the beginning of the line
       to "pos" to calculate the x coordinate */
    xStep = left_ - horizOffset_;
    outIndex = 0;
    for (charIndex = 0; charIndex < pos - lineStartPos; charIndex++) {
        charLen = TextBuffer::BufExpandCharacter(lineStr[charIndex], outIndex, expandedChar,
                                                 buffer_->BufGetTabDistance(), buffer_->BufGetNullSubsChar());
        int charStyle = styleOfPos(lineStartPos, lineLen, charIndex, outIndex, lineStr[charIndex]);
        xStep += stringWidth(expandedChar, charLen, charStyle);
        outIndex += charLen;
    }
    *x = xStep;
    delete[] lineStr;
    return true;
}

/*
** Callback attached to the text buffer to receive delete information before
** the modifications are actually made.
*/
void NirvanaQt::preDelete(const PreDeleteEvent *event) {
    const int pos = event->pos;
    const int nDeleted = event->nDeleted;

    if (continuousWrap_ && (fixedFontWidth_ == -1 || modifyingTabDist_)) {
        /* Note: we must perform this measurement, even if there is not a
        * single character deleted; the number of "deleted" lines is the
        * number of visual lines spanned by the real line in which the
        * modification takes place.
        * Also, a modification of the tab distance requires the same
        * kind of calculations in advance, even if the font width is "fixed",
        * because when the width of the tab characters changes, the layout
        * of the text may be completely different.
        */
        measureDeletedLines(pos, nDeleted);
    } else {
        suppressResync_ = false; /* Probably not needed, but just in case */
    }
}

/*
** Callback attached to the text buffer to receive modification information
*/
void NirvanaQt::bufferModified(const ModifyEvent *event) {

    const int pos                      = event->pos;
    const int nInserted                = event->nInserted;
    const int nDeleted                 = event->nDeleted;
    const int nRestyled                = event->nRestyled;
    const char_type *const deletedText = event->deletedText;

    // NOTE(eteran): a bit of a hack, there were multiple callbacks
    // but the object based event system wants a seperate event for each
    // watcher. So, we just manually call the handler here .. for now
    modifiedCB(pos, nInserted, nDeleted, nRestyled, deletedText);

    int linesInserted;
    int linesDeleted;
    int startDispPos;
    int endDispPos;
    int oldFirstChar = firstChar_;
    bool scrolled;
    int origCursorPos = cursorPos_;
    int wrapModStart;
    int wrapModEnd;

    TextBuffer *const styleBuffer = syntaxHighlighter_->styleBuffer();

    /* buffer modification cancels vertical cursor motion column */
    if (nInserted != 0 || nDeleted != 0) {
        cursorPreferredCol_ = -1;
    }

    /* Count the number of lines inserted and deleted, and in the case
     * of continuous wrap mode, how much has changed */
    if (continuousWrap_) {
        findWrapRange(deletedText, pos, nInserted, nDeleted, &wrapModStart, &wrapModEnd, &linesInserted, &linesDeleted);
    } else {
        linesInserted = nInserted == 0 ? 0 : buffer_->BufCountLines(pos, pos + nInserted);
        linesDeleted = nDeleted == 0 ? 0 : countLines(deletedText);
    }

    /* Update the line starts and topLineNum */
    if (nInserted != 0 || nDeleted != 0) {
        if (continuousWrap_) {
            updateLineStarts(wrapModStart, wrapModEnd - wrapModStart,
                             nDeleted + pos - wrapModStart + (wrapModEnd - (pos + nInserted)), linesInserted,
                             linesDeleted, &scrolled);
        } else {
            updateLineStarts(pos, nInserted, nDeleted, linesInserted, linesDeleted, &scrolled);
        }
    } else {
        scrolled = false;
    }

    /* If we're counting non-wrapped lines as well, maintain the absolute
     * (non-wrapped) line number of the text displayed */
    if (maintainingAbsTopLineNum() && (nInserted != 0 || nDeleted != 0)) {
        if (pos + nDeleted < oldFirstChar) {
            absTopLineNum_ += buffer_->BufCountLines(pos, pos + nInserted) - countLines(deletedText);
        } else if (pos < oldFirstChar) {
            resetAbsLineNum();
        }
    }

    /* Update the line count for the whole buffer */
    nBufferLines_ += linesInserted - linesDeleted;

    /* Update the scroll bar ranges (and value if the value changed).  Note
     * that updating the horizontal scroll bar range requires scanning the
     * entire displayed text, however, it doesn't seem to hurt performance
     * much.  Note also, that the horizontal scroll bar update routine is
     * allowed to re-adjust horizOffset if there is blank space to the right
    * of all lines of text. */
    updateVScrollBarRange();
    scrolled |= updateHScrollBarRange();

    /* Update the cursor position */
    if (cursorToHint_ != NoCursorHint) {
        cursorPos_ = cursorToHint_;
        cursorToHint_ = NoCursorHint;
    } else if (cursorPos_ > pos) {
        if (cursorPos_ < pos + nDeleted) {
            cursorPos_ = pos;
        } else {
            cursorPos_ += nInserted - nDeleted;
        }
    }

    /* If the changes caused scrolling, re-paint everything and we're done. */
    if (scrolled) {
        blankCursorProtrusions();
        TextDRedisplayRect(0, top_, viewport()->width() + left_, viewport()->height());
        if (styleBuffer) { /* See comments in extendRangeForStyleMods */
            styleBuffer->BufGetPrimarySelection().selected = false;
            styleBuffer->BufGetPrimarySelection().zeroWidth = false;
        }
        return;
    }

    /* If the changes didn't cause scrolling, decide the range of characters
     * that need to be re-painted.  Also if the cursor position moved, be
     * sure that the redisplay range covers the old cursor position so the
     * old cursor gets erased, and erase the bits of the cursor which extend
     * beyond the left and right edges of the text. */
    startDispPos = continuousWrap_ ? wrapModStart : pos;
    if (origCursorPos == startDispPos && cursorPos_ != startDispPos) {
        startDispPos = qMin(startDispPos, origCursorPos - 1);
    }

    if (linesInserted == linesDeleted) {
        if (nInserted == 0 && nDeleted == 0) {
            endDispPos = pos + nRestyled;
        } else {
            endDispPos = continuousWrap_ ? wrapModEnd : buffer_->BufEndOfLine(pos + nInserted) + 1;
            if (origCursorPos >= startDispPos &&
                (origCursorPos <= endDispPos || endDispPos == buffer_->BufGetLength())) {
                blankCursorProtrusions();
            }
        }

        /* If more than one line is inserted/deleted, a line break may have
         * been inserted or removed in between, and the line numbers may
         * have changed. If only one line is altered, line numbers cannot
         * be affected (the insertion or removal of a line break always
         * results in at least two lines being redrawn). */
        if (linesInserted > 1) {
#if 0
            redrawLineNumbers(false);
#else
            // TODO(eteran): force a redraw of the line numbers
#endif
        }
    } else { /* linesInserted != linesDeleted */
        endDispPos = lastChar_ + 1;
        if (origCursorPos >= pos) {
            blankCursorProtrusions();
        }

#if 0
        // NOTE(eteran): added this here to update line number width, not 100% sure where this code is ideally
        int lineNumberColumns = nBufferLines_ < 1 ? 1 : static_cast<int>(log10(static_cast<double>(nBufferLines_) + 1)) + 1;
        int charWidth  = fixedFontWidth_;

        lineNumberColumns = qMax(lineNumberColumns, 4);

        lineNumLeft_ = charWidth / 2;
        lineNumWidth_ = lineNumberColumns * charWidth;
        left_ = lineNumLeft_ + lineNumWidth_ + (charWidth / 2);

#endif

#if 0
        redrawLineNumbers(false);
#else
            // TODO(eteran): force a redraw of the line numbers
#endif
    }

    /* If there is a style buffer, check if the modification caused additional
     * changes that need to be redisplayed.  (Redisplaying separately would
     * cause double-redraw on almost every modification involving styled
     * text).  Extend the redraw range to incorporate style changes */
    if (styleBuffer) {
        extendRangeForStyleMods(&startDispPos, &endDispPos);
    }

    /* Redisplay computed range */
    textDRedisplayRange(startDispPos, endDispPos);
}

/*
** Update the line starts array, topLineNum, firstChar and lastChar for text
** display "textD" after a modification to the text buffer, given by the
** position where the change began "pos", and the nmubers of characters
** and lines inserted and deleted.
*/
void NirvanaQt::updateLineStarts(int pos, int charsInserted, int charsDeleted, int linesInserted, int linesDeleted, bool *scrolled) {

    int i;
    int lineOfPos;
    int lineOfEnd;
    const int nVisLines = nVisibleLines_;
    const int charDelta = charsInserted - charsDeleted;
    const int lineDelta = linesInserted - linesDeleted;

    /* {   int i;
        printf("linesDeleted %d, linesInserted %d, charsInserted %d, charsDeleted
    %d\n",
                linesDeleted, linesInserted, charsInserted, charsDeleted);
        printf("lineStarts Before: ");
        for(i=0; i<nVisLines; i++) printf("%d ", lineStarts_[i]);
        printf("\n");
    } */
    /* If all of the changes were before the displayed text, the display
       doesn't change, just update the top line num and offset the line
       start entries and first and last characters */
    if (pos + charsDeleted < firstChar_) {
        topLineNum_ += lineDelta;
        for (i = 0; i < nVisLines && lineStarts_[i] != -1; i++)
            lineStarts_[i] += charDelta;
        /* {   int i;
            printf("lineStarts after delete doesn't touch: ");
            for(i=0; i<nVisLines; i++) printf("%d ", lineStarts_[i]);
            printf("\n");
        } */
        firstChar_ += charDelta;
        lastChar_ += charDelta;
        *scrolled = false;
        return;
    }

    /* The change began before the beginning of the displayed text, but
       part or all of the displayed text was deleted */
    if (pos < firstChar_) {
        /* If some text remains in the window, anchor on that  */
        if (posToVisibleLineNum(pos + charsDeleted, &lineOfEnd) && ++lineOfEnd < nVisLines &&
            lineStarts_[lineOfEnd] != -1) {
            topLineNum_ = qMax(1, topLineNum_ + lineDelta);
            firstChar_ = TextDCountBackwardNLines(lineStarts_[lineOfEnd] + charDelta, lineOfEnd);
            /* Otherwise anchor on original line number and recount everything */
        } else {
            if (topLineNum_ > nBufferLines_ + lineDelta) {
                topLineNum_ = 1;
                firstChar_ = 0;
            } else
                firstChar_ = TextDCountForwardNLines(0, topLineNum_ - 1, true);
        }
        calcLineStarts(0, nVisLines - 1);
        /* {   int i;
            printf("lineStarts after delete encroaches: ");
            for(i=0; i<nVisLines; i++) printf("%d ", lineStarts_[i]);
            printf("\n");
        } */
        /* calculate lastChar by finding the end of the last displayed line */
        calcLastChar();
        *scrolled = true;
        return;
    }

    /* If the change was in the middle of the displayed text (it usually is),
       salvage as much of the line starts array as possible by moving and
       offsetting the entries after the changed area, and re-counting the
       added lines or the lines beyond the salvaged part of the line starts
       array */
    if (pos <= lastChar_) {
        /* find line on which the change began */
        posToVisibleLineNum(pos, &lineOfPos);
        /* salvage line starts after the changed area */
        if (lineDelta == 0) {
            for (i = lineOfPos + 1; i < nVisLines && lineStarts_[i] != -1; i++)
                lineStarts_[i] += charDelta;
        } else if (lineDelta > 0) {
            for (i = nVisLines - 1; i >= lineOfPos + lineDelta + 1; i--)
                lineStarts_[i] = lineStarts_[i - lineDelta] + (lineStarts_[i - lineDelta] == -1 ? 0 : charDelta);
        } else /* (lineDelta < 0) */ {
            for (i = qMax(0, lineOfPos + 1); i < nVisLines + lineDelta; i++)
                lineStarts_[i] = lineStarts_[i - lineDelta] + (lineStarts_[i - lineDelta] == -1 ? 0 : charDelta);
        }
        /* {   int i;
            printf("lineStarts after salvage: ");
            for(i=0; i<nVisLines; i++) printf("%d ", lineStarts_[i]);
            printf("\n");
        } */
        /* fill in the missing line starts */
        if (linesInserted >= 0)
            calcLineStarts(lineOfPos + 1, lineOfPos + linesInserted);
        if (lineDelta < 0)
            calcLineStarts(nVisLines + lineDelta, nVisLines);
        /* {   int i;
            printf("lineStarts after recalculation: ");
            for(i=0; i<nVisLines; i++) printf("%d ", lineStarts_[i]);
            printf("\n");
        } */
        /* calculate lastChar by finding the end of the last displayed line */
        calcLastChar();
        *scrolled = false;
        return;
    }

    /* Change was past the end of the displayed text, but displayable by virtue
       of being an insert at the end of the buffer into visible blank lines */
    if (emptyLinesVisible()) {
        posToVisibleLineNum(pos, &lineOfPos);
        calcLineStarts(lineOfPos, lineOfPos + linesInserted);
        calcLastChar();
        /* {   int i;
            printf("lineStarts after insert at end: ");
            for(i=0; i<nVisLines; i++) printf("%d ", lineStarts_[i]);
            printf("\n");
        } */
        *scrolled = false;
        return;
    }

    /* Change was beyond the end of the buffer and not visible, do nothing */
    *scrolled = false;
}

/*
** Refresh the line number area.  If clearAll is false, writes only over
** the character cell areas.  Setting clearAll to true will clear out any
** stray marks outside of the character cell area, which might have been
** left from before a resize or font change.
*/
void NirvanaQt::redrawLineNumbers(QPainter *painter, bool clearAll) {

    int y;
    int line;
    int visLine;
    int nCols;
    int lineStart;
    char_type lineNumString[12];
    int lineHeight = viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent();
    int charWidth  = fixedFontWidth_;

    /* Don't draw if lineNumWidth == 0 (line numbers are hidden), or widget is
       not yet realized */
    if (lineNumWidth_ == 0) {
        return;
	}
	
	Q_UNUSED(nCols);

    /* Erase the previous contents of the line number area, if requested */
    if (clearAll) {
#if 0
        XClearArea(textD->lineNumLeft, textD->top, textD->lineNumWidth, viewport()->height(), false);
#else
        //painter->fillRect(QRectF(lineNumLeft_, top_, lineNumWidth_ + (left_ - (lineNumLeft_ + lineNumWidth_)), viewport()->height()), viewport()->palette().base().color());
#endif
    }

    /* Draw the line numbers, aligned to the text */
    nCols = qMin(11, lineNumWidth_ / charWidth);
    y = top_;
    line = getAbsTopLineNum();
    for (visLine=0; visLine < nVisibleLines_; visLine++) {
        lineStart = lineStarts_[visLine];
        if (lineStart != -1 && (lineStart==0 || buffer_->BufGetCharacter(lineStart-1)=='\n')) {
            sprintf(lineNumString, "%d", line);
#if 0
            XDrawImageString(
                        XtDisplay(textD->w),
                        XtWindow(textD->w),
                        textD->lineNumGC,
                        textD->lineNumLeft,
                        y + textD->ascent,
                        lineNumString,
                        traits_type::length(lineNumString));
#else
            painter->drawText(
                        QRectF(lineNumLeft_, y, lineNumWidth_, lineHeight),
                        Qt::AlignTop | Qt::AlignRight,
                        QString::fromLatin1(lineNumString));
#endif
            line++;
        } else {
#if 0
            XClearArea(textD->lineNumLeft, y, textD->lineNumWidth, textD->ascent + textD->descent, false);
#else
            //painter->fillRect(QRectF(lineNumLeft_, y, lineNumWidth_ + (left_ - (lineNumLeft_ + lineNumWidth_)), lineHeight), viewport()->palette().base().color());
#endif
            if (visLine == 0)
                line++;
        }
        y += lineHeight;
    }
}

/*
** Update the minimum, maximum, slider size, page increment, and value
** for vertical scroll bar.
*/
void NirvanaQt::updateVScrollBarRange() {

    /* The Vert. scroll bar value and slider size directly represent the top
       line number, and the number of visible lines respectively.  The scroll
       bar maximum value is chosen to generally represent the size of the whole
       buffer, with minor adjustments to keep the scroll bar widget happy */
    if (continuousWrap_) {
        verticalScrollBar()->setMaximum(qMax(0, nBufferLines_ + 2 + cursorVPadding_ - nVisibleLines_));
    } else {
        verticalScrollBar()->setMaximum(qMax(0, nBufferLines_ - nVisibleLines_));
    }
    verticalScrollBar()->setPageStep(qMax(1, nVisibleLines_ - 1));
}

/*
** Update the minimum, maximum, slider size, page increment, and value
** for the horizontal scroll bar.  If scroll position is such that there
** is blank space to the right of all lines of text, scroll back (adjust
** horizOffset but don't redraw) to take up the slack and position the
** right edge of the text at the right edge of the display.
**
** Note, there is some cost to this routine, since it scans the whole range
** of displayed text, particularly since it's usually called for each typed
** character!
*/
bool NirvanaQt::updateHScrollBarRange() {
    int maxWidth = 0;
    int sliderMax;
    int sliderWidth;
    int origHOffset = horizOffset_;

    /* Scan all the displayed lines to find the width of the longest line */
    for (int i = 0; i < nVisibleLines_ && lineStarts_[i] != -1; i++) {
        maxWidth = qMax(measureVisLine(i), maxWidth);
    }

    // account for the line number width if necessary
    maxWidth += left_;

    /* If the scroll position is beyond what's necessary to keep all lines
       in view, scroll to the left to bring the end of the longest line to
       the right margin */
    if (maxWidth < viewport()->width() + horizOffset_ && horizOffset_ > 0) {
        horizOffset_ = qMax(0, maxWidth - viewport()->width());
    }

    /* Readjust the scroll bar */
    sliderWidth = viewport()->width();
    sliderMax = qMax(maxWidth, sliderWidth + horizOffset_);

    horizontalScrollBar()->setMaximum(qMax(sliderMax - viewport()->width(), 0));
    horizontalScrollBar()->setPageStep(qMax(viewport()->width() - 100, 10));

    /* Return true if scroll position was changed */
    return origHOffset != horizOffset_;
}

/*
** Count lines from the beginning of the buffer to reestablish the
** absolute (non-wrapped) top line number.  If mode is not continuous wrap,
** or the number is not being maintained, does nothing.
*/
void NirvanaQt::resetAbsLineNum() {
    absTopLineNum_ = 1;
    offsetAbsLineNum(0);
}

/*
** Re-calculate absolute top line number for a change in scroll position.
*/
void NirvanaQt::offsetAbsLineNum(int oldFirstChar) {

    if (maintainingAbsTopLineNum()) {
        if (firstChar_ < oldFirstChar) {
            absTopLineNum_ -= buffer_->BufCountLines(firstChar_, oldFirstChar);
        } else {
            absTopLineNum_ += buffer_->BufCountLines(oldFirstChar, firstChar_);
        }
    }
}

/*
** Refresh a rectangle of the text display.  left and top are in coordinates of
** the text drawing window
*/
void NirvanaQt::TextDRedisplayRect(int left, int top, int width, int height) {
#if 1
    // This may be enough
    viewport()->update(QRect(left, top, width, height));
#else
    int fontHeight, firstLine, lastLine, line;

    /* find the line number range of the display */
    fontHeight = textD->ascent + textD->descent;
    firstLine = (top - textD->top - fontHeight + 1) / fontHeight;
    lastLine = (top + height - textD->top) / fontHeight;

    /* If the graphics contexts are shared using XtAllocateGC, their
       clipping rectangles may have changed since the last use */
    resetClipRectangles(textD);

    /* draw the lines of text */
    for (line = firstLine; line <= lastLine; line++)
        redisplayLine(textD, line, left, left + width, 0, INT_MAX);

    /* draw the line numbers if exposed area includes them */
    if (textD->lineNumWidth != 0 && left <= textD->lineNumLeft + textD->lineNumWidth)
        redrawLineNumbers(textD, false);
#endif
}

/*
** Extend the range of a redraw request (from *start to *end) with additional
** redraw requests resulting from changes to the attached style buffer (which
** contains auxiliary information for coloring or styling text).
*/
void NirvanaQt::extendRangeForStyleMods(int *start, int *end) {

    TextBuffer *styleBuffer = syntaxHighlighter_->styleBuffer();
    Selection *sel = &styleBuffer->BufGetPrimarySelection();
    bool extended = false;

    /* The peculiar protocol used here is that modifications to the style
       buffer are marked by selecting them with the buffer's primary selection.
       The style buffer is usually modified in response to a modify callback on
       the text buffer BEFORE textDisp.c's modify callback, so that it can keep
       the style buffer in step with the text buffer.  The style-update
       callback can't just call for a redraw, because textDisp hasn't processed
       the original text changes yet.  Anyhow, to minimize redrawing and to
       avoid the complexity of scheduling redraws later, this simple protocol
       tells the text display's buffer modify callback to extend it's redraw
       range to show the text color/and font changes as well. */
    if (sel->selected) {
        if (sel->start < *start) {
            *start = sel->start;
            extended = true;
        }

        if (sel->end > *end) {
            *end = sel->end;
            extended = true;
        }
    }

    /* If the selection was extended due to a style change, and some of the
       fonts don't match in spacing, extend redraw area to end of line to
       redraw characters exposed by possible font size changes */
    if (fixedFontWidth_ == -1 && extended) {
        *end = buffer_->BufEndOfLine(*end) + 1;
    }
}

/*
** When continuous wrap is on, and the user inserts or deletes characters,
** wrapping can happen before and beyond the changed position.  This routine
** finds the extent of the changes, and counts the deleted and inserted lines
** over that range.  It also attempts to minimize the size of the range to
** what has to be counted and re-displayed, so the results can be useful
** both for delimiting where the line starts need to be recalculated, and
** for deciding what part of the text to redisplay.
*/
void NirvanaQt::findWrapRange(const char_type *deletedText, int pos, int nInserted, int nDeleted, int *modRangeStart,
                              int *modRangeEnd, int *linesInserted, int *linesDeleted) {

    int length;
    int retPos;
    int retLines;
    int retLineStart;
    int retLineEnd;
    int nVisLines = nVisibleLines_;
    int countFrom;
    int countTo;
    int lineStart;
    int adjLineStart;
    int i;
    int visLineNum = 0;
    int nLines = 0;

    /*
    ** Determine where to begin searching: either the previous newline, or
    ** if possible, limit to the start of the (original) previous displayed
    ** line, using information from the existing line starts array
    */
    if (pos >= firstChar_ && pos <= lastChar_) {

        for (i = nVisLines - 1; i > 0; i--) {
            if (lineStarts_[i] != -1 && pos >= lineStarts_[i]) {
                break;
            }
        }

        if (i > 0) {
            countFrom = lineStarts_[i - 1];
            visLineNum = i - 1;
        } else {
            countFrom = buffer_->BufStartOfLine(pos);
        }
    } else {
        countFrom = buffer_->BufStartOfLine(pos);
    }

    /*
    ** Move forward through the (new) text one line at a time, counting
    ** displayed lines, and looking for either a real newline, or for the
    ** line starts to re-sync with the original line starts array
    */
    lineStart = countFrom;
    *modRangeStart = countFrom;
    while (true) {

        /* advance to the next line.  If the line ended in a real newline
         * or the end of the buffer, that's far enough */
        wrappedLineCounter(buffer_, lineStart, buffer_->BufGetLength(), 1, true, 0, &retPos, &retLines, &retLineStart,
                           &retLineEnd);
        if (retPos >= buffer_->BufGetLength()) {
            countTo = buffer_->BufGetLength();
            *modRangeEnd = countTo;

            if (retPos != retLineEnd) {
                nLines++;
            }

            break;
        } else {
            lineStart = retPos;
        }

        nLines++;
        if (lineStart > pos + nInserted && buffer_->BufGetCharacter(lineStart - 1) == '\n') {
            countTo = lineStart;
            *modRangeEnd = lineStart;
            break;
        }

        /* Don't try to resync in continuous wrap mode with non-fixed font
         * sizes; it would result in a chicken-and-egg dependency between
         * the calculations for the inserted and the deleted lines.
         * If we're in that mode, the number of deleted lines is calculated in
         * advance, without resynchronization, so we shouldn't resynchronize
         * for the inserted lines either. */
        if (suppressResync_) {
            continue;
        }

        /* check for synchronization with the original line starts array
         * before pos, if so, the modified range can begin later */
        if (lineStart <= pos) {
            while (visLineNum < nVisLines && lineStarts_[visLineNum] < lineStart) {
                visLineNum++;
            }

            if (visLineNum < nVisLines && lineStarts_[visLineNum] == lineStart) {
                countFrom = lineStart;
                nLines = 0;

                if (visLineNum + 1 < nVisLines && lineStarts_[visLineNum + 1] != -1) {
                    *modRangeStart = qMin(pos, lineStarts_[visLineNum + 1] - 1);
                } else {
                    *modRangeStart = countFrom;
                }
            } else {
                *modRangeStart = qMin(*modRangeStart, lineStart - 1);
            }
        }

        /* check for synchronization with the original line starts array
         * after pos, if so, the modified range can end early */
        else if (lineStart > pos + nInserted) {
            adjLineStart = lineStart - nInserted + nDeleted;
            while (visLineNum < nVisLines && lineStarts_[visLineNum] < adjLineStart) {
                visLineNum++;
            }

            if (visLineNum < nVisLines && lineStarts_[visLineNum] != -1 && lineStarts_[visLineNum] == adjLineStart) {
                countTo = TextDEndOfLine(lineStart, true);
                *modRangeEnd = lineStart;
                break;
            }
        }
    }

    *linesInserted = nLines;

    /* Count deleted lines between countFrom and countTo as the text existed
     * before the modification (that is, as if the text between pos and
     * pos+nInserted were replaced by "deletedText").  This extra context is
     * necessary because wrapping can occur outside of the modified region
     * as a result of adding or deleting text in the region. This is done by
     * creating a textBuffer containing the deleted text and the necessary
     * additional context, and calling the wrappedLineCounter on it.
     *
     * NOTE: This must not be done in continuous wrap mode when the font
     * width is not fixed. In that case, the calculation would try
     * to access style information that is no longer available (deleted
     * text), or out of date (updated highlighting), possibly leading
     * to completely wrong calculations and/or even crashes eventually.
     * (This is not theoretical; it really happened.)
     *
     * In that case, the calculation of the number of deleted lines
     * has happened before the buffer was modified (only in that case,
     * because resynchronization of the line starts is impossible
     * in that case, which makes the whole calculation less efficient).
     */
    if (suppressResync_) {
        *linesDeleted = nLinesDeleted_;
        suppressResync_ = false;
        return;
    }

    length = (pos - countFrom) + nDeleted + (countTo - (pos + nInserted));
    auto deletedTextBuf = new TextBuffer(length);

    if (pos > countFrom) {
        buffer_->BufCopyFromBuf(deletedTextBuf, countFrom, pos, 0);
    }

    if (nDeleted != 0) {
        deletedTextBuf->BufInsert(pos - countFrom, deletedText);
    }

    if (countTo > pos + nInserted) {
        buffer_->BufCopyFromBuf(deletedTextBuf, pos + nInserted, countTo, pos - countFrom + nDeleted);
    }

    /* Note that we need to take into account an offset for the style buffer:
     * the deletedTextBuf can be out of sync with the style buffer. */
    wrappedLineCounter(deletedTextBuf, 0, length, INT_MAX, true, countFrom, &retPos, &retLines, &retLineStart,
                       &retLineEnd);
    delete deletedTextBuf;
    *linesDeleted = retLines;
    suppressResync_ = false;
}

void NirvanaQt::deletePreviousCharacterAP() {
    int insertPos = TextDGetInsertPosition();
    char_type c;

    cancelDrag();
    if (checkReadOnly())
        return;

    TakeMotifDestination();
    if (deletePendingSelection())
        return;

    if (insertPos == 0) {
        bool silent = false; // hasKey("nobell", args, nArgs);
        ringIfNecessary(silent);
        return;
    }

    if (deleteEmulatedTab())
        return;

    if (overstrike_) {
        c = buffer_->BufGetCharacter(insertPos - 1);
        if (c == '\n')
            buffer_->BufRemove(insertPos - 1, insertPos);
        else if (c != '\t')
            buffer_->BufReplace(insertPos - 1, insertPos, " ", 1);
    } else {
        buffer_->BufRemove(insertPos - 1, insertPos);
    }

    TextDSetInsertPosition(insertPos - 1);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::deleteNextCharacterAP() {
    int insertPos = TextDGetInsertPosition();

    cancelDrag();
    if (checkReadOnly())
        return;

    TakeMotifDestination();
    if (deletePendingSelection())
        return;
    if (insertPos == buffer_->BufGetLength()) {
        bool silent = false; // hasKey("nobell", args, nArgs);
        ringIfNecessary(silent);
        return;
    }
    buffer_->BufRemove(insertPos, insertPos + 1);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::ringIfNecessary(bool silent) {
    if (!silent) {
        QApplication::beep();
    }
}

/*
** Cancel any drag operation that might be in progress.  Should be included
** in nearly every key event to cleanly end any dragging before edits are made
** which might change the insert position or the content of the buffer during
** a drag operation)
*/
void NirvanaQt::cancelDrag() {
    const int dragState = dragState_;

    autoScrollTimer_->stop();

    if (dragState == SECONDARY_DRAG || dragState == SECONDARY_RECT_DRAG) {
        buffer_->BufSecondaryUnselect();
    }

    if (dragState == PRIMARY_BLOCK_DRAG) {
        CancelBlockDrag();
    }

    if (dragState == MOUSE_PAN) {
#if 0
        XUngrabPointer(XtDisplay(w), CurrentTime);
#endif
    }

    if (dragState != NOT_CLICKED) {
        dragState_ = DRAG_CANCELED;
    }
}

bool NirvanaQt::checkReadOnly() {

    if (readOnly_) {
        QApplication::beep();
        return true;
    }

    return false;
}

void NirvanaQt::TakeMotifDestination() {
    // TODO(eteran): what did this do?
}

/*
** If there's a selection, delete it and position the cursor where the
** selection was deleted.  (Called by routines which do deletion to check
** first for and do possible selection delete)
*/
bool NirvanaQt::deletePendingSelection() {
    if (buffer_->BufGetPrimarySelection().selected) {
        buffer_->BufRemoveSelected();
        TextDSetInsertPosition(buffer_->BufGetCursorPosHint());
        checkAutoShowInsertPos();
        emitCursorMoved();
        return true;
    } else {
        return false;
    }
}

/*
** Check if tab emulation is on and if there are emulated tabs before the
** cursor, and if so, delete an emulated tab as a unit.  Also finishes up
** by calling checkAutoShowInsertPos and callCursorMovementCBs, so the
** calling action proc can just return (this is necessary to preserve
** emTabsBeforeCursor which is otherwise cleared by callCursorMovementCBs).
*/
bool NirvanaQt::deleteEmulatedTab() {
    const int emTabDist = emulateTabs_;
    const int emTabsBeforeCursor = emTabsBeforeCursor_;
    int startPos;
    int pos, indent, startPosIndent;
    char_type c;

    if (emTabDist <= 0 || emTabsBeforeCursor <= 0) {
        return false;
    }

    /* Find the position of the previous tab stop */
    int insertPos = TextDGetInsertPosition();
    int lineStart = buffer_->BufStartOfLine(insertPos);
    int startIndent = buffer_->BufCountDispChars(lineStart, insertPos);
    int toIndent = (startIndent - 1) - ((startIndent - 1) % emTabDist);

    /* Find the position at which to begin deleting (stop at non-whitespace
       characters) */
    startPosIndent = indent = 0;
    startPos = lineStart;
    for (pos = lineStart; pos < insertPos; pos++) {
        c = buffer_->BufGetCharacter(pos);
        indent += TextBuffer::BufCharWidth(c, indent, buffer_->BufGetTabDistance(), buffer_->BufGetNullSubsChar());
        if (indent > toIndent)
            break;
        startPosIndent = indent;
        startPos = pos + 1;
    }

    /* Just to make sure, check that we're not deleting any non-white chars */
    for (pos = insertPos - 1; pos >= startPos; pos--) {
        c = buffer_->BufGetCharacter(pos);
        if (c != ' ' && c != '\t') {
            startPos = pos + 1;
            break;
        }
    }

    /* Do the text replacement and reposition the cursor.  If any spaces need
       to be inserted to make up for a deleted tab, do a BufReplace, otherwise,
       do a BufRemove. */
    if (startPosIndent < toIndent) {
        auto spaceString = new char_type[toIndent - startPosIndent + 1];
        memset(spaceString, ' ', toIndent - startPosIndent);
        spaceString[toIndent - startPosIndent] = '\0';
        buffer_->BufReplace(startPos, insertPos, spaceString);
        TextDSetInsertPosition(startPos + toIndent - startPosIndent);
        delete[] spaceString;
    } else {
        buffer_->BufRemove(startPos, insertPos);
        TextDSetInsertPosition(startPos);
    }

    /* The normal cursor movement stuff would usually be called by the action
       routine, but this wraps around it to restore emTabsBeforeCursor */
    checkAutoShowInsertPos();
    emitCursorMoved();

    /* Decrement and restore the marker for consecutive emulated tabs, which
       would otherwise have been zeroed by callCursorMovementCBs */
    emTabsBeforeCursor_ = emTabsBeforeCursor - 1;
    return true;
}

void NirvanaQt::beginningOfLineAP(MoveMode mode) {
    int insertPos = TextDGetInsertPosition();

    cancelDrag();

    if (/*hasKey("absolute", args, nArgs)*/ true) {
        TextDSetInsertPosition(buffer_->BufStartOfLine(insertPos));
    } else {
        TextDSetInsertPosition(TextDStartOfLine(insertPos));
    }

    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
    cursorPreferredCol_ = 0;
}

void NirvanaQt::endOfLineAP(MoveMode mode) {
    int insertPos = TextDGetInsertPosition();

    cancelDrag();
    if (/*hasKey("absolute", args, nArgs)*/ true)
        TextDSetInsertPosition(buffer_->BufEndOfLine(insertPos));
    else
        TextDSetInsertPosition(TextDEndOfLine(insertPos, false));
    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
    cursorPreferredCol_ = -1;
}

void NirvanaQt::beginningOfFileAP(MoveMode mode) {
    int insertPos = TextDGetInsertPosition();

    cancelDrag();
    if (/*hasKey("scrollbar", args, nArgs)*/ false) {
        if (topLineNum_ != 1) {
            TextDSetScroll(1, horizOffset_);
        }
    } else {
        TextDSetInsertPosition(0);
        checkMoveSelectionChange(insertPos, mode);
        checkAutoShowInsertPos();
        emitCursorMoved();
    }
}

void NirvanaQt::endOfFileAP(MoveMode mode) {
    int insertPos = TextDGetInsertPosition();
    int lastTopLine;

    cancelDrag();
    if (/*hasKey("scrollbar", args, nArgs)*/ false) {
        lastTopLine = qMax(1, nBufferLines_ - (nVisibleLines_ - 2) + cursorVPadding_);

        if (lastTopLine != topLineNum_) {
            TextDSetScroll(lastTopLine, horizOffset_);
        }
    } else {
        TextDSetInsertPosition(buffer_->BufGetLength());
        checkMoveSelectionChange(insertPos, mode);
        checkAutoShowInsertPos();
        emitCursorMoved();
    }
}

/*
** For actions involving cursor movement, "extend" keyword means incorporate
** the new cursor position in the selection, and lack of an "extend" keyword
** means cancel the existing selection
*/
void NirvanaQt::checkMoveSelectionChange(int startPos, MoveMode mode) {
    switch (mode) {
    case MoveExtendRect:
        keyMoveExtendSelection(startPos, true);
        break;
    case MoveExtend:
        keyMoveExtendSelection(startPos, false);
        break;
    default:
        buffer_->BufUnselect();
        break;
    }
}

/*
** If a selection change was requested via a keyboard command for moving
** the insertion cursor (usually with the "extend" keyword), adjust the
** selection to include the new cursor position, or begin a new selection
** between startPos and the new cursor position with anchor at startPos.
*/
void NirvanaQt::keyMoveExtendSelection(int origPos, bool rectangular) {
    Selection *sel = &buffer_->BufGetPrimarySelection();
    int newPos = TextDGetInsertPosition();
    int startPos;
    int endPos;
    int startCol;
    int endCol;
    int newCol;
    int origCol;
    int anchor;
    int rectAnchor;
    int anchorLineStart;

    /* Moving the cursor does not take the Motif destination, but as soon as
     * the user selects something, grab it (I'm not sure if this distinction
     * actually makes sense, but it's what Motif was doing, back when their
     * secondary selections actually worked correctly) */
    TakeMotifDestination();

    if ((sel->selected || sel->zeroWidth) && sel->rectangular && rectangular) {
        /* rect -> rect */
        newCol = buffer_->BufCountDispChars(buffer_->BufStartOfLine(newPos), newPos);
        startCol = qMin(rectAnchor_, newCol);
        endCol = qMax(rectAnchor_, newCol);
        startPos = buffer_->BufStartOfLine(qMin(anchor_, newPos));
        endPos = buffer_->BufEndOfLine(qMax(anchor_, newPos));

        buffer_->BufRectSelect(startPos, endPos, startCol, endCol);
    } else if (sel->selected && rectangular) { /* plain -> rect */
        newCol = buffer_->BufCountDispChars(buffer_->BufStartOfLine(newPos), newPos);

        if (abs(newPos - sel->start) < abs(newPos - sel->end)) {
            anchor = sel->end;
        } else {
            anchor = sel->start;
        }

        anchorLineStart = buffer_->BufStartOfLine(anchor);
        rectAnchor = buffer_->BufCountDispChars(anchorLineStart, anchor);
        anchor_ = anchor;
        rectAnchor_ = rectAnchor;
        buffer_->BufRectSelect(buffer_->BufStartOfLine(qMin(anchor, newPos)),
                               buffer_->BufEndOfLine(qMax(anchor, newPos)), qMin(rectAnchor, newCol),
                               qMax(rectAnchor, newCol));
    } else if (sel->selected && sel->rectangular) { /* rect -> plain */

        startPos = buffer_->BufCountForwardDispChars(buffer_->BufStartOfLine(sel->start), sel->rectStart);
        endPos = buffer_->BufCountForwardDispChars(buffer_->BufStartOfLine(sel->end), sel->rectEnd);

        if (abs(origPos - startPos) < abs(origPos - endPos)) {
            anchor = endPos;
        } else {
            anchor = startPos;
        }

        buffer_->BufSelect(anchor, newPos);
    } else if (sel->selected) { /* plain -> plain */

        if (abs(origPos - sel->start) < abs(origPos - sel->end)) {
            anchor = sel->end;
        } else {
            anchor = sel->start;
        }

        buffer_->BufSelect(anchor, newPos);
    } else if (rectangular) { /* no sel -> rect */

        origCol = buffer_->BufCountDispChars(buffer_->BufStartOfLine(origPos), origPos);
        newCol = buffer_->BufCountDispChars(buffer_->BufStartOfLine(newPos), newPos);
        startCol = qMin(newCol, origCol);
        endCol = qMax(newCol, origCol);
        startPos = buffer_->BufStartOfLine(qMin(origPos, newPos));
        endPos = buffer_->BufEndOfLine(qMax(origPos, newPos));
        anchor_ = origPos;
        rectAnchor_ = origCol;

        buffer_->BufRectSelect(startPos, endPos, startCol, endCol);
    } else { /* no sel -> plain */

        anchor_ = origPos;
        rectAnchor_ = buffer_->BufCountDispChars(buffer_->BufStartOfLine(origPos), origPos);
        buffer_->BufSelect(anchor_, newPos);
    }
}

void NirvanaQt::forwardWordAP(MoveMode mode) {
    int insertPos = TextDGetInsertPosition();

    cancelDrag();
    if (insertPos == buffer_->BufGetLength()) {
        bool silent = /*hasKey("nobell", args, nArgs);*/ false;
        ringIfNecessary(silent);
        return;
    }
    int pos = insertPos;

    if (/*hasKey("tail", args, nArgs)*/ false) {
        for (; pos < buffer_->BufGetLength(); pos++) {
            if (nullptr == strchr(delimiters_, buffer_->BufGetCharacter(pos))) {
                break;
            }
        }
        if (nullptr == strchr(delimiters_, buffer_->BufGetCharacter(pos))) {
            pos = endOfWord(pos);
        }
    } else {
        if (nullptr == strchr(delimiters_, buffer_->BufGetCharacter(pos))) {
            pos = endOfWord(pos);
        }
        for (; pos < buffer_->BufGetLength(); pos++) {
            if (nullptr == strchr(delimiters_, buffer_->BufGetCharacter(pos))) {
                break;
            }
        }
    }

    TextDSetInsertPosition(pos);
    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::backwardWordAP(MoveMode mode) {
    int insertPos = TextDGetInsertPosition();

    cancelDrag();
    if (insertPos == 0) {
        bool silent = /* hasKey("nobell", args, nArgs);*/ false;
        ringIfNecessary(silent);
        return;
    }
    int pos = qMax(insertPos - 1, 0);
    while (strchr(delimiters_, buffer_->BufGetCharacter(pos)) != nullptr && pos > 0)
        pos--;
    pos = startOfWord(pos);

    TextDSetInsertPosition(pos);
    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

int NirvanaQt::startOfWord(int pos) {

    int startPos;
    char_type c = buffer_->BufGetCharacter(pos);

    if (c == ' ' || c == '\t') {
        if (!spanBackward(buffer_, pos, " \t", false, &startPos)) {
            return 0;
        }
    } else if (strchr(delimiters_, c)) {
        if (!spanBackward(buffer_, pos, delimiters_, true, &startPos)) {
            return 0;
        }
    } else {
        if (!buffer_->BufSearchBackward(pos, delimiters_, &startPos)) {
            return 0;
        }
    }

    return qMin(pos, startPos + 1);
}

int NirvanaQt::endOfWord(int pos) {
    int endPos;
    char_type c = buffer_->BufGetCharacter(pos);

    if (c == ' ' || c == '\t') {
        if (!spanForward(buffer_, pos, " \t", false, &endPos)) {
            return buffer_->BufGetLength();
        }
    } else if (strchr(delimiters_, c)) {
        if (!spanForward(buffer_, pos, delimiters_, true, &endPos)) {
            return buffer_->BufGetLength();
        }
    } else {
        if (!buffer_->BufSearchForward(pos, delimiters_, &endPos)) {
            return buffer_->BufGetLength();
        }
    }

    return endPos;
}

/*
** Search forwards in buffer "buf" for the first character NOT in
** "searchChars",  starting with the character "startPos", and returning the
** result in "foundPos" returns true if found, false if not. If ignoreSpace
** is set, then Space, Tab, and Newlines are ignored in searchChars.
*/
bool NirvanaQt::spanForward(TextBuffer *buf, int startPos, const char_type *searchChars, bool ignoreSpace, int *foundPos) {

    int pos = startPos;
    while (pos < buf->BufGetLength()) {
        const char_type *c;
        for (c = searchChars; *c != '\0'; c++) {
            if (!(ignoreSpace && (*c == ' ' || *c == '\t' || *c == '\n'))) {
                if (buf->BufGetCharacter(pos) == *c) {
                    break;
                }
            }
        }

        if (*c == 0) {
            *foundPos = pos;
            return true;
        }

        pos++;
    }

    *foundPos = buf->BufGetLength();
    return false;
}

/*
** Search backwards in buffer "buf" for the first character NOT in
** "searchChars",  starting with the character BEFORE "startPos", returning the
** result in "foundPos" returns true if found, false if not. If ignoreSpace is
** set, then Space, Tab, and Newlines are ignored in searchChars.
*/
bool NirvanaQt::spanBackward(TextBuffer *buf, int startPos, const char_type *searchChars, bool ignoreSpace, int *foundPos) {

    if (startPos == 0) {
        *foundPos = 0;
        return false;
    }

    int pos = (startPos == 0) ? 0 : (startPos - 1);
    while (pos >= 0) {
        const char_type *c;
        for (c = searchChars; *c != '\0'; c++) {
            if (!(ignoreSpace && (*c == ' ' || *c == '\t' || *c == '\n'))) {
                if (buf->BufGetCharacter(pos) == *c) {
                    break;
                }
            }
        }

        if (*c == 0) {
            *foundPos = pos;
            return true;
        }

        pos--;
    }

    *foundPos = 0;
    return false;
}

void NirvanaQt::deletePreviousWordAP() {
    int insertPos = TextDGetInsertPosition();
    int pos;
    int lineStart = buffer_->BufStartOfLine(insertPos);
    bool silent = /*hasKey("nobell", args, nArgs);*/ false;

    cancelDrag();
    if (checkReadOnly()) {
        return;
    }

    TakeMotifDestination();
    if (deletePendingSelection()) {
        return;
    }

    if (insertPos == lineStart) {
        ringIfNecessary(silent);
        return;
    }

    pos = qMax(insertPos - 1, 0);
    while (strchr(delimiters_, buffer_->BufGetCharacter(pos)) != nullptr && pos != lineStart) {
        pos--;
    }

    pos = startOfWord(pos);
    buffer_->BufRemove(pos, insertPos);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::deleteNextWordAP() {
    int insertPos = TextDGetInsertPosition();
    int pos, lineEnd = buffer_->BufEndOfLine(insertPos);
    bool silent = /* hasKey("nobell", args, nArgs); */ false;

    cancelDrag();
    if (checkReadOnly()) {
        return;
    }

    TakeMotifDestination();
    if (deletePendingSelection()) {
        return;
    }

    if (insertPos == lineEnd) {
        ringIfNecessary(silent);
        return;
    }

    pos = insertPos;
    while (strchr(delimiters_, buffer_->BufGetCharacter(pos)) != nullptr && pos != lineEnd) {
        pos++;
    }

    pos = endOfWord(pos);
    buffer_->BufRemove(insertPos, pos);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::processUpAP(MoveMode mode) {
    const int insertPos = TextDGetInsertPosition();
    const bool silent = /* hasKey("nobell", args, nArgs);   */ false;
    const int abs = /* hasKey("absolute", args, nArgs); */ false;

    cancelDrag();

    if (!TextDMoveUp(abs)) {
        ringIfNecessary(silent);
    }

    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::processDownAP(MoveMode mode) {
    const int insertPos = TextDGetInsertPosition();
    const bool silent = /* hasKey("nobell", args, nArgs); */ false;
    const int abs = /* hasKey("absolute", args, nArgs); */ false;

    cancelDrag();
    if (!TextDMoveDown(abs))
        ringIfNecessary(silent);
    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::pasteClipboardAP(PasteMode pasteMode) {
    if (pasteMode == PasteColumnar) {
        TextColPasteClipboard();
    } else {
        TextPasteClipboard();
    }
}

void NirvanaQt::copyClipboardAP() {
    TextCopyClipboard();
}

void NirvanaQt::cutClipboardAP() {
    TextCutClipboard();
}

void NirvanaQt::TextPasteClipboard() {
    cancelDrag();
    if (checkReadOnly()) {
        return;
    }
    TakeMotifDestination();
    InsertClipboard(PasteStandard);
    emitCursorMoved();
}

void NirvanaQt::TextColPasteClipboard() {
    cancelDrag();
    if (checkReadOnly())
        return;
    TakeMotifDestination();
    InsertClipboard(PasteColumnar);
    emitCursorMoved();
}

void NirvanaQt::TextCopyClipboard() {
    cancelDrag();
    if (!buffer_->BufGetPrimarySelection().selected) {
        QApplication::beep();
    }
    CopyToClipboard();
}

void NirvanaQt::TextCutClipboard() {
    cancelDrag();
    if (checkReadOnly())
        return;
    if (!buffer_->BufGetPrimarySelection().selected) {
        QApplication::beep();
        return;
    }
    TakeMotifDestination();
    CopyToClipboard();
    buffer_->BufRemoveSelected();
    TextDSetInsertPosition(buffer_->BufGetCursorPosHint());
    emitCursorMoved();
}

/*
** Insert the X CLIPBOARD selection at the cursor position.  If isColumnar,
** do an BufInsertCol for a columnar paste instead of BufInsert.
*/
void NirvanaQt::InsertClipboard(PasteMode pasteMode) {
    unsigned long retLength;
    int cursorLineStart;
    int column;
    char_type *string;

    if (QClipboard *const clipboard = QApplication::clipboard()) {
        QString contents = clipboard->text();

        QByteArray latin1 = contents.toLatin1();

        retLength = latin1.size();
        string = latin1.data();

        /* If the string contains ascii-nul characters, substitute something
           else, or give up, warn, and refuse */
        if (!buffer_->BufSubstituteNullChars(string, retLength)) {
            qDebug() << "Too much binary data, text not pasted";
            return;
        }

        /* Insert it in the text widget */
        if (pasteMode == PasteColumnar && !buffer_->BufGetPrimarySelection().selected) {
            int cursorPos = TextDGetInsertPosition();
            cursorLineStart = buffer_->BufStartOfLine(cursorPos);
            column = buffer_->BufCountDispChars(cursorLineStart, cursorPos);

            if (overstrike_) {
                buffer_->BufOverlayRect(cursorLineStart, column, -1, string, nullptr, nullptr);
            } else {
                buffer_->BufInsertCol(column, cursorLineStart, string, nullptr, nullptr);
            }

            TextDSetInsertPosition(buffer_->BufCountForwardDispChars(cursorLineStart, column));

            if (autoShowInsertPos_) {
                TextDMakeInsertPosVisible();
            }
        } else {
            TextInsertAtCursor(string, true, autoWrapPastedText_);
        }
    }
}

/*
** Copy the primary selection to the clipboard
*/
void NirvanaQt::CopyToClipboard() {
    /* Get the selected text, if there's no selection, do nothing */
    char_type *text = buffer_->BufGetSelectionText();
    if (*text == '\0') {
        delete[] text;
        return;
    }

    /* If the string contained ascii-nul characters, something else was
       substituted in the buffer.  Put the nulls back */
    const int length = static_cast<int>(traits_type::length(text));
    buffer_->BufUnsubstituteNullChars(text);

    if (QClipboard *const clipboard = QApplication::clipboard()) {

        clipboard->setText(QString::fromLatin1(text, length));
    }

    delete[] text;
}

void NirvanaQt::setScroll(int topLineNum, int horizOffset, bool updateVScrollBar, bool updateHScrollBar) {
    int fontHeight = viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent();
    int origHOffset = horizOffset_;
    int lineDelta = topLineNum_ - topLineNum;
    int xOffset;
    int yOffset;
    int srcX;
    int srcY;
    int dstX;
    int dstY;
    int width;
    int height;
    int exactHeight = viewport()->height() - viewport()->height() % fontHeight;

    Q_UNUSED(fontHeight);
    Q_UNUSED(origHOffset);
    Q_UNUSED(lineDelta);
    Q_UNUSED(xOffset);
    Q_UNUSED(yOffset);
    Q_UNUSED(srcX);
    Q_UNUSED(srcY);
    Q_UNUSED(dstX);
    Q_UNUSED(dstY);
    Q_UNUSED(width);
    Q_UNUSED(height);
    Q_UNUSED(exactHeight);

    /* Do nothing if scroll position hasn't actually changed or there's no
       window to draw in yet */
    if (horizOffset_ == horizOffset && topLineNum_ == topLineNum) {
        return;
    }

    /* If part of the cursor is protruding beyond the text clipping region,
       clear it off */
    blankCursorProtrusions();

    /* If the vertical scroll position has changed, update the line
       starts array and related counters in the text display */
    offsetLineStarts(topLineNum);

    /* Just setting horizOffset_ is enough information for redisplay */
    horizOffset_ = horizOffset;

    /* Update the scroll bar positions if requested, note: updating the
       horizontal scroll bars can have the further side-effect of changing
       the horizontal scroll position, horizOffset_ */
    if (updateVScrollBar) {
        updateVScrollBarRange();
        verticalScrollBar()->setSliderPosition(topLineNum - 1);
    }
    if (updateHScrollBar) {
        updateHScrollBarRange();
        horizontalScrollBar()->setSliderPosition(horizOffset);
    }

#if 0
    /* Redisplay everything if the window is partially obscured (since
       it's too hard to tell what displayed areas are salvageable) or
       if there's nothing to recover because the scroll distance is large */
    xOffset = origHOffset - horizOffset_;
    yOffset = lineDelta * fontHeight;
    if (textD->visibility != VisibilityUnobscured || abs(xOffset) > viewport()->width() || abs(yOffset) > exactHeight) {
        TextDTranlateGraphicExposeQueue(xOffset, yOffset, false);
        TextDRedisplayRect(left_, top_, viewport()->width(), viewport()->height());
    } else {
        /* If the window is not obscured, paint most of the window using XCopyArea
           from existing displayed text, and redraw only what's necessary */
        /* Recover the useable window areas by moving to the proper location */
        srcX = textD->left + (xOffset >= 0 ? 0 : -xOffset);
        dstX = textD->left + (xOffset >= 0 ? xOffset : 0);
        width = viewport()->width() - abs(xOffset);
        srcY = textD->top + (yOffset >= 0 ? 0 : -yOffset);
        dstY = textD->top + (yOffset >= 0 ? yOffset : 0);
        height = exactHeight - abs(yOffset);
        resetClipRectangles();
        TextDTranlateGraphicExposeQueue(xOffset, yOffset, true);
        XCopyArea(XtDisplay(textD->w), XtWindow(textD->w), XtWindow(textD->w), textD->gc, srcX, srcY, width, height, dstX, dstY);
        /* redraw the un-recoverable parts */
        if (yOffset > 0) {
            TextDRedisplayRect(textD->left, textD->top, viewport()->width(), yOffset);
        }
        else if (yOffset < 0) {
            TextDRedisplayRect(textD, textD->left, textD->top + viewport()->height() + yOffset, viewport()->width(), -yOffset);
        }
        if (xOffset > 0) {
            TextDRedisplayRect(textD, textD->left, textD->top, xOffset, viewport()->height());
        }
        else if (xOffset < 0) {
            TextDRedisplayRect(textD, textD->left + viewport()->width() + xOffset, textD->top, -xOffset, viewport()->height());
        }
        /* Restore protruding parts of the cursor */
        textDRedisplayRange(textD->cursorPos-1, textD->cursorPos+1);
    }

    /* Refresh line number/calltip display if its up and we've scrolled
        vertically */
    if (lineDelta != 0) {
        redrawLineNumbers(false);
        TextDRedrawCalltip(0);
    }

    HandleAllPendingGraphicsExposeNoExposeEvents(nullptr);
#endif

    viewport()->update();
}

/*
** Offset the line starts array, topLineNum, firstChar and lastChar, for a new
** vertical scroll position given by newTopLineNum.  If any currently displayed
** lines will still be visible, salvage the line starts values, otherwise,
** count lines from the nearest known line start (start or end of buffer, or
** the closest value in the lineStarts array)
*/
void NirvanaQt::offsetLineStarts(int newTopLineNum) {
    int oldTopLineNum = topLineNum_;
    int oldFirstChar = firstChar_;
    int lineDelta = newTopLineNum - oldTopLineNum;
    int nVisLines = nVisibleLines_;
    int *lineStarts = lineStarts_.data();
    int i, lastLineNum;
    TextBuffer *buf = buffer_;

    /* If there was no offset, nothing needs to be changed */
    if (lineDelta == 0)
        return;

    /* {   int i;
        printf("Scroll, lineDelta %d\n", lineDelta);
        printf("lineStarts Before: ");
        for(i=0; i<nVisLines; i++) printf("%d ", lineStarts[i]);
        printf("\n");
    } */

    /* Find the new value for firstChar by counting lines from the nearest
       known line start (start or end of buffer, or the closest value in the
       lineStarts array) */
    lastLineNum = oldTopLineNum + nVisLines - 1;
    if (newTopLineNum < oldTopLineNum && newTopLineNum < -lineDelta) {
        firstChar_ = TextDCountForwardNLines(0, newTopLineNum - 1, true);
        /* printf("counting forward %d lines from start\n", newTopLineNum-1);*/
    } else if (newTopLineNum < oldTopLineNum) {
        firstChar_ = TextDCountBackwardNLines(firstChar_, -lineDelta);
        /* printf("counting backward %d lines from firstChar\n", -lineDelta);*/
    } else if (newTopLineNum < lastLineNum) {
        firstChar_ = lineStarts[newTopLineNum - oldTopLineNum];
        /* printf("taking new start from lineStarts[%d]\n", newTopLineNum -
         * oldTopLineNum); */
    } else if (newTopLineNum - lastLineNum < nBufferLines_ - newTopLineNum) {
        firstChar_ = TextDCountForwardNLines(lineStarts[nVisLines - 1], newTopLineNum - lastLineNum, true);
        /* printf("counting forward %d lines from start of last line\n",
         * newTopLineNum - lastLineNum); */
    } else {
        firstChar_ = TextDCountBackwardNLines(buf->BufGetLength(), nBufferLines_ - newTopLineNum + 1);
        /* printf("counting backward %d lines from end\n", textD->nBufferLines -
         * newTopLineNum + 1); */
    }

    /* Fill in the line starts array */
    if (lineDelta < 0 && -lineDelta < nVisLines) {
        for (i = nVisLines - 1; i >= -lineDelta; i--)
            lineStarts[i] = lineStarts[i + lineDelta];
        calcLineStarts(0, -lineDelta);
    } else if (lineDelta > 0 && lineDelta < nVisLines) {
        for (i = 0; i < nVisLines - lineDelta; i++)
            lineStarts[i] = lineStarts[i + lineDelta];
        calcLineStarts(nVisLines - lineDelta, nVisLines - 1);
    } else
        calcLineStarts(0, nVisLines);

    /* Set lastChar and topLineNum */
    calcLastChar();
    topLineNum_ = newTopLineNum;

    /* If we're numbering lines or being asked to maintain an absolute line
       number, re-calculate the absolute line number */
    offsetAbsLineNum(oldFirstChar);

    /* {   int i;
        printf("lineStarts After: ");
        for(i=0; i<nVisLines; i++) printf("%d ", lineStarts[i]);
        printf("\n");
    } */
}

/*
** Set the scroll position of the text display vertically by line number and
** horizontally by pixel offset from the left margin
*/
void NirvanaQt::TextDSetScroll(int topLineNum, int horizOffset) {
    int vPadding = (int)(cursorVPadding_);

    /* Limit the requested scroll position to allowable values */
    if (topLineNum < 1) {
        topLineNum = 1;
    } else if ((topLineNum > topLineNum_) && (topLineNum > (nBufferLines_ + 2 - nVisibleLines_ + vPadding))) {
        topLineNum = qMax(topLineNum_, nBufferLines_ + 2 - nVisibleLines_ + vPadding);
    }

    horizOffset = qBound(0, horizOffset, horizontalScrollBar()->maximum());
    setScroll(topLineNum, horizOffset, true, true);
}

/*
** Return the width in pixels of the displayed line pointed to by "visLineNum"
*/
int NirvanaQt::measureVisLine(int visLineNum) {
    int width = 0;
    int len;
    int lineLen = visLineLength(visLineNum);
    int charCount = 0;
    int lineStartPos = lineStarts_[visLineNum];
    char_type expandedChar[MAX_EXP_CHAR_LEN];
    TextBuffer *styleBuffer = syntaxHighlighter_->styleBuffer();

    if (styleBuffer == nullptr) {
        for (int i = 0; i < lineLen; i++) {
            len = buffer_->BufGetExpandedChar(lineStartPos + i, charCount, expandedChar);
            width += viewport()->fontMetrics().width(QString::fromLatin1(expandedChar, len));
            charCount += len;
        }
    } else {
        for (int i = 0; i < lineLen; i++) {
            len = buffer_->BufGetExpandedChar(lineStartPos + i, charCount, expandedChar);
            int style = (unsigned char)styleBuffer->BufGetCharacter(lineStartPos + i) - ASCII_A;
            Q_UNUSED(style);
#if 0
            width += XTextWidth(textD->styleTable[style].font, expandedChar, len);
#else
            // TODO(eteran): take into account style
            width += viewport()->fontMetrics().width(QString::fromLatin1(expandedChar, len));
#endif
            charCount += len;
        }
    }

    return width;
}

void NirvanaQt::forwardCharacterAP(MoveMode mode) {
    int insertPos = TextDGetInsertPosition();
    bool silent = /* hasKey("nobell", args, nArgs); */ false;

    cancelDrag();
    if (!TextDMoveRight()) {
        ringIfNecessary(silent);
    }
    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::backwardCharacterAP(MoveMode mode) {
    int insertPos = TextDGetInsertPosition();
    bool silent = /* hasKey("nobell", args, nArgs); */ false;

    cancelDrag();
    if (!TextDMoveLeft()) {
        ringIfNecessary(silent);
    }
    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::newlineAP() {
    if (autoIndent_ || smartIndent_) {
        newlineAndIndentAP();
    } else {
        newlineNoIndentAP();
    }
}

void NirvanaQt::newlineNoIndentAP() {
    cancelDrag();
    if (checkReadOnly()) {
        return;
    }

    TakeMotifDestination();
    simpleInsertAtCursor("\n", true);
    buffer_->BufUnselect();
}

void NirvanaQt::newlineAndIndentAP() {
    int column;

    if (checkReadOnly()) {
        return;
    }

    cancelDrag();
    TakeMotifDestination();

    /* Create a string containing a newline followed by auto or smart
     * indent string
     */
    int cursorPos = TextDGetInsertPosition();
    int lineStartPos = buffer_->BufStartOfLine(cursorPos);
    char_type *const indentStr = createIndentString(buffer_, 0, lineStartPos, cursorPos, nullptr, &column);

    /* Insert it at the cursor */
    simpleInsertAtCursor(indentStr, true);
    delete[] indentStr;

    if (emulateTabs_ > 0) {
        /*  If emulated tabs are on, make the inserted indent deletable by
         * tab. Round this up by faking the column a bit to the right to
         * let the user delete half-tabs with one keypress.
         */

        column += emulateTabs_ - 1;
        emTabsBeforeCursor_ = column / emulateTabs_;
    }

    buffer_->BufUnselect();
}

/*
** Decide whether the user needs (or may need) a horizontal scroll bar,
** and manage or unmanage the scroll bar widget accordingly.  The H.
** scroll bar is only hidden in continuous wrap mode when it's absolutely
** certain that the user will not need it: when wrapping is set
** to the window edge, or when the wrap margin is strictly less than
** the longest possible line.
*/
void NirvanaQt::hideOrShowHScrollBar() {
    if (continuousWrap_ && (wrapMargin_ == 0 || wrapMargin_ * fixedFontWidth_ < width())) {
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    } else {
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    }
}

void NirvanaQt::processTabAP() {
    Selection *const sel = &buffer_->BufGetPrimarySelection();
    int emTabDist = emulateTabs_;
    int emTabsBeforeCursor = emTabsBeforeCursor_;
    int indent;
    int tabWidth;
    char_type *outPtr;

    if (checkReadOnly()) {
        return;
    }

    cancelDrag();
    TakeMotifDestination();

    /* If emulated tabs are off, just insert a tab */
    if (emTabDist <= 0) {
        TextInsertAtCursor("\t", true, true);
        return;
    }

    /* Find the starting and ending indentation.  If the tab is to
       replace an existing selection, use the start of the selection
       instead of the cursor position as the indent.  When replacing
       rectangular selections, tabs are automatically recalculated as
       if the inserted text began at the start of the line */
    int insertPos = pendingSelection() ? sel->start : TextDGetInsertPosition();
    int lineStart = buffer_->BufStartOfLine(insertPos);

    if (pendingSelection() && sel->rectangular) {
        insertPos = buffer_->BufCountForwardDispChars(lineStart, sel->rectStart);
    }

    int startIndent = buffer_->BufCountDispChars(lineStart, insertPos);
    int toIndent = startIndent + emTabDist - (startIndent % emTabDist);

    if (pendingSelection() && sel->rectangular) {
        toIndent -= startIndent;
        startIndent = 0;
    }

    /* Allocate a buffer assuming all the inserted characters will be spaces */
    auto outStr = new char_type[toIndent - startIndent + 1];

    /* Add spaces and tabs to outStr until it reaches toIndent */
    outPtr = outStr;
    indent = startIndent;
    while (indent < toIndent) {
        tabWidth = TextBuffer::BufCharWidth('\t', indent, buffer_->BufGetTabDistance(), buffer_->BufGetNullSubsChar());
        if (buffer_->BufGetUseTabs() && tabWidth > 1 && indent + tabWidth <= toIndent) {
            *outPtr++ = '\t';
            indent += tabWidth;
        } else {
            *outPtr++ = ' ';
            indent++;
        }
    }
    *outPtr = '\0';

    /* Insert the emulated tab */
    TextInsertAtCursor(outStr, true, true);
    delete[] outStr;

    /* Restore and ++ emTabsBeforeCursor cleared by TextInsertAtCursor */
    emTabsBeforeCursor_ = emTabsBeforeCursor + 1;

    buffer_->BufUnselect();
}

void NirvanaQt::verticalScrollBar_valueChanged(int value) {
    const int newValue = value + 1;
    const int lineDelta = newValue - topLineNum_;

    if (lineDelta == 0) {
        return;
    }

    setScroll(newValue, horizOffset_, false, true);
}

void NirvanaQt::horizontalScrollBar_valueChanged(int value) {
    Q_UNUSED(value);
#if 1
    const int newValue = value;

    if (newValue == horizOffset_) {
        return;
    }

    setScroll(topLineNum_, newValue, false, false);
#endif
}

void NirvanaQt::moveDestinationAP(QMouseEvent *event) {
    /* Move the cursor */
    TextDSetInsertPosition(TextDXYToPosition(event->x(), event->y()));
    checkAutoShowInsertPos();
    emitCursorMoved();
}

/*
** Translate window coordinates to the nearest text cursor position.
*/
int NirvanaQt::TextDXYToPosition(int x, int y) {
    return xyToPos(x, y, CURSOR_POS);
}

/*
** Translate window coordinates to the nearest (insert cursor or character
** cell) text position.  The parameter posType specifies how to interpret the
** position: CURSOR_POS means translate the coordinates to the nearest cursor
** position, and CHARACTER_POS means return the position of the character
** closest to (x, y).
*/
int NirvanaQt::xyToPos(int x, int y, PositionTypes posType) {
    int charIndex, lineStart, lineLen, fontHeight;
    int charWidth, charStyle, visLineNum, xStep, outIndex;
    char_type *lineStr, expandedChar[MAX_EXP_CHAR_LEN];

    /* Find the visible line number corresponding to the y coordinate */
    fontHeight = viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent();
    visLineNum = (y - top_) / fontHeight;
    if (visLineNum < 0)
        return firstChar_;
    if (visLineNum >= nVisibleLines_)
        visLineNum = nVisibleLines_ - 1;

    /* Find the position at the start of the line */
    lineStart = lineStarts_[visLineNum];

    /* If the line start was empty, return the last position in the buffer */
    if (lineStart == -1)
        return buffer_->BufGetLength();

    /* Get the line text and its length */
    lineLen = visLineLength(visLineNum);
    lineStr = buffer_->BufGetRange(lineStart, lineStart + lineLen);

    /* Step through character positions from the beginning of the line
       to find the character position corresponding to the x coordinate */
    xStep = left_ - horizOffset_;
    outIndex = 0;
    for (charIndex = 0; charIndex < lineLen; charIndex++) {
        int charLen = TextBuffer::BufExpandCharacter(lineStr[charIndex], outIndex, expandedChar,
                                                     buffer_->BufGetTabDistance(), buffer_->BufGetNullSubsChar());
        charStyle = styleOfPos(lineStart, lineLen, charIndex, outIndex, lineStr[charIndex]);
        charWidth = stringWidth(expandedChar, charLen, charStyle);
        if (x < xStep + (posType == CURSOR_POS ? charWidth / 2 : charWidth)) {
            delete[] lineStr;
            return lineStart + charIndex;
        }
        xStep += charWidth;
        outIndex += charLen;
    }

    /* If the x position was beyond the end of the line, return the position
       of the newline at the end of the line */
    delete[] lineStr;
    return lineStart + lineLen;
}

/*
** Correct a column number based on an unconstrained position (as returned by
** TextDXYToUnconstrainedPosition) to be relative to the last actual newline
** in the buffer before the row and column position given, rather than the
** last line start created by line wrapping.  This is an adapter
** for rectangular selections and code written before continuous wrap mode,
** which thinks that the unconstrained column is the number of characters
** from the last newline.  Obviously this is time consuming, because it
** invloves character re-counting.
*/
int NirvanaQt::TextDOffsetWrappedColumn(int row, int column) {
    int lineStart;
    int dispLineStart;

    if (!continuousWrap_ || row < 0 || row > nVisibleLines_) {
        return column;
    }

    dispLineStart = lineStarts_[row];

    if (dispLineStart == -1) {
        return column;
    }

    lineStart = buffer_->BufStartOfLine(dispLineStart);
    return column + buffer_->BufCountDispChars(lineStart, dispLineStart);
}

/*
** Reset drag state and cancel the auto-scroll timer
*/
void NirvanaQt::endDrag() {
#if 0
    if (autoScrollProcID_ != 0) {
        XtRemoveTimeOut(autoScrollProcID_);
    }

    autoScrollProcID_ = 0;

    if (dragState_ == MOUSE_PAN) {
        XUngrabPointer();
    }
#endif

    dragState_ = NOT_CLICKED;
}

void NirvanaQt::moveToAP(QMouseEvent *event) {
    int dragState = dragState_;
    Selection *secondary = &buffer_->BufGetSecondarySelection();
    Selection *primary = &buffer_->BufGetPrimarySelection();
    int insertPos;
    int rectangular = secondary->rectangular;
    int column;

    endDrag();
    if (!((dragState == SECONDARY_DRAG && secondary->selected) ||
          (dragState == SECONDARY_RECT_DRAG && secondary->selected) || dragState == SECONDARY_CLICKED ||
          dragState == NOT_CLICKED)) {
        return;
    }

    if (checkReadOnly()) {
        buffer_->BufSecondaryUnselect();
        return;
    }

    if (secondary->selected) {
        if (motifDestOwner_) {
            auto textToCopy = buffer_->BufGetSecSelectText();

            if (primary->selected && rectangular) {
                insertPos = TextDGetInsertPosition();
                buffer_->BufReplaceSelected(textToCopy);
                TextDSetInsertPosition(buffer_->BufGetCursorPosHint());
            } else if (rectangular) {
                insertPos = TextDGetInsertPosition();
                int lineStart = buffer_->BufStartOfLine(insertPos);
                column = buffer_->BufCountDispChars(lineStart, insertPos);
                buffer_->BufInsertCol(column, lineStart, textToCopy, nullptr, nullptr);
                TextDSetInsertPosition(buffer_->BufGetCursorPosHint());
            } else {
                TextInsertAtCursor(textToCopy, true, autoWrapPastedText_);
            }

            delete[] textToCopy;
            buffer_->BufRemoveSecSelect();
            buffer_->BufSecondaryUnselect();
        } else {
            SendSecondarySelection(true);
        }
    } else if (primary->selected) {
        auto textToCopy = buffer_->BufGetRange(primary->start, primary->end);
        TextDSetInsertPosition(TextDXYToPosition(event->x(), event->y()));
        TextInsertAtCursor(textToCopy, false, autoWrapPastedText_);
        delete[] textToCopy;
        buffer_->BufRemoveSelected();
        buffer_->BufUnselect();
    } else {
        TextDSetInsertPosition(TextDXYToPosition(event->x(), event->y()));
        MovePrimarySelection(PasteStandard);
    }
}

/*
** Complete a block text drag operation
*/
void NirvanaQt::FinishBlockDrag() {
#if 0
    dragEndCBStruct endStruct;
    int modRangeStart = -1, origModRangeEnd, bufModRangeEnd;
    char_type *deletedText;

    /* Find the changed region of the buffer, covering both the deletion
       of the selected text at the drag start position, and insertion at
       the drag destination */
    trackModifyRange(&modRangeStart, &bufModRangeEnd, &origModRangeEnd,
                tw->text.dragSourceDeletePos, tw->text.dragSourceInserted,
                tw->text.dragSourceDeleted);
    trackModifyRange(&modRangeStart, &bufModRangeEnd, &origModRangeEnd,
                tw->text.dragInsertPos, tw->text.dragInserted,
                tw->text.dragDeleted);

    /* Get the original (pre-modified) range of text from saved backup buffer */
    deletedText = BufGetRange(tw->text.dragOrigBuf, modRangeStart,
            origModRangeEnd);

    /* Free the backup buffer */
    BufFree(tw->text.dragOrigBuf);

    /* Return to normal drag state */
    tw->text.dragState = NOT_CLICKED;

    /* Call finish-drag calback */
    endStruct.startPos = modRangeStart;
    endStruct.nCharsDeleted = origModRangeEnd - modRangeStart;
    endStruct.nCharsInserted = bufModRangeEnd - modRangeStart;
    endStruct.deletedText = deletedText;
    XtCallCallbacks((Widget)tw, textNdragEndCallback, (XtPointer)&endStruct);
    delete [] deletedText;
#endif
}

/*
** Insert the contents of the PRIMARY selection at the cursor position in
** widget "w" and delete the contents of the selection in its current owner
** (if the selection owner supports DELETE targets).
*/
void NirvanaQt::MovePrimarySelection(PasteMode pasteMode) {
    Q_UNUSED(pasteMode);
#if 0
   static Atom targets[2] = {XA_STRING};
   static int isColFlag;
   static XtPointer clientData[2] = {
       (XtPointer)&isColFlag,
       (XtPointer)&isColFlag
   };

   targets[1] = getAtom(XtDisplay(w), A_DELETE);
   isColFlag = isColumnar;
   /* some strangeness here: the selection callback appears to be getting
      clientData[1] for targets[0] */
   XtGetSelectionValues(w, XA_PRIMARY, targets, 2, getSelectionCB, clientData, time);
#endif
}

/*
** Insert the secondary selection at the motif destination by initiating
** an INSERT_SELECTION request to the current owner of the MOTIF_DESTINATION
** selection.  Upon completion, unselect the secondary selection.  If
** "removeAfter" is true, also delete the secondary selection from the
** widget's buffer upon completion.
*/
void NirvanaQt::SendSecondarySelection(bool removeAfter) {
    Q_UNUSED(removeAfter);
#if 0
    sendSecondary(w, time, getAtom(XtDisplay(w), A_MOTIF_DESTINATION), removeAfter ? REMOVE_SECONDARY : UNSELECT_SECONDARY, nullptr, 0);
#endif
}

/*
** Select the word or whitespace adjacent to the cursor, and move the cursor
** to its end.  pointerX is used as a tie-breaker, when the cursor is at the
** boundary between a word and some white-space.  If the cursor is on the
** left, the word or space on the left is used.  If it's on the right, that
** is used instead.
*/
void NirvanaQt::selectWord(int pointerX) {
    int x;
    int y;
    int insertPos = TextDGetInsertPosition();

    TextPosToXY(insertPos, &x, &y);
    if (pointerX < x && insertPos > 0 && buffer_->BufGetCharacter(insertPos - 1) != '\n') {
        insertPos--;
    }

    buffer_->BufSelect(startOfWord(insertPos), endOfWord(insertPos));
}

/*
** Translate a buffer text position to the XY location where the center
** of the cursor would be positioned to point to that character.  Returns
** false if the position is not displayed because it is VERTICALLY out
** of view.  If the position is horizontally out of view, returns the
** x coordinate where the position would be if it were visible.
*/
int NirvanaQt::TextPosToXY(int pos, int *x, int *y) {
    return TextDPositionToXY(pos, x, y);
}

/*
** Select the line containing the cursor, including the terminating newline,
** and move the cursor to its end.
*/
void NirvanaQt::selectLine() {

    const int insertPos = TextDGetInsertPosition();
    const int endPos = buffer_->BufEndOfLine(insertPos);
    const int startPos = buffer_->BufStartOfLine(insertPos);

    buffer_->BufSelect(startPos, qMin(endPos + 1, buffer_->BufGetLength()));
    TextDSetInsertPosition(endPos);
}

void NirvanaQt::extendAdjustAP(QMouseEvent *event) {
    int dragState = dragState_;
    bool rectDrag = /* hasKey("rect", args, nArgs); */ (event->modifiers() & Qt::ControlModifier);

    /* Make sure the proper initialization was done on mouse down */
    if (dragState != PRIMARY_DRAG && dragState != PRIMARY_CLICKED && dragState != PRIMARY_RECT_DRAG) {
        return;
    }

    /* If the selection hasn't begun, decide whether the mouse has moved
       far enough from the initial mouse down to be considered a drag */
    if (dragState_ == PRIMARY_CLICKED) {
        if (abs(event->x() - btnDownX_) > SelectThreshold || abs(event->y() - btnDownY_) > SelectThreshold) {
            dragState_ = rectDrag ? PRIMARY_RECT_DRAG : PRIMARY_DRAG;
        } else {
            return;
        }
    }

    /* If "rect" argument has appeared or disappeared, keep dragState up
       to date about which type of drag this is */
    dragState_ = rectDrag ? PRIMARY_RECT_DRAG : PRIMARY_DRAG;

    /* Record the new position for the autoscrolling timer routine, and
       engage or disengage the timer if the mouse is in/out of the window */
    checkAutoScroll(event->x(), event->y());

    /* Adjust the selection and move the cursor */
    adjustSelection(event->x(), event->y());
}

/*
** Adjust the selection as the mouse is dragged to position: (x, y).
*/
void NirvanaQt::adjustSelection(int x, int y) {

    int newPos = TextDXYToPosition(x, y);

    /* Adjust the selection */
    if (dragState_ == PRIMARY_RECT_DRAG) {
        int row;
        int col;
        TextDXYToUnconstrainedPosition(x, y, &row, &col);
        col = TextDOffsetWrappedColumn(row, col);
        const int startCol = qMin(rectAnchor_, col);
        const int endCol = qMax(rectAnchor_, col);
        const int startPos = buffer_->BufStartOfLine(qMin(anchor_, newPos));
        const int endPos = buffer_->BufEndOfLine(qMax(anchor_, newPos));
        buffer_->BufRectSelect(startPos, endPos, startCol, endCol);
    } else if (clickCount_ == 1) {
        const int startPos = startOfWord(qMin(anchor_, newPos));
        const int endPos = endOfWord(qMax(anchor_, newPos));
        buffer_->BufSelect(startPos, endPos);
        newPos = newPos < anchor_ ? startPos : endPos;
    } else if (clickCount_ == 2) {
        const int startPos = buffer_->BufStartOfLine(qMin(anchor_, newPos));
        const int endPos = buffer_->BufEndOfLine(qMax(anchor_, newPos));
        buffer_->BufSelect(startPos, qMin(endPos + 1, buffer_->BufGetLength()));
        newPos = (newPos < anchor_) ? startPos : endPos;
    } else {
        buffer_->BufSelect(anchor_, newPos);
    }

    /* Move the cursor */
    TextDSetInsertPosition(newPos);
    emitCursorMoved();
}

void NirvanaQt::adjustSecondarySelection(int x, int y) {
    int newPos = TextDXYToPosition(x, y);

    if (dragState_ == SECONDARY_RECT_DRAG) {

        int row;
        int col;

        TextDXYToUnconstrainedPosition(x, y, &row, &col);

        col = TextDOffsetWrappedColumn(row, col);
        const int startCol = qMin(rectAnchor_, col);
        const int endCol = qMax(rectAnchor_, col);
        const int startPos = buffer_->BufStartOfLine(qMin(anchor_, newPos));
        const int endPos = buffer_->BufEndOfLine(qMax(anchor_, newPos));

        buffer_->BufSecRectSelect(startPos, endPos, startCol, endCol);
    } else {
        buffer_->BufSecondarySelect(anchor_, newPos);
    }
}

/*
** Given a new mouse pointer location, pass the position on to the
** autoscroll timer routine, and make sure the timer is on when it's
** needed and off when it's not.
*/
void NirvanaQt::checkAutoScroll(int x, int y) {
    /* Is the pointer in or out of the window? */
    const bool inWindow = viewport()->rect().contains(x, y);

    /* If it's in the window, cancel the timer procedure */
    if (inWindow) {
        autoScrollTimer_->stop();
        return;
    }

    /* If the timer is not already started, start it */
    autoScrollTimer_->start(0);

    /* Pass on the newest mouse location to the autoscroll routine */
    mouseX_ = x;
    mouseY_ = y;
}

void NirvanaQt::nextPageAP(MoveMode mode) {
    int lastTopLine = qMax(1, nBufferLines_ - (nVisibleLines_ - 2) + cursorVPadding_);
    int insertPos = TextDGetInsertPosition();
    int column = 0, visLineNum, lineStartPos;
    int pos, targetLine;
    int pageForwardCount = qMax(1, nVisibleLines_ - 1);
    int maintainColumn = 0;
    int silent = /* hasKey("nobell", args, nArgs); */ false;

    maintainColumn = /* hasKey("column", args, nArgs); */ false;
    cancelDrag();
    if (/* hasKey("scrollbar", args, nArgs) */ false) { /* scrollbar only */
        targetLine = qMin(topLineNum_ + pageForwardCount, lastTopLine);

        if (targetLine == topLineNum_) {
            ringIfNecessary(silent);
            return;
        }
        TextDSetScroll(targetLine, horizOffset_);
    } else if (/* hasKey("stutter", args, nArgs) */ false) { /* Mac style */
        /* move to bottom line of visible area */
        /* if already there, page down maintaining preferrred column */
        targetLine = qMax(qMin(nVisibleLines_ - 1, nBufferLines_), 0);
        column = TextDPreferredColumn(&visLineNum, &lineStartPos);
        if (lineStartPos == lineStarts_[targetLine]) {
            if (insertPos >= buffer_->BufGetLength() || topLineNum_ == lastTopLine) {
                ringIfNecessary(silent);
                return;
            }
            targetLine = qMin(topLineNum_ + pageForwardCount, lastTopLine);
            pos = TextDCountForwardNLines(insertPos, pageForwardCount, false);
            if (maintainColumn) {
                pos = TextDPosOfPreferredCol(column, pos);
            }
            TextDSetInsertPosition(pos);
            TextDSetScroll(targetLine, horizOffset_);
        } else {
            pos = lineStarts_[targetLine];
            while (targetLine > 0 && pos == -1) {
                --targetLine;
                pos = lineStarts_[targetLine];
            }
            if (lineStartPos == pos) {
                ringIfNecessary(silent);
                return;
            }
            if (maintainColumn) {
                pos = TextDPosOfPreferredCol(column, pos);
            }
            TextDSetInsertPosition(pos);
        }
        checkMoveSelectionChange(insertPos, mode);
        checkAutoShowInsertPos();
        emitCursorMoved();
        if (maintainColumn) {
            cursorPreferredCol_ = column;
        } else {
            cursorPreferredCol_ = -1;
        }
    } else { /* "standard" */
        if (insertPos >= buffer_->BufGetLength() && topLineNum_ == lastTopLine) {
            ringIfNecessary(silent);
            return;
        }
        if (maintainColumn) {
            column = TextDPreferredColumn(&visLineNum, &lineStartPos);
        }
        targetLine = topLineNum_ + nVisibleLines_ - 1;
        if (targetLine < 1)
            targetLine = 1;
        if (targetLine > lastTopLine)
            targetLine = lastTopLine;
        pos = TextDCountForwardNLines(insertPos, nVisibleLines_ - 1, false);
        if (maintainColumn) {
            pos = TextDPosOfPreferredCol(column, pos);
        }
        TextDSetInsertPosition(pos);
        TextDSetScroll(targetLine, horizOffset_);
        checkMoveSelectionChange(insertPos, mode);
        checkAutoShowInsertPos();
        emitCursorMoved();
        if (maintainColumn) {
            cursorPreferredCol_ = column;
        } else {
            cursorPreferredCol_ = -1;
        }
    }
}

void NirvanaQt::previousPageAP(MoveMode mode) {

    int insertPos = TextDGetInsertPosition();
    int column = 0, visLineNum, lineStartPos;
    int pos, targetLine;
    int pageBackwardCount = qMax(1, nVisibleLines_ - 1);
    int maintainColumn = 0;
    int silent = /* hasKey("nobell", args, nArgs); */ false;

    maintainColumn = /* hasKey("column", args, nArgs); */ false;
    cancelDrag();
    if (/* hasKey("scrollbar", args, nArgs) */ false) { /* scrollbar only */
        targetLine = qMax(topLineNum_ - pageBackwardCount, 1);

        if (targetLine == topLineNum_) {
            ringIfNecessary(silent);
            return;
        }
        TextDSetScroll(targetLine, horizOffset_);
    } else if (/* hasKey("stutter", args, nArgs) */ false) { /* Mac style */
        /* move to top line of visible area */
        /* if already there, page up maintaining preferrred column if required */
        targetLine = 0;
        column = TextDPreferredColumn(&visLineNum, &lineStartPos);
        if (lineStartPos == lineStarts_[targetLine]) {
            if (topLineNum_ == 1 && (maintainColumn || column == 0)) {
                ringIfNecessary(silent);
                return;
            }
            targetLine = qMax(topLineNum_ - pageBackwardCount, 1);
            pos = TextDCountBackwardNLines(insertPos, pageBackwardCount);
            if (maintainColumn) {
                pos = TextDPosOfPreferredCol(column, pos);
            }
            TextDSetInsertPosition(pos);
            TextDSetScroll(targetLine, horizOffset_);
        } else {
            pos = lineStarts_[targetLine];
            if (maintainColumn) {
                pos = TextDPosOfPreferredCol(column, pos);
            }
            TextDSetInsertPosition(pos);
        }
        checkMoveSelectionChange(insertPos, mode);
        checkAutoShowInsertPos();
        emitCursorMoved();
        if (maintainColumn) {
            cursorPreferredCol_ = column;
        } else {
            cursorPreferredCol_ = -1;
        }
    } else { /* "standard" */
        if (insertPos <= 0 && topLineNum_ == 1) {
            ringIfNecessary(silent);
            return;
        }
        if (maintainColumn) {
            column = TextDPreferredColumn(&visLineNum, &lineStartPos);
        }
        targetLine = topLineNum_ - (nVisibleLines_ - 1);
        if (targetLine < 1)
            targetLine = 1;
        pos = TextDCountBackwardNLines(insertPos, nVisibleLines_ - 1);
        if (maintainColumn) {
            pos = TextDPosOfPreferredCol(column, pos);
        }
        TextDSetInsertPosition(pos);
        TextDSetScroll(targetLine, horizOffset_);
        checkMoveSelectionChange(insertPos, mode);
        checkAutoShowInsertPos();
        emitCursorMoved();
        if (maintainColumn) {
            cursorPreferredCol_ = column;
        } else {
            cursorPreferredCol_ = -1;
        }
    }
}

/*
** Return the current preferred column along with the current
** visible line index (-1 if not visible) and the lineStartPos
** of the current insert position.
*/
int NirvanaQt::TextDPreferredColumn(int *visLineNum, int *lineStartPos) {
    int column;

    /* Find the position of the start of the line.  Use the line starts array
    if possible, to avoid unbounded line-counting in continuous wrap mode */
    if (posToVisibleLineNum(cursorPos_, visLineNum)) {
        *lineStartPos = lineStarts_[*visLineNum];
    } else {
        *lineStartPos = TextDStartOfLine(cursorPos_);
        *visLineNum = -1;
    }

    /* Decide what column to move to, if there's a preferred column use that */
    column = (cursorPreferredCol_ >= 0) ? cursorPreferredCol_ : buffer_->BufCountDispChars(*lineStartPos, cursorPos_);
    return (column);
}

/*
** Return the insert position of the requested column given
** the lineStartPos.
*/
int NirvanaQt::TextDPosOfPreferredCol(int column, int lineStartPos) {
    int newPos = buffer_->BufCountForwardDispChars(lineStartPos, column);
    if (continuousWrap_) {
        newPos = qMin(newPos, TextDEndOfLine(lineStartPos, true));
    }
    return newPos;
}

/*
** timer procedure for autoscrolling
*/
void NirvanaQt::autoScrollTimeout() {

    int cursorX;
    int y;
    const int fontWidth = fixedFontWidth_;
    const int fontHeight = viewport()->fontMetrics().ascent() + viewport()->fontMetrics().descent();

    /* For vertical autoscrolling just dragging the mouse outside of the top
       or bottom of the window is sufficient, for horizontal (non-rectangular)
       scrolling, see if the position where the CURSOR would go is outside */
    int newPos = TextDXYToPosition(mouseX_, mouseY_);
    if (dragState_ == PRIMARY_RECT_DRAG) {
        cursorX = mouseX_;
    } else if (!TextDPositionToXY(newPos, &cursorX, &y)) {
        cursorX = mouseX_;
    }

    /* Scroll away from the pointer, 1 character (horizontal), or 1 character
       for each fontHeight distance from the mouse to the text (vertical) */
    int topLineNum;
    int horizOffset;
    TextDGetScroll(&topLineNum, &horizOffset);

    if (cursorX >= viewport()->width()) {
        horizOffset += fontWidth;
    } else if (mouseX_ < left_) {
        horizOffset -= fontWidth;
    }

    if (mouseY_ >= viewport()->height()) {
        topLineNum += 1 + ((mouseY_ - (int)viewport()->height()) / fontHeight) + 1;
    } else if (mouseY_ < top_) {
        topLineNum -= 1 + ((top_ - mouseY_) / fontHeight);
    }

    TextDSetScroll(topLineNum, horizOffset);

    /* Continue the drag operation in progress.  If none is in progress
     *(safety check) don't continue to re-establish the timer proc */
    if (dragState_ == PRIMARY_DRAG) {
        adjustSelection(mouseX_, mouseY_);
    } else if (dragState_ == PRIMARY_RECT_DRAG) {
        adjustSelection(mouseX_, mouseY_);
    } else if (dragState_ == SECONDARY_DRAG) {
        adjustSecondarySelection(mouseX_, mouseY_);
    } else if (dragState_ == SECONDARY_RECT_DRAG) {
        adjustSecondarySelection(mouseX_, mouseY_);
    } else if (dragState_ == PRIMARY_BLOCK_DRAG) {
#if 0
        BlockDragSelection(mouseX_, mouseY_, USE_LAST);
#endif
    } else {
        // the timer is single shot, so it will stop if we don't start it again
        return;
    }

    /* re-establish the timer proc (this routine) to continue processing */
    autoScrollTimer_->start();
}

/*
** Get the current scroll position for the text display, in terms of line
** number of the top line and horizontal pixel offset from the left margin
*/
void NirvanaQt::TextDGetScroll(int *topLineNum, int *horizOffset) {
    *topLineNum = topLineNum_;
    *horizOffset = horizOffset_;
}

/*
** This is a stripped-down version of the findWrapRange() function above,
** intended to be used to calculate the number of "deleted" lines during
** a buffer modification. It is called _before_ the modification takes place.
**
** This function should only be called in continuous wrap mode with a
** non-fixed font width. In that case, it is impossible to calculate
** the number of deleted lines, because the necessary style information
** is no longer available _after_ the modification. In other cases, we
** can still perform the calculation afterwards (possibly even more
** efficiently).
*/
void NirvanaQt::measureDeletedLines(int pos, int nDeleted) {
    int retPos;
    int retLines;
    int retLineStart;
    int retLineEnd;

    int nVisLines = nVisibleLines_;
    int *lineStarts = lineStarts_.data();
    int countFrom;
    int lineStart;
    int nLines = 0;

    /*
    ** Determine where to begin searching: either the previous newline, or
    ** if possible, limit to the start of the (original) previous displayed
    ** line, using information from the existing line starts array
    */
    if (pos >= firstChar_ && pos <= lastChar_) {
        int i;
        for (i = nVisLines - 1; i > 0; i--)
            if (lineStarts[i] != -1 && pos >= lineStarts[i])
                break;
        if (i > 0) {
            countFrom = lineStarts[i - 1];
        } else
            countFrom = buffer_->BufStartOfLine(pos);
    } else
        countFrom = buffer_->BufStartOfLine(pos);

    /*
    ** Move forward through the (new) text one line at a time, counting
    ** displayed lines, and looking for either a real newline, or for the
    ** line starts to re-sync with the original line starts array
    */
    lineStart = countFrom;
    while (true) {
        /* advance to the next line.  If the line ended in a real newline
           or the end of the buffer, that's far enough */
        wrappedLineCounter(buffer_, lineStart, buffer_->BufGetLength(), 1, true, 0, &retPos, &retLines, &retLineStart,
                           &retLineEnd);
        if (retPos >= buffer_->BufGetLength()) {
            if (retPos != retLineEnd)
                nLines++;
            break;
        } else
            lineStart = retPos;
        nLines++;
        if (lineStart > pos + nDeleted && buffer_->BufGetCharacter(lineStart - 1) == '\n') {
            break;
        }

        /* Unlike in the findWrapRange() function above, we don't try to
           resync with the line starts, because we don't know the length
           of the inserted text yet, nor the updated style information.

           Because of that, we also shouldn't resync with the line starts
           after the modification either, because we must perform the
           calculations for the deleted and inserted lines in the same way.

           This can result in some unnecessary recalculation and redrawing
           overhead, and therefore we should only use this two-phase mode
           of calculation when it's really needed (continuous wrap + variable
           font width). */
    }
    nLinesDeleted_ = nLines;
    suppressResync_ = true;
}

void NirvanaQt::forwardParagraphAP(MoveMode mode) {
    int pos, insertPos = TextDGetInsertPosition();
    char_type c;
    static const char_type whiteChars[] = " \t";
    int silent = /* hasKey("nobell", args, nArgs); */ false;

    cancelDrag();
    if (insertPos == buffer_->BufGetLength()) {
        ringIfNecessary(silent);
        return;
    }
    pos = qMin(buffer_->BufEndOfLine(insertPos) + 1, buffer_->BufGetLength());
    while (pos < buffer_->BufGetLength()) {
        c = buffer_->BufGetCharacter(pos);
        if (c == '\n')
            break;
        if (strchr(whiteChars, c) != nullptr)
            pos++;
        else
            pos = qMin(buffer_->BufEndOfLine(pos) + 1, buffer_->BufGetLength());
    }
    TextDSetInsertPosition(qMin(pos + 1, buffer_->BufGetLength()));
    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::backwardParagraphAP(MoveMode mode) {
    int parStart, pos, insertPos = TextDGetInsertPosition();
    char_type c;
    static const char_type whiteChars[] = " \t";
    int silent = /* hasKey("nobell", args, nArgs); */ false;

    cancelDrag();
    if (insertPos == 0) {
        ringIfNecessary(silent);
        return;
    }
    parStart = buffer_->BufStartOfLine(qMax(insertPos - 1, 0));
    pos = qMax(parStart - 2, 0);
    while (pos > 0) {
        c = buffer_->BufGetCharacter(pos);
        if (c == '\n')
            break;
        if (strchr(whiteChars, c) != nullptr)
            pos--;
        else {
            parStart = buffer_->BufStartOfLine(pos);
            pos = qMax(parStart - 2, 0);
        }
    }
    TextDSetInsertPosition(parStart);
    checkMoveSelectionChange(insertPos, mode);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::emitCursorMoved() {
    for (ICursorMoveHandler *handler : cursorMoveHandlers_) {
        handler->cursorMoved();
    }
}

void NirvanaQt::emitUnfinishedHighlightEncountered(int pos) {

    HighlightEvent event;
    event.buffer = buffer_;
    event.pos = pos;

    for (IHighlightHandler *handler : highlightHandlers_) {
        handler->unfinishedHighlightEncountered(&event);
    }
}

/*
** Cancel a block drag operation
*/
void NirvanaQt::CancelBlockDrag() {
#if 0
    TextBuffer *buf = buffer_;
    TextBuffer *origBuf = dragOrigBuf_;
    Selection *origSel = origBuf->BufGetPrimarySelection();
    int modRangeStart = -1, origModRangeEnd, bufModRangeEnd;
    dragEndCBStruct endStruct;

    /* If the operation was a move, make the modify range reflect the
       removal of the text from the starting position */
    if (dragSourceDeleted_ != 0)
        trackModifyRange(
                    &modRangeStart,
                    &bufModRangeEnd,
                    &origModRangeEnd,
                    dragSourceDeletePos_,
                    dragSourceInserted_,
                    dragSourceDeleted_);

    /* Include the insert being undone from the last step in the modified
       range. */
    trackModifyRange(
                &modRangeStart,
                &bufModRangeEnd,
                &origModRangeEnd,
                dragInsertPos_,
                dragInserted_,
                dragDeleted_);

    /* Make the changes in the buffer */
    char_type *repText = origBuf->BufGetRange(modRangeStart, origModRangeEnd);
    buf->BufReplace(modRangeStart, bufModRangeEnd, repText);
    delete [] repText;

    /* Reset the selection and cursor position */
    if (origSel->rectangular)
        buf->BufRectSelect(origSel->start, origSel->end, origSel->rectStart, origSel->rectEnd);
    else
        buf->BufSelect(origSel->start, origSel->end);
    TextDSetInsertPosition(buf->BufGetCursorPosHint());
    XtCallCallbacks(textNcursorMovementCallback, nullptr);
    emTabsBeforeCursor_ = 0;

    /* Free the backup buffer */
    BufFree(origBuf);

    /* Indicate end of drag */
    dragState_ = DRAG_CANCELED;

    /* Call finish-drag calback */
    endStruct.startPos       = 0;
    endStruct.nCharsDeleted  = 0;
    endStruct.nCharsInserted = 0;
    endStruct.deletedText    = nullptr;
    XtCallCallbacks(textNdragEndCallback, (XtPointer)&endStruct);
#endif
}

/*
** Shift the selection left or right by a single character, or by one tab stop
** if "byTab" is true.  (The length of a tab stop is the size of an emulated
** tab if emulated tabs are turned on, or a hardware tab if not).
*/
void NirvanaQt::ShiftSelection(ShiftDirection direction, bool byTab) {
    int selStart, selEnd;
    bool isRect;
    int rectStart, rectEnd;
    int shiftedLen, newEndPos, cursorPos, origLength, shiftDist;
    char_type *text, *shiftedText;
    TextBuffer *buf = buffer_;

    /* get selection, if no text selected, use current insert position */
    if (!buf->BufGetSelectionPos(&selStart, &selEnd, &isRect, &rectStart, &rectEnd)) {
        cursorPos = TextGetCursorPos();
        selStart = buf->BufStartOfLine(cursorPos);
        selEnd = buf->BufEndOfLine(cursorPos);
        if (selEnd < buf->BufGetLength())
            selEnd++;
        buf->BufSelect(selStart, selEnd);
        isRect = false;
        text = buf->BufGetRange(selStart, selEnd);
    } else if (isRect) {
        cursorPos = TextGetCursorPos();
        origLength = buf->BufGetLength();
        shiftRect(direction, byTab, selStart, selEnd, rectStart, rectEnd);
        TextSetCursorPos((cursorPos < (selEnd + selStart) / 2) ? selStart
                                                               : cursorPos + (buf->BufGetLength() - origLength));
        return;
    } else {
        selStart = buf->BufStartOfLine(selStart);
        if (selEnd != 0 && buf->BufGetCharacter(selEnd - 1) != '\n') {
            selEnd = buf->BufEndOfLine(selEnd);
            if (selEnd < buf->BufGetLength())
                selEnd++;
        }
        buf->BufSelect(selStart, selEnd);
        text = buf->BufGetRange(selStart, selEnd);
    }

    /* shift the text by the appropriate distance */
    if (byTab) {
        shiftDist = emulateTabs_ == 0 ? buf->BufGetTabDistance() : emulateTabs_;
    } else {
        shiftDist = 1;
    }

    shiftedText = ShiftText(text, direction, buf->BufGetUseTabs(), buf->BufGetTabDistance(), shiftDist, &shiftedLen);
    delete[] text;
    buf->BufReplaceSelected(shiftedText);
    delete[] shiftedText;

    newEndPos = selStart + shiftedLen;
    buf->BufSelect(selStart, newEndPos);
}

/*
** Return the cursor position
*/
int NirvanaQt::TextGetCursorPos() {
    return TextDGetInsertPosition();
}

/*
** Set the cursor position
*/
void NirvanaQt::TextSetCursorPos(int pos) {
    TextDSetInsertPosition(pos);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

/*
** shift lines left and right in a multi-line text string.  Returns the
** shifted text in memory that must be freed by the caller with delete[].
*/
char_type *NirvanaQt::ShiftText(char_type *text, ShiftDirection direction, bool tabsAllowed, int tabDist, int nChars, int *newLen) {
    size_t bufLen;

    /*
    ** Allocate memory for shifted string.  Shift left adds a maximum of
    ** tabDist-2 characters per line (remove one tab, add tabDist-1 spaces).
    ** Shift right adds a maximum of nChars character per line.
    */
    if (direction == SHIFT_RIGHT) {
        bufLen = traits_type::length(text) + (countLines(text) + 1) * nChars;
    } else {
        bufLen = traits_type::length(text) + (countLines(text) + 1) * tabDist;
    }

    auto shiftedText = new char_type[bufLen + 1]();

    /*
    ** break into lines and call shiftLine(Left/Right) on each
    */
    const char_type *lineStartPtr = text;
    const char_type *textPtr = text;
    char_type *shiftedPtr = shiftedText;

    while (true) {
        if (*textPtr == '\n' || *textPtr == '\0') {

            const char_type *const shiftedLine = (direction == SHIFT_RIGHT)
                    ? shiftLineRight(lineStartPtr, textPtr - lineStartPtr, tabsAllowed, tabDist, nChars)
                    : shiftLineLeft (lineStartPtr, textPtr - lineStartPtr,              tabDist, nChars);


			const size_t shiftedLineLen = traits_type::length(shiftedLine);
			
			std::copy_n(shiftedLine, shiftedLineLen, shiftedPtr);
			shiftedPtr[shiftedLineLen] = _T('\0');
            shiftedPtr += shiftedLineLen;
            delete[] shiftedLine;

            if (*textPtr == '\0') {
                /* terminate string & exit loop at end of text */
                *shiftedPtr = '\0';
                break;
            } else {
                /* move the newline from text to shifted text */
                *shiftedPtr++ = *textPtr++;
            }
            /* start line over */
            lineStartPtr = textPtr;
        } else {
            textPtr++;
        }
    }
    *newLen = shiftedPtr - shiftedText;
    return shiftedText;
}

char_type *NirvanaQt::shiftLineRight(const char_type *line, int lineLen, bool tabsAllowed, int tabDist, int nChars) {

    int whiteWidth;
    int i;

    const char_type *lineInPtr = line;
    auto lineOut = new char_type[lineLen + nChars + 1];
    char_type *lineOutPtr = lineOut;
    whiteWidth = 0;
    while (true) {
        if (*lineInPtr == '\0' || (lineInPtr - line) >= lineLen) {
            /* nothing on line, wipe it out */
            *lineOut = '\0';
            return lineOut;
        } else if (*lineInPtr == ' ') {
            /* white space continues with tab, advance to next tab stop */
            whiteWidth++;
            *lineOutPtr++ = *lineInPtr++;
        } else if (*lineInPtr == '\t') {
            /* white space continues with tab, advance to next tab stop */
            whiteWidth = nextTab(whiteWidth, tabDist);
            *lineOutPtr++ = *lineInPtr++;
        } else {
            /* end of white space, add nChars of space */
            for (i = 0; i < nChars; i++) {
                *lineOutPtr++ = ' ';
                whiteWidth++;
                /* if we're now at a tab stop, change last 8 spaces to a tab */
                if (tabsAllowed && atTabStop(whiteWidth, tabDist)) {
                    lineOutPtr -= tabDist;
                    *lineOutPtr++ = '\t';
                }
            }
            /* move remainder of line */
            while (*lineInPtr != '\0' && (lineInPtr - line) < lineLen) {
                *lineOutPtr++ = *lineInPtr++;
            }

            *lineOutPtr = '\0';
            return lineOut;
        }
    }
}

char_type *NirvanaQt::shiftLineLeft(const char_type *line, int lineLen, int tabDist, int nChars) {
    int i;
    int whiteWidth;
    int lastWhiteWidth;
    int whiteGoal;

    const char_type *lineInPtr = line;
    auto lineOut = new char_type[lineLen + tabDist + 1];
    char_type *lineOutPtr = lineOut;
    whiteWidth = 0;
    lastWhiteWidth = 0;
    while (true) {
        if (*lineInPtr == '\0' || (lineInPtr - line) >= lineLen) {
            /* nothing on line, wipe it out */
            *lineOut = '\0';
            return lineOut;
        } else if (*lineInPtr == ' ') {
            /* white space continues with space, advance one character */
            whiteWidth++;
            *lineOutPtr++ = *lineInPtr++;
        } else if (*lineInPtr == '\t') {
            /* white space continues with tab, advance to next tab stop	    */
            /* save the position, though, in case we need to remove the tab */
            lastWhiteWidth = whiteWidth;
            whiteWidth = nextTab(whiteWidth, tabDist);
            *lineOutPtr++ = *lineInPtr++;
        } else {
            /* end of white space, remove nChars characters */
            for (i = 1; i <= nChars; i++) {
                if (lineOutPtr > lineOut) {
                    if (*(lineOutPtr - 1) == ' ') {
                        /* end of white space is a space, just remove it */
                        lineOutPtr--;
                    } else {
                        /* end of white space is a tab, remove it and add
                           back spaces */
                        lineOutPtr--;
                        whiteGoal = whiteWidth - i;
                        whiteWidth = lastWhiteWidth;
                        while (whiteWidth < whiteGoal) {
                            *lineOutPtr++ = ' ';
                            whiteWidth++;
                        }
                    }
                }
            }
            /* move remainder of line */
            while (*lineInPtr != '\0' && (lineInPtr - line) < lineLen)
                *lineOutPtr++ = *lineInPtr++;
            /* add a null */
            *lineOutPtr = '\0';
            return lineOut;
        }
    }
}

int NirvanaQt::nextTab(int pos, int tabDist) {
    return (pos / tabDist) * tabDist + tabDist;
}

int NirvanaQt::atTabStop(int pos, int tabDist) {
    return (pos % tabDist == 0);
}

void NirvanaQt::shiftRect(ShiftDirection direction, bool byTab, int selStart, int selEnd, int rectStart, int rectEnd) {
    int offset;
    TextBuffer *buf = buffer_;
    char_type *text;

    /* Make sure selStart and SelEnd refer to whole lines */
    selStart = buf->BufStartOfLine(selStart);
    selEnd = buf->BufEndOfLine(selEnd);

    /* Calculate the the left/right offset for the new rectangle */
    if (byTab) {
        offset = emulateTabs_ == 0 ? buf->BufGetTabDistance() : emulateTabs_;
    } else
        offset = 1;
    offset *= direction == SHIFT_LEFT ? -1 : 1;
    if (rectStart + offset < 0)
        offset = -rectStart;

    /* Create a temporary buffer for the lines containing the selection, to
       hide the intermediate steps from the display update routines */
    TextBuffer tempBuf;
    tempBuf.BufSetTabDistance(buf->BufGetTabDistance());
    tempBuf.BufSetUseTabs(buf->BufGetUseTabs());
    text = buf->BufGetRange(selStart, selEnd);
    tempBuf.BufSetAll(text);
    delete[] text;

    /* Do the shift in the temporary buffer */
    text = buf->BufGetTextInRect(selStart, selEnd, rectStart, rectEnd);
    tempBuf.BufRemoveRect(0, selEnd - selStart, rectStart, rectEnd);
    tempBuf.BufInsertCol(rectStart + offset, 0, text, nullptr, nullptr);
    delete[] text;

    /* Make the change in the real buffer */
    buf->BufReplace(selStart, selEnd, tempBuf.BufAsString());
    buf->BufRectSelect(selStart, selStart + tempBuf.BufGetLength(), rectStart + offset, rectEnd + offset);
}

void NirvanaQt::deleteToEndOfLineAP() {
    int insertPos = TextDGetInsertPosition();
    int endOfLine;

    if (/*hasKey("absolute", args, nArgs)*/ false)
        endOfLine = buffer_->BufEndOfLine(insertPos);
    else
        endOfLine = TextDEndOfLine(insertPos, false);
    cancelDrag();
    if (checkReadOnly())
        return;
    TakeMotifDestination();
    if (deletePendingSelection())
        return;
    if (insertPos == endOfLine) {
        bool silent = /* silent = hasKey("nobell", args, nArgs); */ false;
        ringIfNecessary(silent);
        return;
    }
    buffer_->BufRemove(insertPos, endOfLine);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::deleteToStartOfLineAP() {
    int insertPos = TextDGetInsertPosition();
    int startOfLine;

    if (/*hasKey("wrap", args, nArgs)*/ false)
        startOfLine = TextDStartOfLine(insertPos);
    else
        startOfLine = buffer_->BufStartOfLine(insertPos);
    cancelDrag();
    if (checkReadOnly())
        return;
    TakeMotifDestination();
    if (deletePendingSelection())
        return;
    if (insertPos == startOfLine) {
        bool silent = /* silent = hasKey("nobell", args, nArgs); */ false;
        ringIfNecessary(silent);
        return;
    }
    buffer_->BufRemove(startOfLine, insertPos);
    checkAutoShowInsertPos();
    emitCursorMoved();
}

void NirvanaQt::deselectAllAP() {
    cancelDrag();
    buffer_->BufUnselect();
}

void NirvanaQt::gotoMatching() {
    GotoMatchingCharacter();
}

void NirvanaQt::selectToMatching() {
    SelectToMatchingCharacter();
}

void NirvanaQt::GotoMatchingCharacter() {
    int selStart, selEnd;
    int matchPos;
    TextBuffer *buf = buffer_;

    /* get the character to match and its position from the selection, or
       the character before the insert point if nothing is selected.
       Give up if too many characters are selected */
    if (!GetSimpleSelection(buf, &selStart, &selEnd)) {
        selEnd = TextGetCursorPos();
        if (overstrike_) {
            selEnd += 1;
        }
        selStart = selEnd - 1;
        if (selStart < 0) {
            QApplication::beep();
            return;
        }
    }
    if ((selEnd - selStart) != 1) {
        QApplication::beep();
        return;
    }

    /* Search for it in the buffer */
    if (!findMatchingChar(buf->BufGetCharacter(selStart), syntaxHighlighter_->GetHighlightInfo(selStart), selStart, 0,
                          buf->BufGetLength(), &matchPos)) {
        QApplication::beep();
        return;
    }

/* temporarily shut off autoShowInsertPos before setting the cursor
   position so MakeSelectionVisible gets a chance to place the cursor
   string at a pleasing position on the screen (otherwise, the cursor would
   be automatically scrolled on screen and MakeSelectionVisible would do
   nothing) */
#if 0
    XtVaSetValues(textNautoShowInsertPos, false, nullptr);
#endif
    TextSetCursorPos(matchPos + 1);
    MakeSelectionVisible();
#if 0
    XtVaSetValues(textNautoShowInsertPos, true, nullptr);
#endif
}

/*
** Find the start and end of a single line selection.  Hides rectangular
** selection issues for older routines which use selections that won't
** span lines.
*/
bool NirvanaQt::GetSimpleSelection(TextBuffer *buf, int *left, int *right) {
    int selStart;
    int selEnd;
    bool isRect;
    int rectStart;
    int rectEnd;
    int lineStart;

    /* get the character to match and its position from the selection, or
       the character before the insert point if nothing is selected.
       Give up if too many characters are selected */
    if (!buf->BufGetSelectionPos(&selStart, &selEnd, &isRect, &rectStart, &rectEnd))
        return false;
    if (isRect) {
        lineStart = buf->BufStartOfLine(selStart);
        selStart = buf->BufCountForwardDispChars(lineStart, rectStart);
        selEnd = buf->BufCountForwardDispChars(lineStart, rectEnd);
    }
    *left = selStart;
    *right = selEnd;
    return true;
}

/*
** If the selection (or cursor position if there's no selection) is not
** fully shown, scroll to bring it in to view.  Note that as written,
** this won't work well with multi-line selections.  Modest re-write
** of the horizontal scrolling part would be quite easy to make it work
** well with rectangular selections.
*/
void NirvanaQt::MakeSelectionVisible() {
    int left, right;
    bool isRect;
    int rectStart, rectEnd, horizOffset;
    int scrollOffset, leftX, rightX, y, rows, margin;
    int topLineNum, lastLineNum, rightLineNum, leftLineNum, linesToScroll;
    int topChar = TextFirstVisiblePos();
    int lastChar = TextLastVisiblePos();
    int targetLineNum;
    int width;

    /* find out where the selection is */
    if (!buffer_->BufGetSelectionPos(&left, &right, &isRect, &rectStart, &rectEnd)) {
        left = right = TextGetCursorPos();
        isRect = false;
    }

    /* Check vertical positioning unless the selection is already shown or
       already covers the display.  If the end of the selection is below
       bottom, scroll it in to view until the end selection is scrollOffset
       lines from the bottom of the display or the start of the selection
       scrollOffset lines from the top.  Calculate a pleasing distance from the
       top or bottom of the window, to scroll the selection to (if scrolling is
       necessary), around 1/3 of the height of the window */
    if (!((left >= topChar && right <= lastChar) || (left <= topChar && right >= lastChar))) {
#if 0
        XtVaGetValues(textNrows, &rows, nullptr);
#else
        rows = 0;
#endif
        scrollOffset = rows / 3;
        TextGetScroll(&topLineNum, &horizOffset);
        if (right > lastChar) {
            /* End of sel. is below bottom of screen */
            leftLineNum = topLineNum + TextDCountLines(topChar, left, false);
            targetLineNum = topLineNum + scrollOffset;
            if (leftLineNum >= targetLineNum) {
                /* Start of sel. is not between top & target */
                linesToScroll = TextDCountLines(lastChar, right, false) + scrollOffset;
                if (leftLineNum - linesToScroll < targetLineNum)
                    linesToScroll = leftLineNum - targetLineNum;
                /* Scroll start of selection to the target line */
                TextSetScroll(topLineNum + linesToScroll, horizOffset);
            }
        } else if (left < topChar) {
            /* Start of sel. is above top of screen */
            lastLineNum = topLineNum + rows;
            rightLineNum = lastLineNum - TextDCountLines(right, lastChar, false);
            targetLineNum = lastLineNum - scrollOffset;
            if (rightLineNum <= targetLineNum) {
                /* End of sel. is not between bottom & target */
                linesToScroll = TextDCountLines(left, topChar, false) + scrollOffset;
                if (rightLineNum + linesToScroll > targetLineNum)
                    linesToScroll = targetLineNum - rightLineNum;
                /* Scroll end of selection to the target line */
                TextSetScroll(topLineNum - linesToScroll, horizOffset);
            }
        }
    }

    /* If either end of the selection off screen horizontally, try to bring it
       in view, by making sure both end-points are visible.  Using only end
       points of a multi-line selection is not a great idea, and disaster for
       rectangular selections, so this part of the routine should be re-written
       if it is to be used much with either.  Note also that this is a second
       scrolling operation, causing the display to jump twice.  It's done after
       vertical scrolling to take advantage of TextPosToXY which requires it's
       reqested position to be vertically on screen) */
    if (TextPosToXY(left, &leftX, &y) && TextPosToXY(right, &rightX, &y) && leftX <= rightX) {
        TextGetScroll(&topLineNum, &horizOffset);
#if 0
        XtVaGetValues(textPane, XmNwidth, &width, textNmarginWidth, &margin, nullptr);
#else
        margin = 0;
        width = viewport()->width();
#endif
        if (leftX < margin + lineNumLeft_ + lineNumWidth_)
            horizOffset -= margin + lineNumLeft_ + lineNumWidth_ - leftX;
        else if (rightX > width - margin)
            horizOffset += rightX - (width - margin);
        TextSetScroll(topLineNum, horizOffset);
    }

    /* make sure that the statistics line is up to date */
    UpdateStatsLine();
}

bool NirvanaQt::findMatchingChar(char_type toMatch, void *styleToMatch, int charPos, int startLimit, int endLimit,
                                 int *matchPos) {
    int nestDepth, matchIndex;
    SearchDirection direction;
    int beginPos, pos;
    char_type matchChar, c;
    void *style = nullptr;
    TextBuffer *buf = buffer_;
    bool matchSyntaxBased = matchSyntaxBased_;

    /* If we don't match syntax based, fake a matching style. */
    if (!matchSyntaxBased)
        style = styleToMatch;

    /* Look up the matching character and match direction */
    for (matchIndex = 0; matchIndex < N_MATCH_CHARS; matchIndex++) {
        if (MatchingChars[matchIndex].c == toMatch)
            break;
    }
    if (matchIndex == N_MATCH_CHARS)
        return false;
    matchChar = MatchingChars[matchIndex].match;
    direction = MatchingChars[matchIndex].direction;

    /* find it in the buffer */
    beginPos = (direction == SEARCH_FORWARD) ? charPos + 1 : charPos - 1;
    nestDepth = 1;
    if (direction == SEARCH_FORWARD) {
        for (pos = beginPos; pos < endLimit; pos++) {
            c = buf->BufGetCharacter(pos);
            if (c == matchChar) {
                if (matchSyntaxBased)
                    style = syntaxHighlighter_->GetHighlightInfo(pos);
                if (style == styleToMatch) {
                    nestDepth--;
                    if (nestDepth == 0) {
                        *matchPos = pos;
                        return true;
                    }
                }
            } else if (c == toMatch) {
                if (matchSyntaxBased)
                    style = syntaxHighlighter_->GetHighlightInfo(pos);
                if (style == styleToMatch)
                    nestDepth++;
            }
        }
    } else { /* SEARCH_BACKWARD */
        for (pos = beginPos; pos >= startLimit; pos--) {
            c = buf->BufGetCharacter(pos);
            if (c == matchChar) {
                if (matchSyntaxBased)
                    style = syntaxHighlighter_->GetHighlightInfo(pos);
                if (style == styleToMatch) {
                    nestDepth--;
                    if (nestDepth == 0) {
                        *matchPos = pos;
                        return true;
                    }
                }
            } else if (c == toMatch) {
                if (matchSyntaxBased)
                    style = syntaxHighlighter_->GetHighlightInfo(pos);
                if (style == styleToMatch)
                    nestDepth++;
            }
        }
    }
    return false;
}

int NirvanaQt::TextFirstVisibleLine() {
    return topLineNum_;
}

int NirvanaQt::TextNumVisibleLines() {
    return nVisibleLines_;
}

int NirvanaQt::TextVisibleWidth() {
#if 0
    return width_;
#else
    return viewport()->width();
#endif
}

int NirvanaQt::TextFirstVisiblePos() {
    return firstChar_;
}

int NirvanaQt::TextLastVisiblePos() {
    return lastChar_;
}

/*
** Return the horizontal and vertical scroll positions of the widget
*/
void NirvanaQt::TextGetScroll(int *topLineNum, int *horizOffset) {
    TextDGetScroll(topLineNum, horizOffset);
}

/*
** Set the horizontal and vertical scroll positions of the widget
*/
void NirvanaQt::TextSetScroll(int topLineNum, int horizOffset) {
    TextDSetScroll(topLineNum, horizOffset);
}

void NirvanaQt::SelectToMatchingCharacter() {
    int selStart, selEnd;
    int startPos, endPos, matchPos;
    TextBuffer *buf = buffer_;

    /* get the character to match and its position from the selection, or
       the character before the insert point if nothing is selected.
       Give up if too many characters are selected */
    if (!GetSimpleSelection(buf, &selStart, &selEnd)) {
        selEnd = TextGetCursorPos();
        if (overstrike_)
            selEnd += 1;
        selStart = selEnd - 1;
        if (selStart < 0) {
            QApplication::beep();
            return;
        }
    }
    if ((selEnd - selStart) != 1) {
        QApplication::beep();
        return;
    }

    /* Search for it in the buffer */
    if (!findMatchingChar(buf->BufGetCharacter(selStart), syntaxHighlighter_->GetHighlightInfo(selStart), selStart, 0,
                          buf->BufGetLength(), &matchPos)) {
        QApplication::beep();
        return;
    }
    startPos = (matchPos > selStart) ? selStart : matchPos;
    endPos = (matchPos > selStart) ? matchPos : selStart;

/* temporarily shut off autoShowInsertPos before setting the cursor
   position so MakeSelectionVisible gets a chance to place the cursor
   string at a pleasing position on the screen (otherwise, the cursor would
   be automatically scrolled on screen and MakeSelectionVisible would do
   nothing) */
#if 0
    XtVaSetValues(window->lastFocus, textNautoShowInsertPos, false, nullptr);
#endif
    /* select the text between the matching characters */
    buf->BufSelect(startPos, endPos + 1);
    MakeSelectionVisible();
#if 0
    XtVaSetValues(window->lastFocus, textNautoShowInsertPos, true, nullptr);
#endif
}

void NirvanaQt::FillSelection() {
    TextBuffer *buf = buffer_;
    char_type *text;
    char_type *filledText;
    int left, right, nCols, len, rectStart, rectEnd;
    bool isRect;
    int rightMargin, wrapMargin;
    int insertPos = TextGetCursorPos();
    int hasSelection = buf->BufGetPrimarySelection().selected;

    Q_UNUSED(nCols);
    Q_UNUSED(wrapMargin);

    /* Find the range of characters and get the text to fill.  If there is a
       selection, use it but extend non-rectangular selections to encompass
       whole lines.  If there is no selection, find the paragraph containing
       the insertion cursor */
    if (!buf->BufGetSelectionPos(&left, &right, &isRect, &rectStart, &rectEnd)) {
        left = findParagraphStart(buf, insertPos);
        right = findParagraphEnd(buf, insertPos);
        if (left == right) {
            QApplication::beep();
            return;
        }
        text = buf->BufGetRange(left, right);
    } else if (isRect) {
        left = buf->BufStartOfLine(left);
        right = buf->BufEndOfLine(right);
        text = buf->BufGetTextInRect(left, right, rectStart, INT_MAX);
    } else {
        left = buf->BufStartOfLine(left);
        if (right != 0 && buf->BufGetCharacter(right - 1) != '\n') {
            right = buf->BufEndOfLine(right);
            if (right < buf->BufGetLength())
                right++;
        }
        buf->BufSelect(left, right);
        text = buf->BufGetRange(left, right);
    }

    /* Find right margin either as specified in the rectangular selection, or
       by measuring the text and querying the window's wrap margin (or width) */
    if (hasSelection && isRect) {
        rightMargin = rectEnd - rectStart;
    } else {
#if 0
        XtVaGetValues(window->textArea,
                textNcolumns, &nCols,
                textNwrapMargin, &wrapMargin,
                nullptr);
        rightMargin = (wrapMargin == 0 ? nCols : wrapMargin);
#else
        rightMargin = wrapMargin_ != 0 ? wrapMargin_ : viewport()->width() / fixedFontWidth_;
#endif
    }

    /* Fill the text */
    filledText = fillParagraphs(text, rightMargin, buf->BufGetTabDistance(), buf->BufGetUseTabs(),
                                buf->BufGetNullSubsChar(), &len, false);
    delete[] text;

    /* Replace the text in the window */
    if (hasSelection && isRect) {
        buf->BufReplaceRect(left, right, rectStart, INT_MAX, filledText);
        buf->BufRectSelect(left, buf->BufEndOfLine(buf->BufCountForwardNLines(left, countLines(filledText) /*-1*/)),
                           rectStart, rectEnd);
    } else {
        buf->BufReplace(left, right, filledText);
        if (hasSelection)
            buf->BufSelect(left, left + len);
    }
    delete[] filledText;

    /* Find a reasonable cursor position.  Usually insertPos is best, but
       if the text was indented, positions can shift */
    if (hasSelection && isRect) {
        TextSetCursorPos(buf->BufGetCursorPosHint());
    } else {
        TextSetCursorPos(insertPos < left ? left : (insertPos > left + len ? left + len : insertPos));
    }
}

/*
** Find the boundaries of the paragraph containing pos
*/
int NirvanaQt::findParagraphEnd(TextBuffer *buf, int startPos) {
    char_type c;
    int pos;
    static const char_type whiteChars[] = " \t";

    pos = buf->BufEndOfLine(startPos) + 1;
    while (pos < buf->BufGetLength()) {
        c = buf->BufGetCharacter(pos);
        if (c == '\n')
            break;
        if (strchr(whiteChars, c) != nullptr)
            pos++;
        else
            pos = buf->BufEndOfLine(pos) + 1;
    }
    return pos < buf->BufGetLength() ? pos : buf->BufGetLength();
}

int NirvanaQt::findParagraphStart(TextBuffer *buf, int startPos) {
    char_type c;
    int pos, parStart;
    static const char_type whiteChars[] = " \t";

    if (startPos == 0)
        return 0;
    parStart = buf->BufStartOfLine(startPos);
    pos = parStart - 2;
    while (pos > 0) {
        c = buf->BufGetCharacter(pos);
        if (c == '\n')
            break;
        if (strchr(whiteChars, c) != nullptr)
            pos--;
        else {
            parStart = buf->BufStartOfLine(pos);
            pos = parStart - 2;
        }
    }
    return parStart > 0 ? parStart : 0;
}

/*
** Fill multiple paragraphs between rightMargin and an implied left margin
** and first line indent determined by analyzing the text.  alignWithFirst
** aligns subsequent paragraphs with the margins of the first paragraph (a
** capability not currently used in NEdit, but carried over from code for
** previous versions which did all paragraphs together).
*/
char_type *NirvanaQt::fillParagraphs(char_type *text, int rightMargin, int tabDist, bool useTabs, char_type nullSubsChar,
                                int *filledLen, int alignWithFirst) {
    int paraEnd, fillEnd;
    char_type *c;
    char_type ch;
    char_type *secondLineStart;
    char_type *paraText;
    char_type *filledText;
    int firstLineLen;
    int firstLineIndent;
    int leftMargin;
    int len;

    /* Create a buffer to accumulate the filled paragraphs */
    TextBuffer *const buf = new TextBuffer();
    buf->BufSetAll(text);

    /*
    ** Loop over paragraphs, filling each one, and accumulating the results
    ** in buf
    */
    int paraStart = 0;
    for (;;) {

        /* Skip over white space */
        while (paraStart < buf->BufGetLength()) {
            ch = buf->BufGetCharacter(paraStart);
            if (ch != ' ' && ch != '\t' && ch != '\n')
                break;
            paraStart++;
        }
        if (paraStart >= buf->BufGetLength())
            break;
        paraStart = buf->BufStartOfLine(paraStart);

        /* Find the end of the paragraph */
        paraEnd = findParagraphEnd(buf, paraStart);

        /* Operate on either the one paragraph, or to make them all identical,
           do all of them together (fill paragraph can format all the paragraphs
           it finds with identical specs if it gets passed more than one) */
        fillEnd = alignWithFirst ? buf->BufGetLength() : paraEnd;

        /* Get the paragraph in a text string (or all of the paragraphs if
           we're making them all the same) */
        paraText = buf->BufGetRange(paraStart, fillEnd);

        /* Find separate left margins for the first and for the first line of
           the paragraph, and for rest of the remainder of the paragraph */
        for (c = paraText; *c != '\0' && *c != '\n'; c++)
            ;
        firstLineLen = c - paraText;
        secondLineStart = *c == '\0' ? paraText : c + 1;
        firstLineIndent = findLeftMargin(paraText, firstLineLen, tabDist);
        leftMargin = findLeftMargin(secondLineStart, paraEnd - paraStart - (secondLineStart - paraText), tabDist);

        /* Fill the paragraph */
        filledText =
            fillParagraph(paraText, leftMargin, firstLineIndent, rightMargin, tabDist, useTabs, nullSubsChar, &len);
        delete[] paraText;

        /* Replace it in the buffer */
        buf->BufReplace(paraStart, fillEnd, filledText);
        delete[] filledText;

        /* move on to the next paragraph */
        paraStart += len;
    }

    /* Free the buffer and return its contents */
    filledText = buf->BufGetAll();
    *filledLen = buf->BufGetLength();
    delete buf;
    return filledText;
}

/*
** Trim leading space, and arrange text to fill between leftMargin and
** rightMargin (except for the first line which fills from firstLineIndent),
** re-creating whitespace to the left of the text using tabs (if allowTabs is
** true) calculated using tabDist, and spaces.  Returns a newly allocated
** string as the function result, and the length of the new string in filledLen.
*/
char_type *NirvanaQt::fillParagraph(char_type *text, int leftMargin, int firstLineIndent, int rightMargin, int tabDist,
                               bool allowTabs, char_type nullSubsChar, int *filledLen) {

    char_type *outText, *c, *b;
    int col, cleanedLen, indentLen, leadIndentLen, nLines = 1;
    bool inWhitespace;

    /* remove leading spaces, convert newlines to spaces */
    char_type *cleanedText = new char_type[traits_type::length(text) + 1];
    char_type *outPtr = cleanedText;
    bool inMargin = true;

    for (c = text; *c != '\0'; c++) {
        if (*c == '\t' || *c == ' ') {
            if (!inMargin)
                *outPtr++ = *c;
        } else if (*c == '\n') {
            if (inMargin) {
                /* a newline before any text separates paragraphs, so leave
                   it in, back up, and convert the previous space back to \n */
                if (outPtr > cleanedText && *(outPtr - 1) == ' ')
                    *(outPtr - 1) = '\n';
                *outPtr++ = '\n';
                nLines += 2;
            } else
                *outPtr++ = ' ';
            inMargin = true;
        } else {
            *outPtr++ = *c;
            inMargin = false;
        }
    }
    cleanedLen = outPtr - cleanedText;
    *outPtr = '\0';

    /* Put back newlines breaking text at word boundaries within the margins.
       Algorithm: scan through characters, counting columns, and when the
       margin width is exceeded, search backward for beginning of the word
       and convert the last whitespace character into a newline */
    col = firstLineIndent;
    for (c = cleanedText; *c != '\0'; c++) {
        if (*c == '\n')
            col = leftMargin;
        else
            col += TextBuffer::BufCharWidth(*c, col, tabDist, nullSubsChar);
        if (col - 1 > rightMargin) {
            inWhitespace = true;
            for (b = c; b >= cleanedText && *b != '\n'; b--) {
                if (*b == '\t' || *b == ' ') {
                    if (!inWhitespace) {
                        *b = '\n';
                        c = b;
                        col = leftMargin;
                        nLines++;
                        break;
                    }
                } else
                    inWhitespace = false;
            }
        }
    }
    nLines++;

    /* produce a string to prepend to lines to indent them to the left margin */
    char_type *leadIndentStr = makeIndentString(firstLineIndent, tabDist, allowTabs, &leadIndentLen);
    char_type *indentString = makeIndentString(leftMargin, tabDist, allowTabs, &indentLen);

    /* allocate memory for the finished string */
    outText = new char_type[(cleanedLen + leadIndentLen + indentLen * (nLines - 1) + 1)];
    outPtr = outText;

    /* prepend the indent string to each line of the filled text */
    strncpy(outPtr, leadIndentStr, leadIndentLen);
    outPtr += leadIndentLen;
    for (c = cleanedText; *c != '\0'; c++) {
        *outPtr++ = *c;
        if (*c == '\n') {
            strncpy(outPtr, indentString, indentLen);
            outPtr += indentLen;
        }
    }

    /* convert any trailing space to newline.  Add terminating null */
    if (*(outPtr - 1) == ' ')
        *(outPtr - 1) = '\n';
    *outPtr = '\0';

    /* clean up, return result */
    delete[] cleanedText;
    delete[] leadIndentStr;
    delete[] indentString;
    *filledLen = outPtr - outText;
    return outText;
}

/*
** Find the implied left margin of a text string (the number of columns to the
** first non-whitespace character on any line) up to either the terminating
** null character at the end of the string, or "length" characters, whever
** comes first.
*/
int NirvanaQt::findLeftMargin(char_type *text, int length, int tabDist) {
    char_type *c;
    int col = 0, leftMargin = INT_MAX;
    bool inMargin = true;

    for (c = text; *c != '\0' && c - text < length; c++) {
        if (*c == '\t') {
            col += TextBuffer::BufCharWidth('\t', col, tabDist, '\0');
        } else if (*c == ' ') {
            col++;
        } else if (*c == '\n') {
            col = 0;
            inMargin = true;
        } else {
            /* non-whitespace */
            if (col < leftMargin && inMargin)
                leftMargin = col;
            inMargin = false;
        }
    }

    /* if no non-white text is found, the leftMargin will never be set */
    if (leftMargin == INT_MAX)
        return 0;

    return leftMargin;
}

char_type *NirvanaQt::makeIndentString(int indent, int tabDist, bool allowTabs, int *nChars) {
    char_type *indentString;

    char_type *outPtr = indentString = new char_type[indent + 1];

    if (allowTabs) {
        for (int i = 0; i < indent / tabDist; i++) {
            *outPtr++ = '\t';
        }
        for (int i = 0; i < indent % tabDist; i++) {
            *outPtr++ = ' ';
        }
    } else {
        for (int i = 0; i < indent; i++) {
            *outPtr++ = ' ';
        }
    }

    *outPtr = '\0';
    *nChars = outPtr - indentString;
    return indentString;
}

void NirvanaQt::undoAP() {

    if (checkReadOnly()) {
        return;
    }

    Undo();
}

void NirvanaQt::redoAP() {
    if (checkReadOnly()) {
        return;
    }

    Redo();
}

void NirvanaQt::Undo() {

    /* return if nothing to undo */
    if (!undo_)
        return;

    /* BufReplace will eventually call SaveUndoInformation.  This is mostly
       good because it makes accumulating redo operations easier, however
       SaveUndoInformation needs to know that it is being called in the context
       of an undo.  The inUndo field in the undo record indicates that this
       record is in the process of being undone. */
    undo_->inUndo = true;

    /* use the saved undo information to reverse changes */
    buffer_->BufReplace(undo_->startPos, undo_->endPos, (undo_->oldText != nullptr ? undo_->oldText : ""));

    const int restoredTextLength = undo_->oldText != nullptr ? traits_type::length(undo_->oldText) : 0;
    if (!buffer_->BufGetPrimarySelection().selected || undoModifiesSelection_) {
        /* position the cursor in the focus pane after the changed text
           to show the user where the undo was done */
        TextSetCursorPos(undo_->startPos + restoredTextLength);
    }

    if (undoModifiesSelection_) {
        if (restoredTextLength > 0) {
            buffer_->BufSelect(undo_->startPos, undo_->startPos + restoredTextLength);
        } else {
            buffer_->BufUnselect();
        }
    }
    MakeSelectionVisible();

    /* restore the file's unmodified status if the file was unmodified
       when the change being undone was originally made.  Also, remove
       the backup file, since the text in the buffer is now identical to
       the original file */
    if (undo_->restoresToSaved) {
        SetWindowModified(false);
        RemoveBackupFile();
    }

    /* free the undo record and remove it from the chain */
    removeUndoItem();
}

void NirvanaQt::Redo() {

    /* return if nothing to redo */
    if (!redo_) {
        return;
    }

    /* BufReplace will eventually call SaveUndoInformation.  To indicate
       to SaveUndoInformation that this is the context of a redo operation,
       we set the inUndo indicator in the redo record */
    redo_->inUndo = true;

    /* use the saved redo information to reverse changes */
    buffer_->BufReplace(redo_->startPos, redo_->endPos, (redo_->oldText != nullptr ? redo_->oldText : ""));

    const int restoredTextLength = redo_->oldText != nullptr ? traits_type::length(redo_->oldText) : 0;
    if (!buffer_->BufGetPrimarySelection().selected || undoModifiesSelection_) {
        /* position the cursor in the focus pane after the changed text
           to show the user where the undo was done */
        TextSetCursorPos(redo_->startPos + restoredTextLength);
    }
    if (undoModifiesSelection_) {

        if (restoredTextLength > 0) {
            buffer_->BufSelect(redo_->startPos, redo_->startPos + restoredTextLength);
        } else {
            buffer_->BufUnselect();
        }
    }
    MakeSelectionVisible();

    /* restore the file's unmodified status if the file was unmodified
       when the change being redone was originally made. Also, remove
       the backup file, since the text in the buffer is now identical to
       the original file */
    if (redo_->restoresToSaved) {
        SetWindowModified(false);
        RemoveBackupFile();
    }

    /* remove the redo record from the chain and free it */
    removeRedoItem();
}

/*
** Pop (remove and free) the current (front) undo record from the undo list
*/
void NirvanaQt::removeUndoItem() {
    UndoInfo *undo = undo_;

    if (undo == nullptr)
        return;

    /* Decrement the operation and memory counts */
    undoOpCount_--;
    undoMemUsed_ -= undo->oldLen;

    /* Remove and free the item */
    undo_ = undo->next;
    freeUndoRecord(undo);

    /* if there are no more undo records left, dim the Undo menu item */
    if (!undo_) {
#if 0
    SetSensitive(window, window->undoItem, false);
    SetBGMenuUndoSensitivity(window, false);
#endif
    }
}

/*
** Pop (remove and free) the current (front) redo record from the redo list
*/
void NirvanaQt::removeRedoItem() {
    UndoInfo *redo = redo_;

    /* Remove and free the item */
    redo_ = redo->next;
    freeUndoRecord(redo);

    /* if there are no more redo records left, dim the Redo menu item */
    if (!redo_) {
#if 0
        SetSensitive(window, window->redoItem, false);
        SetBGMenuRedoSensitivity(window, false);
#endif
    }
}

void NirvanaQt::freeUndoRecord(UndoInfo *undo) {
    if (!undo)
        return;

    delete[] undo->oldText;
    delete undo;
}

/*
** Remove the backup file associated with this window
*/
void NirvanaQt::RemoveBackupFile() {
#if 0
    char_type name[MAXPATHLEN];

    /* Don't delete backup files when backups aren't activated. */
    if (!autoSave_)
        return;

    backupFileName(window, name, sizeof(name));
    remove(name);
#endif
}

/*
** Change the window appearance and the window data structure to show
** that the file it contains has been modified
*/
void NirvanaQt::SetWindowModified(bool modified) {

    if (!fileChanged_ && modified) {
#if 0
        SetSensitive(window, window->closeItem, true);
#endif
        fileChanged_ = true;
#if 0
        UpdateWindowTitle();
        RefreshTabState();
#endif
    } else if (fileChanged_ && !modified) {
        fileChanged_ = false;
#if 0
        UpdateWindowTitle();
        RefreshTabState();
#endif
    }
}

void NirvanaQt::modifiedCB(int pos, int nInserted, int nDeleted, int nRestyled, const char_type *deletedText) {

    Q_UNUSED(nRestyled);

    int selected = buffer_->BufGetPrimarySelection().selected;

    /* update the table of bookmarks */
    if (!ignoreModify_) {
        UpdateMarkTable(pos, nInserted, nDeleted);
    }

    /* Check and dim/undim selection related menu items */
    if ((wasSelected_ && !selected) || (!wasSelected_ && selected)) {
        wasSelected_ = selected;

/* do not refresh shell-level items (window, menu-bar etc)
   when motifying non-top document */
#if 0
        if (IsTopDocument(window)) {

            XtSetSensitive(window->printSelItem, selected);
            XtSetSensitive(window->cutItem, selected);
            XtSetSensitive(window->copyItem, selected);
            XtSetSensitive(window->delItem, selected);
            /* Note we don't change the selection for items like
               "Open Selected" and "Find Selected".  That's because
               it works on selections in external applications.
               Desensitizing it if there's no NEdit selection
               disables this feature. */
#ifndef VMS
            XtSetSensitive(window->filterItem, selected);
#endif

            DimSelectionDepUserMenuItems(window, selected);
            if (window->replaceDlog != nullptr && XtIsManaged(window->replaceDlog))
            {
            UpdateReplaceActionButtons(window);
            }

    }
#endif
    }

    /* When the program needs to make a change to a text area without without
       recording it for undo or marking file as changed it sets ignoreModify */
    if (ignoreModify_ || (nDeleted == 0 && nInserted == 0))
        return;

    /* Make sure line number display is sufficient for new data */
    updateLineNumDisp();

    /* Save information for undoing this operation (this call also counts
       characters and editing operations for triggering autosave */
    SaveUndoInformation(pos, nInserted, nDeleted, deletedText);

    /* Trigger automatic backup if operation or character limits reached */
    if (autoSave_ && (autoSaveCharCount_ > AUTOSAVE_CHAR_LIMIT || autoSaveOpCount_ > AUTOSAVE_OP_LIMIT)) {
        WriteBackupFile();
        autoSaveCharCount_ = 0;
        autoSaveOpCount_ = 0;
    }

    /* Indicate that the window has now been modified */
    SetWindowModified(true);

    /* Update # of bytes, and line and col statistics */
    UpdateStatsLine();

    /* Check if external changes have been made to file and warn user */
    CheckForChangesToFile();
}

int NirvanaQt::updateLineNumDisp() {
#if 0
    if (!showLineNumbers_) {
        return 0;
    }

    /* Decide how wide the line number field has to be to display all
       possible line numbers */
    return updateGutterWidth();
#endif
    return 0;
}

/*
** Update the optional statistics line.
*/
void NirvanaQt::UpdateStatsLine() {
#if 0
    int line, pos, colNum;
    char_type *string, *format, slinecol[32];
    Widget statW = window->statsLine;
    XmString xmslinecol;
#ifdef SGI_CUSTOM
    char_type *sleft;
	char_type *smid;
	char_type *sright;
#endif

    if (!IsTopDocument(window))
      return;

    /* This routine is called for each character typed, so its performance
       affects overall editor perfomance.  Only update if the line is on. */
    if (!window->showStats)
        return;

    /* Compose the string to display. If line # isn't available, leave it off */
    pos = TextGetCursorPos(window->lastFocus);
    string = XtMalloc(traits_type::length(window->filename) + traits_type::length(window->path) + 45);
    format = window->fileFormat == DOS_FILE_FORMAT ? " DOS" :
            (window->fileFormat == MAC_FILE_FORMAT ? " Mac" : "");
    if (!TextPosToLineAndCol(window->lastFocus, pos, &line, &colNum)) {
        sprintf(string, "%s%s%s %d bytes", window->path, window->filename, format, window->buffer->length);
        sprintf(slinecol, "L: ---  C: ---");
    } else {
        sprintf(slinecol, "L: %d  C: %d", line, colNum);
        if (window->showLineNumbers)
            sprintf(string, "%s%s%s byte %d of %d", window->path,
                    window->filename, format, pos,
                    window->buffer->length);
        else
            sprintf(string, "%s%s%s %d bytes", window->path,
                    window->filename, format, window->buffer->length);
    }

    /* Update the line/column number */
    xmslinecol = XmStringCreateSimple(slinecol);
    XtVaSetValues( window->statsLineColNo,
            XmNlabelString, xmslinecol, nullptr );
    XmStringFree(xmslinecol);

    /* Don't clobber the line if there's a special message being displayed */
    if (!window->modeMessageDisplayed) {
        /* Change the text in the stats line */
#ifdef SGI_CUSTOM
        /* don't show full pathname, just dir and filename (+ byte info) */
        smid = strchr(string, '/');
        if ( smid != nullptr ) {
            sleft = smid;
            sright = strrchr(string, '/');
            while (strcmp(smid, sright)) {
                    sleft = smid;
                    smid = strchr(sleft + 1, '/');
            }
            XmTextReplace(statW, 0, XmTextGetLastPosition(statW), sleft + 1);
        } else
            XmTextReplace(statW, 0, XmTextGetLastPosition(statW), string);
#else
        XmTextReplace(statW, 0, XmTextGetLastPosition(statW), string);
#endif
    }
    XtFree(string);

    /* Update the line/col display */
    xmslinecol = XmStringCreateSimple(slinecol);
    XtVaSetValues(window->statsLineColNo,
            XmNlabelString, xmslinecol, nullptr);
    XmStringFree(xmslinecol);
#endif
}

/*
** Check if the file in the window was changed by an external source.
** and put up a warning dialog if it has.
*/
void NirvanaQt::CheckForChangesToFile() {
}

/*
** Create a backup file for the current window.  The name for the backup file
** is generated using the name and path stored in the window and adding a
** tilde (~) on UNIX and underscore (_) on VMS to the beginning of the name.
*/
bool NirvanaQt::WriteBackupFile() {
    return true;
}

/*
** Keep the marks in the windows book-mark table up to date across
** changes to the underlying buffer
*/
void NirvanaQt::UpdateMarkTable(int pos, int nInserted, int nDeleted) {
    Q_UNUSED(pos);
    Q_UNUSED(nInserted);
    Q_UNUSED(nDeleted);
#if 0
    int i;

    for (i=0; i<window->nMarks; i++) {
        maintainSelection(&window->markTable[i].sel, pos, nInserted,
                nDeleted);
        maintainPosition(&window->markTable[i].cursorPos, pos, nInserted,
                nDeleted);
    }
#endif
}

/*
** SaveUndoInformation stores away the changes made to the text buffer.  As a
** side effect, it also increments the autoSave operation and character counts
** since it needs to do the classification anyhow.
**
** Note: This routine must be kept efficient.  It is called for every
**       character typed.
*/
void NirvanaQt::SaveUndoInformation(int pos, int nInserted, int nDeleted, const char_type *deletedText) {

    UndoTypes newType;
    UndoTypes oldType;
    UndoInfo *u, *undo = undo_;
    int isUndo = (undo != nullptr && undo->inUndo);
    int isRedo = (redo_ != nullptr && redo_->inUndo);

    /* redo operations become invalid once the user begins typing or does
       other editing.  If this is not a redo or undo operation and a redo
       list still exists, clear it and dim the redo menu item */
    if (!(isUndo || isRedo) && redo_ != nullptr)
        ClearRedoList();

    /* figure out what kind of editing operation this is, and recall
       what the last one was */
    newType = determineUndoType(nInserted, nDeleted);
    if (newType == UNDO_NOOP)
        return;
    oldType = (undo == nullptr || isUndo) ? UNDO_NOOP : undo->type;

    /*
    ** Check for continuations of single character operations.  These are
    ** accumulated so a whole insertion or deletion can be undone, rather
    ** than just the last character that the user typed.  If the window
    ** is currently in an unmodified state, don't accumulate operations
    ** across the save, so the user can undo back to the unmodified state.
    */
    if (fileChanged_) {

        /* normal sequential character insertion */
        if (((oldType == ONE_CHAR_INSERT || oldType == ONE_CHAR_REPLACE) && newType == ONE_CHAR_INSERT) &&
            (pos == undo->endPos)) {
            undo->endPos++;
            autoSaveCharCount_++;
            return;
        }

        /* overstrike mode replacement */
        if ((oldType == ONE_CHAR_REPLACE && newType == ONE_CHAR_REPLACE) && (pos == undo->endPos)) {
            appendDeletedText(deletedText, nDeleted, FORWARD);
            undo->endPos++;
            autoSaveCharCount_++;
            return;
        }

        /* forward delete */
        if ((oldType == ONE_CHAR_DELETE && newType == ONE_CHAR_DELETE) && (pos == undo->startPos)) {
            appendDeletedText(deletedText, nDeleted, FORWARD);
            return;
        }

        /* reverse delete */
        if ((oldType == ONE_CHAR_DELETE && newType == ONE_CHAR_DELETE) && (pos == undo->startPos - 1)) {
            appendDeletedText(deletedText, nDeleted, REVERSE);
            undo->startPos--;
            undo->endPos--;
            return;
        }
    }

    /*
    ** The user has started a new operation, create a new undo record
    ** and save the new undo data.
    */
    undo = new UndoInfo;
    undo->oldLen = 0;
    undo->oldText = nullptr;
    undo->type = newType;
    undo->inUndo = false;
    undo->restoresToSaved = false;
    undo->startPos = pos;
    undo->endPos = pos + nInserted;

    /* if text was deleted, save it */
    if (nDeleted > 0) {
        undo->oldLen = nDeleted + 1; /* +1 is for null at end */
        undo->oldText = new char_type[nDeleted + 1];
        strcpy(undo->oldText, deletedText);
    }

    /* increment the operation count for the autosave feature */
    autoSaveOpCount_++;

    /* if the window is currently unmodified, remove the previous
       restoresToSaved marker, and set it on this record */
    if (!fileChanged_) {
        undo->restoresToSaved = true;
        for (u = undo_; u != nullptr; u = u->next)
            u->restoresToSaved = false;
        for (u = redo_; u != nullptr; u = u->next)
            u->restoresToSaved = false;
    }

    /* Add the new record to the undo list  unless SaveUndoInfo is
       saving information generated by an Undo operation itself, in
       which case, add the new record to the redo list. */
    if (isUndo)
        addRedoItem(undo);
    else
        addUndoItem(undo);
}

/*
** ClearUndoList, ClearRedoList
**
** Functions for clearing all of the information off of the undo or redo
** lists and adjusting the edit menu accordingly
*/
void NirvanaQt::ClearUndoList() {
    while (undo_ != NULL)
        removeUndoItem();
}
void NirvanaQt::ClearRedoList() {
    while (redo_ != NULL)
        removeRedoItem();
}

/*
** Add an undo record (already allocated by the caller) to the window's undo
** list if the item pushes the undo operation or character counts past the
** limits, trim the undo list to an acceptable length.
*/
void NirvanaQt::addUndoItem(UndoInfo *undo) {

    /* Make the undo menu item sensitive now that there's something to undo */
    if (undo_ == NULL) {
#if 0
        SetSensitive(window, window->undoItem, True);
    SetBGMenuUndoSensitivity(window, True);
#endif
    }

    /* Add the item to the beginning of the list */
    undo->next = undo_;
    undo_ = undo;

    /* Increment the operation and memory counts */
    undoOpCount_++;
    undoMemUsed_ += undo->oldLen;

    /* Trim the list if it exceeds any of the limits */
    if (undoOpCount_ > UNDO_OP_LIMIT)
        trimUndoList(UNDO_OP_TRIMTO);
    if (undoMemUsed_ > UNDO_WORRY_LIMIT)
        trimUndoList(UNDO_WORRY_TRIMTO);
    if (undoMemUsed_ > UNDO_PURGE_LIMIT)
        trimUndoList(UNDO_PURGE_TRIMTO);
}

/*
** Add an item (already allocated by the caller) to the window's redo list.
*/
void NirvanaQt::addRedoItem(UndoInfo *redo) {
    /* Make the redo menu item sensitive now that there's something to redo */
    if (redo_ == NULL) {
#if 0
        SetSensitive(window, window->redoItem, True);
    SetBGMenuRedoSensitivity(window, True);
#endif
    }

    /* Add the item to the beginning of the list */
    redo->next = redo_;
    redo_ = redo;
}

UndoTypes NirvanaQt::determineUndoType(int nInserted, int nDeleted) {
    int textDeleted, textInserted;

    textDeleted = (nDeleted > 0);
    textInserted = (nInserted > 0);

    if (textInserted && !textDeleted) {
        /* Insert */
        if (nInserted == 1)
            return ONE_CHAR_INSERT;
        else
            return BLOCK_INSERT;
    } else if (textInserted && textDeleted) {
        /* Replace */
        if (nInserted == 1)
            return ONE_CHAR_REPLACE;
        else
            return BLOCK_REPLACE;
    } else if (!textInserted && textDeleted) {
        /* Delete */
        if (nDeleted == 1)
            return ONE_CHAR_DELETE;
        else
            return BLOCK_DELETE;
    } else {
        /* Nothing deleted or inserted */
        return UNDO_NOOP;
    }
}

/*
** Add deleted text to the beginning or end
** of the text saved for undoing the last operation.  This routine is intended
** for continuing of a string of one character deletes or replaces, but will
** work with more than one character.
*/
void NirvanaQt::appendDeletedText(const char_type *deletedText, int deletedLen, int direction) {
    UndoInfo *undo = undo_;
    char_type *comboText;

    /* re-allocate, adding space for the new character(s) */
    comboText = new char_type[undo->oldLen + deletedLen];

    /* copy the new character and the already deleted text to the new memory */
    if (direction == FORWARD) {
	
		std::copy_n(undo->oldText, undo->oldLen, comboText);
		std::copy_n(deletedText, deletedLen, comboText + undo->oldLen);
		comboText[undo->oldLen + deletedLen] = _T('\0');
    } else {
	
		std::copy_n(deletedText, deletedLen, comboText);
		std::copy_n(undo->oldText, undo->oldLen, comboText + deletedLen);
		comboText[undo->oldLen + deletedLen] = _T('\0');	
    }

    /* keep track of the additional memory now used by the undo list */
    undoMemUsed_++;

    /* free the old saved text and attach the new */
    delete[] undo->oldText;
    undo->oldText = comboText;
    undo->oldLen += deletedLen;
}

/*
** Trim records off of the END of the undo list to reduce it to length
** maxLength
*/
void NirvanaQt::trimUndoList(int maxLength) {
    int i;
    UndoInfo *u, *lastRec;

    if (undo_ == NULL)
        return;

    /* Find last item on the list to leave intact */
    for (i = 1, u = undo_; i < maxLength && u != NULL; i++, u = u->next)
        ;
    if (u == NULL)
        return;

    /* Trim off all subsequent entries */
    lastRec = u;
    while (lastRec->next != NULL) {
        u = lastRec->next;
        lastRec->next = u->next;
        undoOpCount_--;
        undoMemUsed_ -= u->oldLen;
        freeUndoRecord(u);
    }
}

/*
** Returns the absolute (non-wrapped) line number of the first line displayed.
** Returns 0 if the absolute top line number is not being maintained.
*/
int NirvanaQt::getAbsTopLineNum() {

    if (!continuousWrap_)
        return topLineNum_;
    if (maintainingAbsTopLineNum())
        return absTopLineNum_;

    return 0;
}
