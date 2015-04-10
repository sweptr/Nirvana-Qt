
TEMPLATE = app
TARGET = NirvanaQt
DEPENDPATH  += .
INCLUDEPATH += . cpp-json/include
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
    regex/RegExp.h \
    cpp-json/include/cpp-json/value.h \
    cpp-json/include/cpp-json/parser.h \
    cpp-json/include/cpp-json/object.h \
    cpp-json/include/cpp-json/json.h \
    cpp-json/include/cpp-json/exception.h \
    cpp-json/include/cpp-json/array.h
	
SOURCES += \
    main.cpp          \
    NirvanaQt.cpp   \
    TextBuffer.cpp \
    Selection.cpp \
    SyntaxHighlighter.cpp \
    X11Colors.cpp \
    regex/RegExp.cpp \
    cpp-json/include/cpp-json/value.tcc \
    cpp-json/include/cpp-json/parser.tcc \
    cpp-json/include/cpp-json/object.tcc \
    cpp-json/include/cpp-json/json.tcc \
    cpp-json/include/cpp-json/array.tcc

OTHER_FILES += \
    DefaultStyle.xml \
    DefaultLanguages.json
