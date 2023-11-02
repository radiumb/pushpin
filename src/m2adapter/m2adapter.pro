CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
TARGET = m2adapter
DESTDIR = ../../bin

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

LIBS += -L$$PWD/../cpp -lpushpin-cpp
PRE_TARGETDEPS += $$PWD/../cpp/libpushpin-cpp.a

include($$OUT_PWD/../rust/lib.pri)
include($$OUT_PWD/../../conf.pri)
include(m2adapter.pri)

unix:!isEmpty(BINDIR) {
	target.path = $$BINDIR
	INSTALLS += target
}
