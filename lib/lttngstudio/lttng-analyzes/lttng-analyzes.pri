QT       -= gui

HEADERS += \
    lttng-analyzes/common.h \
    lttng-analyzes/sched.h \
    lttng-analyzes/fsm.h \
    lttng-analyzes/io.h

SOURCES += \
    lttng-analyzes/sched.cpp \
    lttng-analyzes/io.cpp

CONFIG += c++11

QMAKE_CXXFLAGS += -O3 -g
