#-------------------------------------------------
#
# Project created by QtCreator 2014-10-31T13:20:56
#
#-------------------------------------------------

include(../common.pri)

QT       += testlib concurrent

QT       -= gui

TARGET = tst_paralleliotest
CONFIG   += console
CONFIG   -= app_bundle

TEMPLATE = app


SOURCES += tst_paralleliotest.cpp
DEFINES += SRCDIR=\\\"$$PWD/\\\"

CONFIG += c++11

QMAKE_CXXFLAGS += -g

INCLUDEPATH += /usr/include/glib-2.0/ /usr/lib/x86_64-linux-gnu/glib-2.0/include/

include(../post.pri)
