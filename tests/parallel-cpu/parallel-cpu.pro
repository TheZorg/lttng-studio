#-------------------------------------------------
#
# Project created by QtCreator 2014-09-02T12:49:12
#
#-------------------------------------------------

include(../common.pri)

QT       += testlib concurrent

QT       -= gui

TARGET = tst_parallelcputest
CONFIG   += console
CONFIG   -= app_bundle

TEMPLATE = app


SOURCES += tst_parallelcputest.cpp
DEFINES += SRCDIR=\\\"$$PWD/\\\"

CONFIG += c++11

INCLUDEPATH += /usr/include/glib-2.0/ /usr/lib/x86_64-linux-gnu/glib-2.0/include/

include(../post.pri)
