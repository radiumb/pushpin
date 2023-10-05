/*
 * Copyright (C) 2014-2016 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#ifndef ZWEBSOCKET_H
#define ZWEBSOCKET_H

#include "websocket.h"

class ZhttpRequestPacket;
class ZhttpResponsePacket;
class ZhttpManager;

class ZWebSocket : public WebSocket
{
	Q_OBJECT

public:
	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	~ZWebSocket();

	Rid rid() const;
	void setIsTls(bool on);

	// reimplemented

	virtual QHostAddress peerAddress() const;

	virtual void setConnectHost(const QString &host);
	virtual void setConnectPort(int port);
	virtual void setIgnorePolicies(bool on);
	virtual void setTrustConnectHost(bool on);
	virtual void setIgnoreTlsErrors(bool on);

	virtual void start(const QUrl &uri, const HttpHeaders &headers);

	virtual void respondSuccess(const QByteArray &reason, const HttpHeaders &headers);
	virtual void respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body);

	virtual State state() const;
	virtual QUrl requestUri() const;
	virtual HttpHeaders requestHeaders() const;
	virtual int responseCode() const;
	virtual QByteArray responseReason() const;
	virtual HttpHeaders responseHeaders() const;
	virtual QByteArray responseBody() const;
	virtual int framesAvailable() const;
	virtual int peerCloseCode() const;
	virtual QString peerCloseReason() const;
	virtual ErrorCondition errorCondition() const;

	virtual void writeFrame(const Frame &frame);
	virtual Frame readFrame();
	virtual void close(int code = -1, const QString &reason = QString());

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZhttpManager;
	ZWebSocket(QObject *parent = 0);
	void setupClient(ZhttpManager *manager);
	bool setupServer(ZhttpManager *manager, const QByteArray &id, int seq, const ZhttpRequestPacket &packet);
	void startServer();
	bool isServer() const;
	QByteArray toAddress() const;
	int outSeqInc();
	void handle(const QByteArray &id, int seq, const ZhttpRequestPacket &packet);
	void handle(const QByteArray &id, int seq, const ZhttpResponsePacket &packet);
};

#endif
