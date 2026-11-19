QT += core gui network
CONFIG += c++17
CONFIG -= app_bundle

TARGET = moonlight-clipboard-helper
TEMPLATE = app

include(../globaldefs.pri)

INCLUDEPATH += $$PWD/../app

SOURCES += \
    main.cpp \
    ../app/streaming/clipboardipc.cpp \
    ../app/streaming/clipboardsync.cpp

macx:SOURCES += macos.mm

HEADERS += \
    ../app/streaming/clipboardipc.h \
    ../app/streaming/clipboardlogging.h \
    ../app/streaming/clipboardsync.h

unix:!macx {
    isEmpty(PREFIX) {
        PREFIX = /usr/local
    }
    isEmpty(BINDIR) {
        BINDIR = bin
    }

    target.path = $$PREFIX/$$BINDIR/
    INSTALLS += target
}

# Files the host copied (clipboard kind 4)
SOURCES += ../app/streaming/fileoffer.cpp
HEADERS += ../app/streaming/fileoffer.h ../app/streaming/virtualfilesource.h
win32 {
    SOURCES += ../app/streaming/virtualfileclipboard_win.cpp
    HEADERS += ../app/streaming/virtualfileclipboard_win.h
    LIBS += -lole32 -lshell32
}
