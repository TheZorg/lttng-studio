#-------------------------------------------------
#
# Project created by QtCreator 2014-10-02T11:15:04
#
#-------------------------------------------------

include(../common.pri)

QT       += testlib concurrent

QT       -= gui

TARGET = tst_parallelstatemachinetest
CONFIG   += console
CONFIG   -= app_bundle

TEMPLATE = app


SOURCES += tst_parallelstatemachinetest.cpp
DEFINES += SRCDIR=\\\"$$PWD/\\\"

QMAKE_CXXFLAGS_DEBUG -= -O2
QMAKE_CXXFLAGS_DEBUG -= -O1
QMAKE_CFLAGS_DEBUG -= -O2
QMAKE_CFLAGS_DEBUG -= -O1
QMAKE_CXXFLAGS_DEBUG += -std=gnu++0x -O0

CONFIG += c++11 debug

INCLUDEPATH += /usr/include/glib-2.0/ /usr/lib/x86_64-linux-gnu/glib-2.0/include/

include(../post.pri)
