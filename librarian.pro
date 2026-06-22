TEMPLATE = lib
TARGET = librarian
CONFIG += shared plugin no_plugin_name_prefix

xoviextension.target = xovi.cpp
xoviextension.commands = python3 $$(XOVI_REPO)/util/xovigen.py -o xovi.cpp -H xovi.h librarian.xovi
xoviextension.depends = librarian.xovi

QMAKE_EXTRA_TARGETS += xoviextension
PRE_TARGETDEPS += xovi.cpp

QT += quick qml

CONFIG += c++11

SOURCES += src/main.cpp xovi.cpp

LIBS += $$PWD/libs/librm_lines.a $$PWD/libs/libsimdutf.a

QMAKE_CXXFLAGS += -fPIC
