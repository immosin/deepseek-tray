QT += core gui widgets

CONFIG += c++17
CONFIG -= debug_and_release

TEMPLATE = app
TARGET = DeepSeekTray
SOURCES += src/main.cpp

LIBS += -lwinhttp
QMAKE_CXXFLAGS += /utf-8
