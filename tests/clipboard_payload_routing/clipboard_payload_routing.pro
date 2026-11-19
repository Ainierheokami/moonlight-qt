QT += core gui network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = clipboard_payload_routing
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/clipboardsync.cpp

macx:SOURCES += ../../clipboard-helper/macos.mm

HEADERS += \
    ../../app/streaming/clipboardlogging.h \
    ../../app/streaming/clipboardsync.h

SOURCES += regression.cpp file_offer.cpp ../../app/streaming/clipboardipc.cpp
HEADERS += ../../app/streaming/clipboardipc.h
RESOURCES += fixtures.qrc

# Files the host copied (clipboard kind 4)
SOURCES += ../../app/streaming/fileoffer.cpp
HEADERS += ../../app/streaming/fileoffer.h ../../app/streaming/virtualfilesource.h
win32 {
    SOURCES += ../../app/streaming/virtualfileclipboard_win.cpp
    HEADERS += ../../app/streaming/virtualfileclipboard_win.h
    LIBS += -lole32 -lshell32
}
