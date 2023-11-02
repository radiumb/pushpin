TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib
QT -= gui
QT += network
TARGET = pushpin-handler
DESTDIR = ..

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

LIBS += -L$$PWD/../../cpp -lpushpin-cpp
PRE_TARGETDEPS += $$PWD/../../cpp/libpushpin-cpp.a

include($$OUT_PWD/../../rust/lib.pri)
include($$OUT_PWD/../../../conf.pri)
include(libpushpin-handler.pri)
