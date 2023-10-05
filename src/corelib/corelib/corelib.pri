QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_CFLAGS += $$(CFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)

SRC_DIR = $$PWD/..
QZMQ_DIR = $$SRC_DIR/qzmq
RUST_DIR = $$SRC_DIR/../rust

INCLUDEPATH += $$QZMQ_DIR/src
include($$QZMQ_DIR/src/src.pri)

DEFINES += NO_IRISNET
HEADERS += $$SRC_DIR/processquit.h
SOURCES += $$SRC_DIR/processquit.cpp

INCLUDEPATH += $$RUST_DIR/include

HEADERS += \
	$$SRC_DIR/tnetstring.h \
	$$SRC_DIR/httpheaders.h \
	$$SRC_DIR/zhttprequestpacket.h \
	$$SRC_DIR/zhttpresponsepacket.h \
	$$SRC_DIR/log.h \
	$$SRC_DIR/bufferlist.h \
	$$SRC_DIR/layertracker.h

SOURCES += \
	$$SRC_DIR/tnetstring.cpp \
	$$SRC_DIR/httpheaders.cpp \
	$$SRC_DIR/zhttprequestpacket.cpp \
	$$SRC_DIR/zhttpresponsepacket.cpp \
	$$SRC_DIR/log.cpp \
	$$SRC_DIR/bufferlist.cpp \
	$$SRC_DIR/layertracker.cpp

HEADERS += \
	$$SRC_DIR/packet/httprequestdata.h \
	$$SRC_DIR/packet/httpresponsedata.h \
	$$SRC_DIR/packet/retryrequestpacket.h \
	$$SRC_DIR/packet/wscontrolpacket.h \
	$$SRC_DIR/packet/statspacket.h \
	$$SRC_DIR/packet/zrpcrequestpacket.h \
	$$SRC_DIR/packet/zrpcresponsepacket.h

SOURCES += \
	$$SRC_DIR/packet/retryrequestpacket.cpp \
	$$SRC_DIR/packet/wscontrolpacket.cpp \
	$$SRC_DIR/packet/statspacket.cpp \
	$$SRC_DIR/packet/zrpcrequestpacket.cpp \
	$$SRC_DIR/packet/zrpcresponsepacket.cpp

HEADERS += \
	$$SRC_DIR/callback.h \
	$$SRC_DIR/timerwheel.h \
	$$SRC_DIR/jwt.h \
	$$SRC_DIR/rtimer.h \
	$$SRC_DIR/logutil.h \
	$$SRC_DIR/uuidutil.h \
	$$SRC_DIR/zutil.h \
	$$SRC_DIR/httprequest.h \
	$$SRC_DIR/websocket.h \
	$$SRC_DIR/zhttpmanager.h \
	$$SRC_DIR/zhttprequest.h \
	$$SRC_DIR/zwebsocket.h \
	$$SRC_DIR/zrpcmanager.h \
	$$SRC_DIR/zrpcrequest.h \
	$$SRC_DIR/statusreasons.h \
	$$SRC_DIR/inspectdata.h \
	$$SRC_DIR/cors.h \
	$$SRC_DIR/simplehttpserver.h \
	$$SRC_DIR/stats.h \
	$$SRC_DIR/statsmanager.h \
	$$SRC_DIR/settings.h

SOURCES += \
	$$SRC_DIR/timerwheel.cpp \
	$$SRC_DIR/jwt.cpp \
	$$SRC_DIR/rtimer.cpp \
	$$SRC_DIR/logutil.cpp \
	$$SRC_DIR/uuidutil.cpp \
	$$SRC_DIR/zutil.cpp \
	$$SRC_DIR/zhttpmanager.cpp \
	$$SRC_DIR/zhttprequest.cpp \
	$$SRC_DIR/zwebsocket.cpp \
	$$SRC_DIR/zrpcmanager.cpp \
	$$SRC_DIR/zrpcrequest.cpp \
	$$SRC_DIR/statusreasons.cpp \
	$$SRC_DIR/cors.cpp \
	$$SRC_DIR/simplehttpserver.cpp \
	$$SRC_DIR/stats.cpp \
	$$SRC_DIR/statsmanager.cpp \
	$$SRC_DIR/settings.cpp
