/*
 * Copyright (C) 2012-2021 Fanout, Inc.
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

#include "zhttpmanager.h"

#include <assert.h>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QCryptographicHash>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "log.h"
#include "zutil.h"
#include "logutil.h"

#define OUT_HWM 100
#define IN_HWM 100
#define DEFAULT_HWM 101000
#define CLIENT_WAIT_TIME 0
#define CLIENT_STREAM_WAIT_TIME 500
#define SERVER_WAIT_TIME 500

#define PENDING_MAX 100

#define REFRESH_INTERVAL 1000
#define ZHTTP_EXPIRE 60000

#define ZHTTP_SHOULD_PROCESS (ZHTTP_EXPIRE * 3 / 4)
#define ZHTTP_REFRESH_BUCKETS (ZHTTP_SHOULD_PROCESS / REFRESH_INTERVAL)

// needs to match the peer
#define ZHTTP_IDS_MAX 128

// variable to count ws
static long wsRequestCount = 0, wsMessageSentCount = 0;
static long wsRpcAuthorCount = 0, wsRpcBabeCount = 0, wsRpcBeefyCount = 0, wsRpcChainCount = 0, wsRpcChildStateCount = 0;
static long wsRpcContractsCount = 0, wsRpcDevCount = 0, wsRpcEngineCount = 0, wsRpcEthCount = 0, wsRpcNetCount = 0;
static long wsRpcWeb3Count = 0, wsRpcGrandpaCount = 0, wsRpcMmrCount = 0, wsRpcOffchainCount = 0, wsRpcPaymentCount = 0;
static long wsRpcRpcCount = 0, wsRpcStateCount = 0, wsRpcSyncstateCount = 0, wsRpcSystemCount = 0, wsRpcSubscribeCount = 0;

// cache variables
struct CacheItem {
	int id;
	char hashVal[20];
	ZhttpResponsePacket priorPacket;
	QByteArray priorAddress;
	bool priorExistFlag;
	ZhttpResponsePacket responsePacket;
	QByteArray instanceAddress;
	time_t createdSeconds;
	bool cachedFlag;
};

QList<CacheItem> gCacheList;
static ZhttpResponsePacket gBackupPacket;
static QByteArray gBackupInstanceAddress;
static QString gBackupSubscription;

class ZhttpManager::Private : public QObject
{
	Q_OBJECT

public:
	enum SessionType
	{
		UnknownSession,
		HttpSession,
		WebSocketSession
	};

	class KeepAliveRegistration
	{
	public:
		SessionType type;
		union { ZhttpRequest *req; ZWebSocket *sock; } p;
		int refreshBucket;
	};

	ZhttpManager *q;
	QStringList client_out_specs;
	QStringList client_out_stream_specs;
	QStringList client_in_specs;
	QStringList client_req_specs;
	QStringList server_in_specs;
	QStringList server_in_stream_specs;
	QStringList server_out_specs;
	QZmq::Socket *client_out_sock;
	QZmq::Socket *client_out_stream_sock;
	QZmq::Socket *client_in_sock;
	QZmq::Socket *client_req_sock;
	QZmq::Socket *server_in_sock;
	QZmq::Socket *server_in_stream_sock;
	QZmq::Socket *server_out_sock;
	QZmq::Valve *client_in_valve;
	QZmq::Valve *server_in_valve;
	QZmq::Valve *server_in_stream_valve;
	QByteArray instanceId;
	int ipcFileMode;
	bool doBind;
	QHash<ZhttpRequest::Rid, ZhttpRequest*> clientReqsByRid;
	QHash<ZhttpRequest::Rid, ZhttpRequest*> serverReqsByRid;
	QList<ZhttpRequest*> serverPendingReqs;
	QHash<ZWebSocket::Rid, ZWebSocket*> clientSocksByRid;
	QHash<ZWebSocket::Rid, ZWebSocket*> serverSocksByRid;
	QList<ZWebSocket*> serverPendingSocks;
	QTimer *refreshTimer;
	QHash<void*, KeepAliveRegistration*> keepAliveRegistrations;
	QSet<KeepAliveRegistration*> sessionRefreshBuckets[ZHTTP_REFRESH_BUCKETS];
	int currentSessionRefreshBucket;
	int write_sync_flag;

	Private(ZhttpManager *_q) :
		QObject(_q),
		q(_q),
		client_out_sock(0),
		client_out_stream_sock(0),
		client_in_sock(0),
		client_req_sock(0),
		server_in_sock(0),
		server_in_stream_sock(0),
		server_out_sock(0),
		client_in_valve(0),
		server_in_valve(0),
		server_in_stream_valve(0),
		ipcFileMode(-1),
		doBind(false),
		currentSessionRefreshBucket(0),
		write_sync_flag(0)
	{
		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, &Private::refresh_timeout);
	}

	~Private()
	{
		while(!serverPendingReqs.isEmpty())
		{
			ZhttpRequest *req = serverPendingReqs.takeFirst();
			serverReqsByRid.remove(req->rid());
			delete req;
		}

		while(!serverPendingSocks.isEmpty())
		{
			ZWebSocket *sock = serverPendingSocks.takeFirst();
			serverSocksByRid.remove(sock->rid());
			delete sock;
		}

		assert(clientReqsByRid.isEmpty());
		assert(serverReqsByRid.isEmpty());
		assert(clientSocksByRid.isEmpty());
		assert(serverSocksByRid.isEmpty());
		assert(keepAliveRegistrations.isEmpty());

		refreshTimer->disconnect(this);
		refreshTimer->setParent(0);
		refreshTimer->deleteLater();
	}

	bool setupClientOut()
	{
		delete client_req_sock;
		delete client_out_sock;

		client_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
		connect(client_out_sock, &QZmq::Socket::messagesWritten, this, &Private::client_out_messagesWritten);

		client_out_sock->setHwm(OUT_HWM);
		client_out_sock->setShutdownWaitTime(CLIENT_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(client_out_sock, client_out_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	bool setupClientOutStream()
	{
		delete client_req_sock;
		delete client_out_stream_sock;

		client_out_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);
		connect(client_out_stream_sock, &QZmq::Socket::messagesWritten, this, &Private::client_out_stream_messagesWritten);

		client_out_stream_sock->setWriteQueueEnabled(false);
		client_out_stream_sock->setHwm(DEFAULT_HWM);
		client_out_stream_sock->setShutdownWaitTime(CLIENT_STREAM_WAIT_TIME);
		client_out_stream_sock->setImmediateEnabled(true);

		QString errorMessage;
		if(!ZUtil::setupSocket(client_out_stream_sock, client_out_stream_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	bool setupClientIn()
	{
		delete client_req_sock;
		delete client_in_sock;

		client_in_sock = new QZmq::Socket(QZmq::Socket::Sub, this);

		client_in_sock->setHwm(DEFAULT_HWM);
		client_in_sock->setShutdownWaitTime(0);
		client_in_sock->subscribe(instanceId + ' ');

		QString errorMessage;
		if(!ZUtil::setupSocket(client_in_sock, client_in_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		client_in_valve = new QZmq::Valve(client_in_sock, this);
		connect(client_in_valve, &QZmq::Valve::readyRead, this, &Private::client_in_readyRead);

		client_in_valve->open();

		return true;
	}

	bool setupClientReq()
	{
		delete client_out_sock;
		delete client_out_stream_sock;
		delete client_in_sock;

		client_req_sock = new QZmq::Socket(QZmq::Socket::Dealer, this);
		connect(client_req_sock, &QZmq::Socket::readyRead, this, &Private::client_req_readyRead);

		client_req_sock->setHwm(OUT_HWM);
		client_req_sock->setShutdownWaitTime(CLIENT_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(client_req_sock, client_req_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	bool setupServerIn()
	{
		delete server_in_sock;

		server_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);

		server_in_sock->setHwm(IN_HWM);

		QString errorMessage;
		if(!ZUtil::setupSocket(server_in_sock, server_in_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		server_in_valve = new QZmq::Valve(server_in_sock, this);
		connect(server_in_valve, &QZmq::Valve::readyRead, this, &Private::server_in_readyRead);

		server_in_valve->open();

		return true;
	}

	bool setupServerInStream()
	{
		delete server_in_stream_sock;

		server_in_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);

		server_in_stream_sock->setIdentity(instanceId);
		server_in_stream_sock->setHwm(DEFAULT_HWM);

		QString errorMessage;
		if(!ZUtil::setupSocket(server_in_stream_sock, server_in_stream_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		server_in_stream_valve = new QZmq::Valve(server_in_stream_sock, this);
		connect(server_in_stream_valve, &QZmq::Valve::readyRead, this, &Private::server_in_stream_readyRead);

		server_in_stream_valve->open();

		return true;
	}

	bool setupServerOut()
	{
		delete server_out_sock;

		server_out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
		connect(server_out_sock, &QZmq::Socket::messagesWritten, this, &Private::server_out_messagesWritten);

		server_out_sock->setWriteQueueEnabled(false);
		server_out_sock->setHwm(DEFAULT_HWM);
		server_out_sock->setShutdownWaitTime(SERVER_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(server_out_sock, server_out_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	int smallestSessionRefreshBucket()
	{
		int best = -1;
		int bestSize = 0;

		for(int n = 0; n < ZHTTP_REFRESH_BUCKETS; ++n)
		{
			if(best == -1 || sessionRefreshBuckets[n].count() < bestSize)
			{
				best = n;
				bestSize = sessionRefreshBuckets[n].count();
			}
		}

		return best;
	}

	void tryRespondCancel(SessionType type, const QByteArray &id, const ZhttpRequestPacket &packet)
	{
		assert(!packet.from.isEmpty());

		// if this was not an error packet, send cancel
		if(packet.type != ZhttpRequestPacket::Error && packet.type != ZhttpRequestPacket::Cancel)
		{
			ZhttpResponsePacket out;
			out.from = instanceId;
			out.ids += ZhttpResponsePacket::Id(id);
			out.type = ZhttpResponsePacket::Cancel;
			write(type, out, packet.from);
		}
	}

	void write(SessionType type, const ZhttpRequestPacket &packet)
	{
		assert(client_out_sock || client_req_sock);
		const char *logprefix = logPrefixForType(type);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(vpacket);

		if(client_out_sock)
		{
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client: OUT", logprefix);

			client_out_sock->write(QList<QByteArray>() << buf);
		}
		else
		{
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client req: OUT", logprefix);

			client_req_sock->write(QList<QByteArray>() << QByteArray() << buf);
		}
	}

	void write(SessionType type, const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
	{
		assert(client_out_stream_sock);
		const char *logprefix = logPrefixForType(type);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client: OUT %s", logprefix, instanceAddress.data());

		QList<QByteArray> msg;
		msg += instanceAddress;
		msg += QByteArray();
		msg += buf;
		client_out_stream_sock->write(msg);
	}

	void write(SessionType type, const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
	{
		assert(server_out_sock);
		const char *logprefix = logPrefixForType(type);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = instanceAddress + " T" + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s server: OUT %s", logprefix, instanceAddress.data());

		// Count (ws messages sent)
		if (type == 2 && packet.type == 0 && packet.credits == -1)
		{
			wsMessageSentCount++;
			// Write to shared memory
			key_t key = ftok("shmfile",65);
			int shmid = shmget(key,0,0666|IPC_CREAT);
			char *str = (char*) shmat(shmid,(void*)0,0);
			memcpy(&str[8], (char *)&wsMessageSentCount, 4);
			shmdt(str);

			// Cache the repsonse packet
			// parse.
			QVariantHash hdata = vpacket.toHash();
			// parse body as JSON string
			QJsonParseError error;
			QJsonDocument jsonDoc = QJsonDocument::fromJson(hdata.value("body").toByteArray(), &error);
			if(error.error != QJsonParseError::NoError || !jsonDoc.isObject())
				goto RESPONSE_WRITE;

			QVariantMap jsonData = jsonDoc.object().toVariantMap();
			// id
			if(!jsonData.contains("id"))// || jsonData["id"].type() != QVariant::LongLong)
			{
				// get subscription for backup
				if(jsonData.contains("params") && jsonData["params"].type() == QVariant::Map)
				{
					QVariantMap jsonBackupData = jsonData["params"].toMap();
					if(jsonBackupData.contains("subscription") && jsonBackupData["subscription"].type() == QVariant::String)
					{
						// Backup
						gBackupSubscription = jsonBackupData["subscription"].toString();
						gBackupPacket = packet;
						gBackupInstanceAddress = instanceAddress;
					}
				}
				goto RESPONSE_WRITE;
			}
			int jId = jsonData["id"].toInt();
			int cacheListCount = gCacheList.count();
			for (int i = 0; i < cacheListCount; i++)
			{
				if ((gCacheList[i].id == jId) && (gCacheList[i].cachedFlag == false))
				{
					// check whether the prior packet is valid or not
					if(jsonData.contains("result") && jsonData["result"].type() == QVariant::String)
					{
						QString jResult = jsonData["result"].toString();
						if (jResult == gBackupSubscription)
						{
							gCacheList[i].priorPacket = gBackupPacket;
							gCacheList[i].priorAddress = gBackupInstanceAddress;
							gCacheList[i].priorExistFlag = true;
						}
					}

					gCacheList[i].responsePacket = packet;
					gCacheList[i].instanceAddress = instanceAddress;
					gCacheList[i].cachedFlag = true;
					log_debug("[CACHE] Added Cache content for method id %d", jId);
					break;
				}
			}
		}
RESPONSE_WRITE:

		while (write_sync_flag == 1)
		{
			usleep(1000);
		}

		write_sync_flag = 1;
		server_out_sock->write(QList<QByteArray>() << buf);
		write_sync_flag = 0;
	}

	void write_cache(SessionType type, const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
	{
		assert(server_out_sock);
		const char *logprefix = logPrefixForType(type);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = instanceAddress + " T" + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s server: OUT %s", logprefix, instanceAddress.data());

		// Count (ws messages sent)
		if (type == 2 && packet.type == 0 && packet.credits == -1)
		{
			wsMessageSentCount++;
			// Write to shared memory
			key_t key = ftok("shmfile",65);
			int shmid = shmget(key,0,0666|IPC_CREAT);
			char *str = (char*) shmat(shmid,(void*)0,0);
			memcpy(&str[8], (char *)&wsMessageSentCount, 4);
			shmdt(str);
		}

		while (write_sync_flag == 1)
		{
			usleep(1000);
		}

		write_sync_flag = 1;
		server_out_sock->write(QList<QByteArray>() << buf);
		write_sync_flag = 0;
	}

	static const char *logPrefixForType(SessionType type)
	{
		switch(type)
		{
			case HttpSession: return "zhttp";
			case WebSocketSession: return "zws";
			default: return "zhttp/zws";
		}
	}

	void registerKeepAlive(void *p, SessionType type)
	{
		if(keepAliveRegistrations.contains(p))
			return;

		KeepAliveRegistration *r = new KeepAliveRegistration;
		r->type = type;
		if(type == HttpSession)
			r->p.req = (ZhttpRequest *)p;
		else // WebSocketSession
			r->p.sock = (ZWebSocket *)p;

		keepAliveRegistrations.insert(p, r);

		r->refreshBucket = smallestSessionRefreshBucket();
		sessionRefreshBuckets[r->refreshBucket] += r;

		setupKeepAlive();
	}

	void unregisterKeepAlive(void *p)
	{
		KeepAliveRegistration *r = keepAliveRegistrations.value(p);
		if(!r)
			return;

		sessionRefreshBuckets[r->refreshBucket].remove(r);
		keepAliveRegistrations.remove(p);
		delete r;

		setupKeepAlive();
	}

	void setupKeepAlive()
	{
		if(!keepAliveRegistrations.isEmpty())
		{
			if(!refreshTimer->isActive())
				refreshTimer->start(REFRESH_INTERVAL);
		}
		else
			refreshTimer->stop();
	}

	void writeKeepAlive(SessionType type, const QList<ZhttpRequestPacket::Id> &ids, const QByteArray &zhttpAddress)
	{
		ZhttpRequestPacket zreq;
		zreq.from = instanceId;
		zreq.ids = ids;
		zreq.type = ZhttpRequestPacket::KeepAlive;
		write(type, zreq, zhttpAddress);
	}

	void writeKeepAlive(SessionType type, const QList<ZhttpResponsePacket::Id> &ids, const QByteArray &zhttpAddress)
	{
		ZhttpResponsePacket zresp;
		zresp.from = instanceId;
		zresp.ids = ids;
		zresp.type = ZhttpResponsePacket::KeepAlive;
		write(type, zresp, zhttpAddress);
	}

public slots:
	void client_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void client_out_stream_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void client_in_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 1)
		{
			log_warning("zhttp/zws client: received message with parts != 1, skipping");
			return;
		}

		int at = msg[0].indexOf(' ');
		if(at == -1)
		{
			log_warning("zhttp/zws client: received message with invalid format, skipping");
			return;
		}

		QByteArray receiver = msg[0].mid(0, at);
		QByteArray dataRaw = msg[0].mid(at + 1);
		if(dataRaw.length() < 1 || dataRaw[0] != 'T')
		{
			log_warning("zhttp/zws client: received message with invalid format (missing type), skipping");
			return;
		}

		QVariant data = TnetString::toVariant(dataRaw.mid(1));
		if(data.isNull())
		{
			log_warning("zhttp/zws client: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws client: IN %s", receiver.data());

		ZhttpResponsePacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zhttp/zws client: received message with invalid format (parse failed), skipping");
			return;
		}

		QPointer<QObject> self = this;

		foreach(const ZhttpResponsePacket::Id &id, p.ids)
		{
			// is this for a websocket?
			ZWebSocket *sock = clientSocksByRid.value(ZWebSocket::Rid(instanceId, id.id));
			if(sock)
			{
				sock->handle(id.id, id.seq, p);
				if(!self)
					return;

				continue;
			}

			// is this for an http request?
			ZhttpRequest *req = clientReqsByRid.value(ZhttpRequest::Rid(instanceId, id.id));
			if(req)
			{
				req->handle(id.id, id.seq, p);
				if(!self)
					return;

				continue;
			}

			log_debug("zhttp/zws client: received message for unknown request id, skipping");
		}
	}

	void server_in_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 1)
		{
			log_warning("zhttp/zws server: received message with parts != 1, skipping");
			return;
		}

		if(msg[0].length() < 1 || msg[0][0] != 'T')
		{
			log_warning("zhttp/zws server: received message with invalid format (missing type), skipping");
			return;
		}

		QVariant data = TnetString::toVariant(msg[0].mid(1));
		if(data.isNull())
		{
			log_warning("zhttp/zws server: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws server: IN");

		ZhttpRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zhttp/zws server: received message with invalid format (parse failed), skipping");
			return;
		}

		if(p.from.isEmpty())
		{
			log_warning("zhttp/zws server: received message without from address, skipping");
			return;
		}

		if(p.ids.count() != 1)
		{
			log_warning("zhttp/zws server: received initial message with multiple ids, skipping");
			return;
		}

		const ZhttpRequestPacket::Id &id = p.ids.first();

		if(p.uri.scheme() == "wss" || p.uri.scheme() == "ws")
		{
			ZWebSocket::Rid rid(p.from, id.id);

			ZWebSocket *sock = serverSocksByRid.value(rid);
			if(sock)
			{
				log_warning("zws server: received message for existing request id, canceling");
				tryRespondCancel(WebSocketSession, id.id, p);
				return;
			}

			sock = new ZWebSocket;
			if(!sock->setupServer(q, id.id, id.seq, p))
			{
				delete sock;
				return;
			}

			serverSocksByRid.insert(rid, sock);
			serverPendingSocks += sock;

			if(serverPendingReqs.count() + serverPendingSocks.count() >= PENDING_MAX)
				server_in_valve->close();

			emit q->socketReady();
		}
		else if(p.uri.scheme() == "https" || p.uri.scheme() == "http")
		{
			ZhttpRequest::Rid rid(p.from, id.id);

			ZhttpRequest *req = serverReqsByRid.value(rid);
			if(req)
			{
				log_warning("zhttp server: received message for existing request id, canceling");
				tryRespondCancel(HttpSession, id.id, p);
				return;
			}

			req = new ZhttpRequest;
			if(!req->setupServer(q, id.id, id.seq, p))
			{
				delete req;
				return;
			}

			serverReqsByRid.insert(rid, req);
			serverPendingReqs += req;

			if(serverPendingReqs.count() + serverPendingSocks.count() >= PENDING_MAX)
				server_in_valve->close();

			emit q->requestReady();
		}
		else
		{
			log_debug("zhttp/zws server: rejecting unsupported scheme: %s", qPrintable(p.uri.scheme()));
			tryRespondCancel(UnknownSession, id.id, p);
			return;
		}
	}

	void client_req_readyRead()
	{
		QPointer<QObject> self = this;

		while(client_req_sock->canRead())
		{
			QList<QByteArray> msg = client_req_sock->read();
			if(msg.count() != 2)
			{
				log_warning("zhttp/zws client req: received message with parts != 2, skipping");
				continue;
			}

			QByteArray dataRaw = msg[1];
			if(dataRaw.length() < 1 || dataRaw[0] != 'T')
			{
				log_warning("zhttp/zws client req: received message with invalid format (missing type), skipping");
				continue;
			}

			QVariant data = TnetString::toVariant(dataRaw.mid(1));
			if(data.isNull())
			{
				log_warning("zhttp/zws client req: received message with invalid format (tnetstring parse failed), skipping");
				continue;
			}

			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws client req: IN");

			ZhttpResponsePacket p;
			if(!p.fromVariant(data))
			{
				log_warning("zhttp/zws client req: received message with invalid format (parse failed), skipping");
				continue;
			}

			if(p.ids.count() != 1)
			{
				log_warning("zhttp/zws client req: received message with multiple ids, skipping");
				return;
			}

			const ZhttpResponsePacket::Id &id = p.ids.first();

			ZhttpRequest *req = clientReqsByRid.value(ZhttpRequest::Rid(instanceId, id.id));
			if(req)
			{
				req->handle(id.id, id.seq, p);
				if(!self)
					return;

				continue;
			}

			log_debug("zhttp/zws client req: received message for unknown request id");

			// NOTE: we don't respond with a cancel message in req mode
		}
	}

	void server_in_stream_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 3)
		{
			log_warning("zhttp/zws server: received message with parts != 3, skipping");
			return;
		}

		if(msg[2].length() < 1 || msg[2][0] != 'T')
		{
			log_warning("zhttp/zws server: received message with invalid format (missing type), skipping");
			return;
		}

		QVariant data = TnetString::toVariant(msg[2].mid(1));
		if(data.isNull())
		{
			log_warning("zhttp/zws server: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws server: IN stream");

		ZhttpRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zhttp/zws server: received message with invalid format (parse failed), skipping");
			return;
		}

		QPointer<QObject> self = this;

		foreach(const ZhttpRequestPacket::Id &id, p.ids)
		{
			// is this for a websocket?
			ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(p.from, id.id));
			if(sock)
			{
				if (p.type == 0)
				{
					// parse JSON-RPC 
					{
						// convert to string
						QVariantHash hdata = data.toHash();
						// parse body as JSON string
						QJsonParseError error;
						QJsonDocument jsonDoc = QJsonDocument::fromJson(hdata.value("body").toByteArray(), &error);
						if(error.error != QJsonParseError::NoError || !jsonDoc.isObject())
							goto SOCK_HANDLE;

						QVariantMap jsonData = jsonDoc.object().toVariantMap();
						if(!jsonData.contains("method") || jsonData["method"].type() != QVariant::String)
							goto SOCK_HANDLE;

						QString jMethod = jsonData["method"].toString();
						char methodStr[256];
						strncpy(methodStr, qPrintable(jMethod.toLower()), jMethod.length()>255?255:jMethod.length());

						if (!memcmp(methodStr, "author_", 7)) {
							wsRpcAuthorCount++;
							if (!memcmp(&methodStr[7], "submitandwatchextrinsic", 23)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "babe_", 5)) {
							wsRpcBabeCount++;
							if (!memcmp(&methodStr[5], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "beefy_", 6)) {
							wsRpcBeefyCount++;
							if (!memcmp(&methodStr[6], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "chain_", 6)) {
							wsRpcChainCount++;
							if (!memcmp(&methodStr[6], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "childstate_", 11)) {
							wsRpcChildStateCount++;
							if (!memcmp(&methodStr[11], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "contracts_", 10)) {
							wsRpcContractsCount++;
							if (!memcmp(&methodStr[10], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "dev_", 4)) {
							wsRpcDevCount++;
							if (!memcmp(&methodStr[4], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "engine_", 7)) {
							wsRpcEngineCount++;
							if (!memcmp(&methodStr[7], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "eth_", 4)) {
							wsRpcEthCount++;
							if (!memcmp(&methodStr[4], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "net_", 4)) {
							wsRpcNetCount++;
							if (!memcmp(&methodStr[4], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "web3_", 5)) {
							wsRpcWeb3Count++;
							if (!memcmp(&methodStr[5], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "grandpa_", 8)) {
							wsRpcGrandpaCount++;
							if (!memcmp(&methodStr[8], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "mmr_", 4)) {
							wsRpcMmrCount++;
							if (!memcmp(&methodStr[4], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "offchain_", 9)) {
							wsRpcOffchainCount++;
							if (!memcmp(&methodStr[9], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "payment_", 8)) {
							wsRpcPaymentCount++;
							if (!memcmp(&methodStr[8], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "rpc_", 4)) {
							wsRpcRpcCount++;
							if (!memcmp(&methodStr[4], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "state_", 6)) {
							wsRpcStateCount++;
							if (!memcmp(&methodStr[6], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "sync_state_", 11)) {
							wsRpcSyncstateCount++;
							if (!memcmp(&methodStr[11], "subscribe", 9)) wsRpcSubscribeCount++;
						} else if (!memcmp(methodStr, "system_", 7)) {
							wsRpcSystemCount++;
							if (!memcmp(&methodStr[7], "subscribe", 9)) wsRpcSubscribeCount++;
						}

						// read shared memory
						// Count WS request
						wsRequestCount++;
						// Write to shared memory
						key_t shm_key = ftok("shmfile",65);
						int shm_id = shmget(shm_key,0,0666|IPC_CREAT);
						char *shm_str = (char*) shmat(shm_id,(void*)0,0);
						memcpy(&shm_str[0], (char *)&wsRequestCount, 4);
						memcpy(&shm_str[20], (char *)&wsRpcAuthorCount, 4);
						memcpy(&shm_str[24], (char *)&wsRpcBabeCount, 4);
						memcpy(&shm_str[28], (char *)&wsRpcBeefyCount, 4);
						memcpy(&shm_str[32], (char *)&wsRpcChainCount, 4);
						memcpy(&shm_str[36], (char *)&wsRpcChildStateCount, 4);
						memcpy(&shm_str[40], (char *)&wsRpcContractsCount, 4);
						memcpy(&shm_str[44], (char *)&wsRpcDevCount, 4);
						memcpy(&shm_str[48], (char *)&wsRpcEngineCount, 4);
						memcpy(&shm_str[52], (char *)&wsRpcEthCount, 4);
						memcpy(&shm_str[56], (char *)&wsRpcNetCount, 4);
						memcpy(&shm_str[60], (char *)&wsRpcWeb3Count, 4);
						memcpy(&shm_str[64], (char *)&wsRpcGrandpaCount, 4);
						memcpy(&shm_str[68], (char *)&wsRpcMmrCount, 4);
						memcpy(&shm_str[72], (char *)&wsRpcOffchainCount, 4);
						memcpy(&shm_str[76], (char *)&wsRpcPaymentCount, 4);
						memcpy(&shm_str[80], (char *)&wsRpcRpcCount, 4);
						memcpy(&shm_str[84], (char *)&wsRpcStateCount, 4);
						memcpy(&shm_str[88], (char *)&wsRpcSyncstateCount, 4);
						memcpy(&shm_str[92], (char *)&wsRpcSystemCount, 4);
						memcpy(&shm_str[96], (char *)&wsRpcSubscribeCount, 4);

						// Group
						int shm_read_count = 100;
						long groupByteCount = *(long *)&shm_str[shm_read_count]; shm_read_count += 4;
						long groupCount = *(long *)&shm_str[shm_read_count]; shm_read_count += 4;
						QString methodName = QString(methodStr);
						QByteArray methodNameHashByteArray = QCryptographicHash::hash(methodName.toLower().toUtf8(),QCryptographicHash::Sha1);

						char methodNameHash[20];
						memcpy(methodNameHash, methodNameHashByteArray.data(), 20);
						
						int gCnt = (int)groupCount;
						for (int i = 0; i < gCnt; i++)
						{
							long methodCount = *(long *)&shm_str[shm_read_count]; shm_read_count += 4;
							int mCnt = (int)methodCount;
							char groupName[256];
							memcpy(groupName, &shm_str[shm_read_count], 256); shm_read_count += 256;	
							long eventCount = *(long *)&shm_str[shm_read_count]; shm_read_count += 4;

							int shm_write_point = shm_read_count - 4;								
							for (int j = 0; j < mCnt; j++)
							{
								char groupMethodNameHash[20];
								memcpy(groupMethodNameHash, &shm_str[shm_read_count], 20); shm_read_count += 20;	
								if (!memcmp(groupMethodNameHash, methodNameHash, 20))
								{
									eventCount++;
									memcpy(&shm_str[shm_write_point], (char *)&eventCount, 4);
									shm_read_count += 20*(mCnt-j-1);
									break;
								}
							}
								
						}
						
						// Cache
						if(!jsonData.contains("id"))// || jsonData["id"].type() != QVariant::LongLong)
							goto SOCK_HANDLE;
						int jId = jsonData["id"].toInt();
						QString jParams(methodStr);
						if (jsonData.contains("params"))
						{
							if (jsonData["params"].type() == QVariant::List)
							{
								for (QVariant m : jsonData["params"].toList())
								{
									if (m.type() == QVariant::String)
									{
										jParams += m.toString();
									}
									else if (m.type() == QVariant::List)
									{
										for (QVariant n : m.toList())
										{
											if (n.type() == QVariant::String)
												jParams += n.toString();
										}
									}
								}
							}
							else if (jsonData["params"].type() == QVariant::String)
							{
								jParams += jsonData["params"].toString();
							}
							
						}
						
						QByteArray paramsHashByteArray = QCryptographicHash::hash(jParams.toUtf8(),QCryptographicHash::Sha1);
						char paramsHash[20];
						memcpy(paramsHash, paramsHashByteArray.data(), 20);
						
						// read shm file 
						shm_read_count = 100 + groupByteCount;
						int cacheByteCount = *(long *)&shm_str[shm_read_count]; shm_read_count += 4;
						int cacheTimeoutSeconds = *(long *)&shm_str[shm_read_count]; shm_read_count += 4;
						int cacheMethodCount = *(long *)&shm_str[shm_read_count]; shm_read_count += 4;

						// delete old cache items
						int cacheListCount;
						time_t currSeconds = time(NULL);
DELETE_OLD_CACHE_ITEMS:
						cacheListCount = gCacheList.count();
						for (int i = 0; i < cacheListCount; i++)
						{
							int diff = (int)(currSeconds - gCacheList[i].createdSeconds);
							if (diff > cacheTimeoutSeconds)
							{
								gCacheList.removeAt(i);
								goto DELETE_OLD_CACHE_ITEMS;
							}
						}
						
						cacheListCount = gCacheList.count();

						for (int i = 0; i < cacheMethodCount; i++)
						{
							char cacheMethodNameHash[20];
							memcpy(cacheMethodNameHash, &shm_str[shm_read_count], 20); shm_read_count += 20;

							if (!memcmp(cacheMethodNameHash, methodNameHash, 20))
							{
								for (int j = 0; j < cacheListCount; j++)
								{
									if (!memcmp(gCacheList[j].hashVal, paramsHash, 20))
									{
										if (gCacheList[j].cachedFlag == true)
										{
											ZhttpResponsePacket packet = gCacheList[j].responsePacket;
											QByteArray instanceAddress = gCacheList[j].instanceAddress;

											// first, send credit packet
											ZhttpResponsePacket creditPacket = packet;
											creditPacket.ids[0].id = id.id;
											creditPacket.ids[0].seq = -1;
											creditPacket.credits = hdata.value("body").toByteArray().size();
											creditPacket.type = ZhttpResponsePacket::Type::Credit;
											creditPacket.body.clear();
											creditPacket.contentType.clear();
											creditPacket.from = packet.from;
											QByteArray creditAddress = gCacheList[j].instanceAddress;
											write_cache(SessionType::WebSocketSession, creditPacket, creditAddress);

											// send prior packet
											if (gCacheList[j].priorExistFlag == true)
											{
												gBackupPacket.ids[0].id = id.id;
												gBackupPacket.ids[0].seq = -1;
												write_cache(SessionType::WebSocketSession, gBackupPacket, gBackupInstanceAddress);
											}

											// replace id str
											char oldIdStr[64], newIdStr[64];
											qsnprintf(oldIdStr, 64, "\"id\":%d", gCacheList[j].id);
											qsnprintf(newIdStr, 64, "\"id\":%d", jId);
											packet.body.replace(QByteArray(oldIdStr), QByteArray(newIdStr));
											packet.ids[0].id = id.id;
											packet.ids[0].seq = -1;
											write_cache(SessionType::WebSocketSession, packet, instanceAddress);
											p.type = ZhttpRequestPacket::KeepAlive;
											log_debug("[CACHE] Replied with Cache content for method \"%s\"", methodStr);
										}
										goto SOCK_HANDLE;
									}
								}

								// create new cache item
								struct CacheItem cacheItem;
								cacheItem.id = jId;
								cacheItem.cachedFlag = false;
								cacheItem.priorExistFlag = false;
								cacheItem.createdSeconds = time(NULL);
								memcpy(cacheItem.hashVal, paramsHash, 20);
								gCacheList.append(cacheItem);
								log_debug("[CACHE] Registered Cache for method \"%s\"", methodStr);

								break;
							}
							
						}
						shmdt(shm_str);
					}
				}
SOCK_HANDLE:				
				sock->handle(id.id, id.seq, p);
				if(!self)
					return;

				continue;
			}

			// is this for an http request?
			ZhttpRequest *req = serverReqsByRid.value(ZhttpRequest::Rid(p.from, id.id));
			if(req)
			{
				req->handle(id.id, id.seq, p);
				if(!self)
					return;

				continue;
			}

			log_debug("zhttp/zws server: received message for unknown request id, skipping");
		}
	}

	void server_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void refresh_timeout()
	{
		QHash<QByteArray, QList<KeepAliveRegistration*> > clientSessionsBySender[2]; // index corresponds to type
		QHash<QByteArray, QList<KeepAliveRegistration*> > serverSessionsBySender[2]; // index corresponds to type

		// process the current bucket
		const QSet<KeepAliveRegistration*> &bucket = sessionRefreshBuckets[currentSessionRefreshBucket];
		foreach(KeepAliveRegistration *r, bucket)
		{
			QPair<QByteArray, QByteArray> rid;
			bool isServer;
			if(r->type == HttpSession)
			{
				rid = r->p.req->rid();
				isServer = r->p.req->isServer();
			}
			else // WebSocketSession
			{
				rid = r->p.sock->rid();
				isServer = r->p.sock->isServer();
			}

			QByteArray sender;
			if(isServer)
			{
				sender = rid.first;
			}
			else
			{
				if(r->type == HttpSession)
					sender = r->p.req->toAddress();
				else // WebSocketSession
					sender = r->p.sock->toAddress();
			}

			assert(!sender.isEmpty());

			QHash<QByteArray, QList<KeepAliveRegistration*> > &sessionsBySender = (isServer ? serverSessionsBySender[r->type - 1] : clientSessionsBySender[r->type - 1]);

			if(!sessionsBySender.contains(sender))
				sessionsBySender.insert(sender, QList<KeepAliveRegistration*>());

			QList<KeepAliveRegistration*> &sessions = sessionsBySender[sender];
			sessions += r;

			// if we're at max, send out now
			if(sessions.count() >= ZHTTP_IDS_MAX)
			{
				if(isServer)
				{
					QList<ZhttpResponsePacket::Id> ids;
					foreach(KeepAliveRegistration *i, sessions)
					{
						assert(i->type == r->type);
						if(r->type == HttpSession)
							ids += ZhttpResponsePacket::Id(i->p.req->rid().second, i->p.req->outSeqInc());
						else // WebSocketSession
							ids += ZhttpResponsePacket::Id(i->p.sock->rid().second, i->p.sock->outSeqInc());
					}

					writeKeepAlive(r->type, ids, sender);
				}
				else
				{
					QList<ZhttpRequestPacket::Id> ids;
					foreach(KeepAliveRegistration *i, sessions)
					{
						assert(i->type == r->type);
						if(r->type == HttpSession)
							ids += ZhttpRequestPacket::Id(i->p.req->rid().second, i->p.req->outSeqInc());
						else // WebSocketSession
							ids += ZhttpRequestPacket::Id(i->p.sock->rid().second, i->p.sock->outSeqInc());
					}

					writeKeepAlive(r->type, ids, sender);
				}

				sessions.clear();
				sessionsBySender.remove(sender);
			}
		}

		// send last packets
		for(int n = 0; n < 2; ++n)
		{
			SessionType type = (SessionType)(n + 1);

			{
				QHashIterator<QByteArray, QList<KeepAliveRegistration*> > sit(clientSessionsBySender[n]);
				while(sit.hasNext())
				{
					sit.next();
					const QByteArray &sender = sit.key();
					const QList<KeepAliveRegistration*> &sessions = sit.value();

					if(!sessions.isEmpty())
					{
						QList<ZhttpRequestPacket::Id> ids;
						foreach(KeepAliveRegistration *i, sessions)
						{
							assert(i->type == type);
							if(type == HttpSession)
								ids += ZhttpRequestPacket::Id(i->p.req->rid().second, i->p.req->outSeqInc());
							else // WebSocketSession
								ids += ZhttpRequestPacket::Id(i->p.sock->rid().second, i->p.sock->outSeqInc());
						}

						writeKeepAlive(type, ids, sender);
					}
				}
			}

			{
				QHashIterator<QByteArray, QList<KeepAliveRegistration*> > sit(serverSessionsBySender[n]);
				while(sit.hasNext())
				{
					sit.next();
					const QByteArray &sender = sit.key();
					const QList<KeepAliveRegistration*> &sessions = sit.value();

					if(!sessions.isEmpty())
					{
						QList<ZhttpResponsePacket::Id> ids;
						foreach(KeepAliveRegistration *i, sessions)
						{
							assert(i->type == type);
							if(type == HttpSession)
								ids += ZhttpResponsePacket::Id(i->p.req->rid().second, i->p.req->outSeqInc());
							else // WebSocketSession
								ids += ZhttpResponsePacket::Id(i->p.sock->rid().second, i->p.sock->outSeqInc());
						}

						writeKeepAlive(type, ids, sender);
					}
				}
			}
		}

		++currentSessionRefreshBucket;
		if(currentSessionRefreshBucket >= ZHTTP_REFRESH_BUCKETS)
			currentSessionRefreshBucket = 0;
	}
};

ZhttpManager::ZhttpManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZhttpManager::~ZhttpManager()
{
	delete d;
}

int ZhttpManager::connectionCount() const
{
	int total = 0;
	total += d->clientReqsByRid.count();
	total += d->serverReqsByRid.count();
	total += d->clientSocksByRid.count();
	total += d->serverSocksByRid.count();
	return total;
}

bool ZhttpManager::clientUsesReq() const
{
	return (!d->client_out_sock && d->client_req_sock);
}

ZhttpRequest *ZhttpManager::serverRequestByRid(const ZhttpRequest::Rid &rid) const
{
	return d->serverReqsByRid.value(rid);
}

QByteArray ZhttpManager::instanceId() const
{
	return d->instanceId;
}

void ZhttpManager::setInstanceId(const QByteArray &id)
{
	d->instanceId = id;
}

void ZhttpManager::setIpcFileMode(int mode)
{
	d->ipcFileMode = mode;
}

void ZhttpManager::setBind(bool enable)
{
	d->doBind = enable;
}

bool ZhttpManager::setClientOutSpecs(const QStringList &specs)
{
	d->client_out_specs = specs;
	return d->setupClientOut();
}

bool ZhttpManager::setClientOutStreamSpecs(const QStringList &specs)
{
	d->client_out_stream_specs = specs;
	return d->setupClientOutStream();
}

bool ZhttpManager::setClientInSpecs(const QStringList &specs)
{
	d->client_in_specs = specs;
	return d->setupClientIn();
}

bool ZhttpManager::setClientReqSpecs(const QStringList &specs)
{
	d->client_req_specs = specs;
	return d->setupClientReq();
}

bool ZhttpManager::setServerInSpecs(const QStringList &specs)
{
	d->server_in_specs = specs;
	return d->setupServerIn();
}

bool ZhttpManager::setServerInStreamSpecs(const QStringList &specs)
{
	d->server_in_stream_specs = specs;
	return d->setupServerInStream();
}

bool ZhttpManager::setServerOutSpecs(const QStringList &specs)
{
	d->server_out_specs = specs;
	return d->setupServerOut();
}

ZhttpRequest *ZhttpManager::createRequest()
{
	ZhttpRequest *req = new ZhttpRequest;
	req->setupClient(this, d->client_req_sock ? true : false);
	return req;
}

ZhttpRequest *ZhttpManager::takeNextRequest()
{
	ZhttpRequest *req = 0;

	while(!req)
	{
		if(d->serverPendingReqs.isEmpty())
			return 0;

		req = d->serverPendingReqs.takeFirst();
		if(!d->serverReqsByRid.contains(req->rid()))
		{
			// this means the object was a zombie. clean up and take next
			delete req;
			req = 0;
			continue;
		}

		d->server_in_valve->open();
	}

	req->startServer();
	return req;
}

ZWebSocket *ZhttpManager::createSocket()
{
	// websockets not allowed in req mode
	assert(!d->client_req_sock);

	ZWebSocket *sock = new ZWebSocket;
	sock->setupClient(this);
	return sock;
}

ZWebSocket *ZhttpManager::takeNextSocket()
{
	ZWebSocket *sock = 0;

	while(!sock)
	{
		if(d->serverPendingSocks.isEmpty())
			return 0;

		sock = d->serverPendingSocks.takeFirst();
		if(!d->serverSocksByRid.contains(sock->rid()))
		{
			// this means the object was a zombie. clean up and take next
			delete sock;
			sock = 0;
			continue;
		}

		d->server_in_valve->open();
	}

	sock->startServer();
	return sock;
}

ZhttpRequest *ZhttpManager::createRequestFromState(const ZhttpRequest::ServerState &state)
{
	ZhttpRequest *req = new ZhttpRequest;
	req->setupServer(this, state);
	return req;
}

void ZhttpManager::link(ZhttpRequest *req)
{
	if(req->isServer())
		d->serverReqsByRid.insert(req->rid(), req);
	else
		d->clientReqsByRid.insert(req->rid(), req);
}

void ZhttpManager::unlink(ZhttpRequest *req)
{
	if(req->isServer())
		d->serverReqsByRid.remove(req->rid());
	else
		d->clientReqsByRid.remove(req->rid());
}

void ZhttpManager::link(ZWebSocket *sock)
{
	if(sock->isServer())
		d->serverSocksByRid.insert(sock->rid(), sock);
	else
		d->clientSocksByRid.insert(sock->rid(), sock);
}

void ZhttpManager::unlink(ZWebSocket *sock)
{
	if(sock->isServer())
		d->serverSocksByRid.remove(sock->rid());
	else
		d->clientSocksByRid.remove(sock->rid());
}

bool ZhttpManager::canWriteImmediately() const
{
	assert(d->client_out_sock || d->client_req_sock);

	if(d->client_out_sock)
		return d->client_out_sock->canWriteImmediately();
	else
		return d->client_req_sock->canWriteImmediately();
}

void ZhttpManager::writeHttp(const ZhttpRequestPacket &packet)
{
	d->write(Private::HttpSession, packet);
}

void ZhttpManager::writeHttp(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::HttpSession, packet, instanceAddress);
}

void ZhttpManager::writeHttp(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::HttpSession, packet, instanceAddress);
}

void ZhttpManager::writeWs(const ZhttpRequestPacket &packet)
{
	d->write(Private::WebSocketSession, packet);
}

void ZhttpManager::writeWs(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::WebSocketSession, packet, instanceAddress);
}

void ZhttpManager::writeWs(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::WebSocketSession, packet, instanceAddress);
}

void ZhttpManager::registerKeepAlive(ZhttpRequest *req)
{
	d->registerKeepAlive(req, Private::HttpSession);
}

void ZhttpManager::unregisterKeepAlive(ZhttpRequest *req)
{
	d->unregisterKeepAlive(req);
}

void ZhttpManager::registerKeepAlive(ZWebSocket *sock)
{
	d->registerKeepAlive(sock, Private::WebSocketSession);
}

void ZhttpManager::unregisterKeepAlive(ZWebSocket *sock)
{
	d->unregisterKeepAlive(sock);
}

#include "zhttpmanager.moc"