
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

*msvc* {
    DEFINES += _CRT_SECURE_NO_WARNINGS _SCL_SECURE_NO_WARNINGS
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
    QJson4/QJsonArray.h \
    QJson4/QJsonDocument.h \
    QJson4/QJsonObject.h \
    QJson4/QJsonParseError.h \
    QJson4/QJsonParser.h \
    QJson4/QJsonRoot.h \
    QJson4/QJsonValue.h \
    QJson4/QJsonValueRef.h
	
SOURCES += \
    main.cpp          \
    NirvanaQt.cpp   \
    TextBuffer.cpp \
    Selection.cpp \
    SyntaxHighlighter.cpp \
    X11Colors.cpp \
    regex/RegExp.cpp \
    QJson4/QJsonArray.cpp \
    QJson4/QJsonDocument.cpp \
    QJson4/QJsonObject.cpp \
    QJson4/QJsonParseError.cpp \
    QJson4/QJsonParser.cpp \
    QJson4/QJsonValue.cpp \
    QJson4/QJsonValueRef.cpp

RESOURCES += \
    NirvanaQt.qrc
