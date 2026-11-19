CONFIG += c++17 console
CONFIG -= app_bundle qt

TARGET = virtual_file_clipboard
TEMPLATE = app

INCLUDEPATH += $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/virtualfileclipboard_win.cpp

HEADERS += \
    ../../app/streaming/virtualfilesource.h \
    ../../app/streaming/virtualfileclipboard_win.h

LIBS += -lole32 -lshell32 -luser32
