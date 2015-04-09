
TEMPLATE = app
TARGET = NirvanaQt
DEPENDPATH  += .
INCLUDEPATH += .
QT += xml

include(qmake/clean-objects.pri)
include(qmake/c++11.pri)
include(qmake/qt5-gui.pri)

linux-g++ {
    QMAKE_CXXFLAGS += -W -Wall -pedantic
}

HEADERS += \
    NirvanaQt.h   \
    TextBuffer.h \
    Selection.h     \
    ICursorMoveHandler.h \
    IHighlightHandler.h \
    IBufferModifiedHandler.h \
    SyntaxHighlighter.h \
    IPreDeleteHandler.h \
    X11Colors.h \
    regex/RegExp.h
	
SOURCES += \
    main.cpp          \
    NirvanaQt.cpp   \
    TextBuffer.cpp \
    Selection.cpp \
    SyntaxHighlighter.cpp \
    X11Colors.cpp \
    regex/RegExp.cpp

OTHER_FILES += \
    DefaultStyle.xml
