#-------------------------------------------------
#
# Project created by QtCreator 2014-08-12T12:22:26
#
#-------------------------------------------------

include(../common.pri)

QT       += testlib concurrent

QT       -= gui

TARGET = tst_paralleltest
CONFIG   += console
CONFIG   -= app_bundle

TEMPLATE = app


SOURCES += \
    tst_paralleltest.cpp
DEFINES += SRCDIR=\\\"$$PWD/\\\"

#LIBS += -ltbb

#CONFIG += c++11

QMAKE_CXXFLAGS += -std=gnu++0x

include(../post.pri)
