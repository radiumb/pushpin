TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib
QT -= gui
QT += network
TARGET = pushpin-proxy
DESTDIR = ..

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

LIBS += -L$$PWD/../../corelib -lpushpin-core
PRE_TARGETDEPS += $$PWD/../../corelib/libpushpin-core.a

include($$OUT_PWD/../../rust/lib.pri)
include($$OUT_PWD/../../../conf.pri)
include(libpushpin-proxy.pri)
