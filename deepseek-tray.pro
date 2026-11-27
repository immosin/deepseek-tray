QT += core gui widgets

CONFIG += c++17
CONFIG -= debug_and_release

TEMPLATE = app
TARGET = DeepSeekTray

SOURCES += src/main.cpp \
           src/appdirs.cpp \
           src/http.cpp \
           src/provider.cpp \
           src/settings.cpp

HEADERS += src/appdirs.h \
           src/http.h \
           src/provider.h \
           src/settings.h

RC_FILE = src/app.rc

LIBS += -lwinhttp
QMAKE_CXXFLAGS += /utf-8
