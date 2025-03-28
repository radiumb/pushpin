/*
 * Copyright (C) 2012-2021 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $FANOUT_END_LICENSE$
 */

#include "zhttpmanager.h"

#include <assert.h>
#include <QStringList>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QTimer>
#include <functional>
#include <QtConcurrent>
#include <QThread>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "log.h"
#include "zutil.h"
#include "logutil.h"
#include "timer.h"
#include "cacheutil.h"

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

// max length of one packet in log
#define DEBUG_LOG_MAX_LENGTH	1024

#define CACHE_INTERVAL 1000

#define PING_INTERVAL	20

/////////////////////////////////////////////////////////////////////////////////////
// cache data structure

bool gCacheEnable = false;
QStringList gHttpBackendUrlList;
QStringList gWsBackendUrlList;

QList<ClientItem> gWsCacheClientList;
ZhttpResponsePacket gWsInitResponsePacket;
QMap<QByteArray, ClientItem> gWsClientMap;
QMap<QByteArray, ClientItem> gHttpClientMap;

QList<CacheKeyItem> gCacheKeyItemList;
QString gMsgIdAttrName = "id";
QString gMsgMethodAttrName = "method";
QString gMsgParamsAttrName = "params";
QString gResultAttrName = "result";
QString gSubscriptionAttrName = "params>>subscription";
QString gSubscribeBlockAttrName = "params>>result>>block";
QString gSubscribeChangesAttrName = "params>>result>>changes";

int gAccessTimeoutSeconds = 30;
int gResponseTimeoutSeconds = 30;
int gCacheTimeoutSeconds = 10;
int gShorterTimeoutSeconds = 5;
int gLongerTimeoutSeconds = 60;
int gCacheItemMaxCount = 1000;

QFuture<void> gCacheThread;

QStringList gCacheMethodList;
QMap<QString, QString> gSubscribeMethodMap;
QList<UnsubscribeRequestItem> gUnsubscribeRequestList;
QStringList gNeverTimeoutMethodList;
QStringList gRefreshUneraseMethodList;
QStringList gRefreshExcludeMethodList;
QStringList gRefreshPassthroughMethodList;

QMap<QByteArray, CacheItem> gCacheItemMap;

// multi packets params
ZhttpResponsePacket gHttpMultiPartResponsePacket;
QMap<QByteArray, ZhttpRequestPacket> gWsMultiPartRequestItemMap;
QMap<QByteArray, ZhttpResponsePacket> gWsMultiPartResponseItemMap;

/////////////////////////////////////////////////////////////////////////////////////

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
	std::unique_ptr<QZmq::Socket> client_out_sock;
	std::unique_ptr<QZmq::Socket> client_out_stream_sock;
	std::unique_ptr<QZmq::Socket> client_in_sock;
	std::unique_ptr<QZmq::Socket> client_req_sock;
	std::unique_ptr<QZmq::Socket> server_in_sock;
	std::unique_ptr<QZmq::Socket> server_in_stream_sock;
	std::unique_ptr<QZmq::Socket> server_out_sock;
	std::unique_ptr<QZmq::Valve> client_in_valve;
	std::unique_ptr<QZmq::Valve> client_out_stream_valve;
	std::unique_ptr<QZmq::Valve> server_in_valve;
	std::unique_ptr<QZmq::Valve> server_in_stream_valve;
	QByteArray instanceId;
	int ipcFileMode;
	bool doBind;
	QHash<ZhttpRequest::Rid, ZhttpRequest*> clientReqsByRid;
	QHash<ZhttpRequest::Rid, ZhttpRequest*> serverReqsByRid;
	QList<ZhttpRequest*> serverPendingReqs;
	QHash<ZWebSocket::Rid, ZWebSocket*> clientSocksByRid;
	QHash<ZWebSocket::Rid, ZWebSocket*> serverSocksByRid;
	QList<ZWebSocket*> serverPendingSocks;
	std::unique_ptr<Timer> refreshTimer;
	QHash<void*, KeepAliveRegistration*> keepAliveRegistrations;
	QSet<KeepAliveRegistration*> sessionRefreshBuckets[ZHTTP_REFRESH_BUCKETS];
	int currentSessionRefreshBucket;
	Connection cosConnection;
	Connection cossConnection;
	Connection sosConnection;
	Connection rrConnection;
	Connection clientConnection;
	Connection clientOutStreamConnection;
	Connection serverConnection;
	Connection serverStreamConnection;
	Connection refreshTimerConnection;

	Private(ZhttpManager *_q) :
		QObject(_q),
		q(_q),
		ipcFileMode(-1),
		doBind(false),
		currentSessionRefreshBucket(0)
	{
		refreshTimer = std::make_unique<Timer>();
		refreshTimerConnection = refreshTimer->timeout.connect(boost::bind(&Private::refresh_timeout, this));
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
	}

	bool setupClientOut()
	{
		cosConnection.disconnect();
		rrConnection.disconnect();
		client_req_sock.reset();
		client_out_sock.reset();

		client_out_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Push);
		cosConnection = client_out_sock->messagesWritten.connect(boost::bind(&Private::client_out_messagesWritten, this, boost::placeholders::_1));

		client_out_sock->setHwm(OUT_HWM);
		client_out_sock->setShutdownWaitTime(CLIENT_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(client_out_sock.get(), client_out_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	bool setupClientOutStream()
	{
		rrConnection.disconnect();
		cossConnection.disconnect();
		client_req_sock.reset();
		client_out_stream_valve.reset();
		client_out_stream_sock.reset();

		client_out_stream_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);
		cossConnection = client_out_stream_sock->messagesWritten.connect(boost::bind(&Private::client_out_stream_messagesWritten, this, boost::placeholders::_1));

		client_out_stream_sock->setIdentity(instanceId);
		client_out_stream_sock->setWriteQueueEnabled(false);
		client_out_stream_sock->setHwm(DEFAULT_HWM);
		client_out_stream_sock->setShutdownWaitTime(CLIENT_STREAM_WAIT_TIME);
		client_out_stream_sock->setImmediateEnabled(true);

		QString errorMessage;
		if(!ZUtil::setupSocket(client_out_stream_sock.get(), client_out_stream_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		client_out_stream_valve = std::make_unique<QZmq::Valve>(client_out_stream_sock.get());
		clientOutStreamConnection = client_out_stream_valve->readyRead.connect(boost::bind(&Private::client_out_stream_readyRead, this, boost::placeholders::_1));

		client_out_stream_valve->open();

		return true;
	}

	bool setupClientIn()
	{
		rrConnection.disconnect();
		client_req_sock.reset();
		client_in_valve.reset();
		client_in_sock.reset();

		client_in_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Sub);

		client_in_sock->setHwm(DEFAULT_HWM);
		client_in_sock->setShutdownWaitTime(0);
		client_in_sock->subscribe(instanceId + ' ');

		QString errorMessage;
		if(!ZUtil::setupSocket(client_in_sock.get(), client_in_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		client_in_valve = std::make_unique<QZmq::Valve>(client_in_sock.get());
		clientConnection = client_in_valve->readyRead.connect(boost::bind(&Private::client_in_readyRead, this, boost::placeholders::_1));

		client_in_valve->open();

		return true;
	}

	bool setupClientReq()
	{
		cosConnection.disconnect();
		cossConnection.disconnect();
		client_out_sock.reset();
		client_out_stream_sock.reset();
		client_in_sock.reset();

		client_req_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Dealer);
		rrConnection = client_req_sock->readyRead.connect(boost::bind(&Private::client_req_readyRead, this));

		client_req_sock->setHwm(OUT_HWM);
		client_req_sock->setShutdownWaitTime(CLIENT_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(client_req_sock.get(), client_req_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	bool setupServerIn()
	{
		server_in_valve.reset();
		server_in_sock.reset();

		server_in_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Pull);

		server_in_sock->setHwm(IN_HWM);

		QString errorMessage;
		if(!ZUtil::setupSocket(server_in_sock.get(), server_in_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		server_in_valve = std::make_unique<QZmq::Valve>(server_in_sock.get());
		serverConnection = server_in_valve->readyRead.connect(boost::bind(&Private::server_in_readyRead, this, boost::placeholders::_1));

		server_in_valve->open();

		return true;
	}

	bool setupServerInStream()
	{
		serverStreamConnection.disconnect();
		server_in_stream_sock.reset();

		server_in_stream_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);

		server_in_stream_sock->setIdentity(instanceId);
		server_in_stream_sock->setHwm(DEFAULT_HWM);

		QString errorMessage;
		if(!ZUtil::setupSocket(server_in_stream_sock.get(), server_in_stream_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		server_in_stream_valve = std::make_unique<QZmq::Valve>(server_in_stream_sock.get());
		serverStreamConnection = server_in_stream_valve->readyRead.connect(boost::bind(&Private::server_in_stream_readyRead, this, boost::placeholders::_1));

		server_in_stream_valve->open();

		return true;
	}

	bool setupServerOut()
	{
		sosConnection.disconnect();
		server_out_sock.reset();

		server_out_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Pub);
		sosConnection = server_out_sock->messagesWritten.connect(boost::bind(&Private::server_out_messagesWritten, this, boost::placeholders::_1));

		server_out_sock->setWriteQueueEnabled(false);
		server_out_sock->setHwm(DEFAULT_HWM);
		server_out_sock->setShutdownWaitTime(SERVER_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(server_out_sock.get(), server_out_specs, doBind, ipcFileMode, &errorMessage))
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

	void send_response_to_client(
		SessionType sessionType, 
		ZhttpResponsePacket::Type packetType,
		const QByteArray &clientId, 
		const QByteArray &from,
		int credits = 0,
		ZhttpResponsePacket *responsePacket = NULL,
		const QByteArray &responseKey = NULL)
	{
		assert(!from.isEmpty());

		ZhttpResponsePacket out;

		ZhttpResponsePacket::Id tempId;

		int newSeq = get_client_new_response_seq(clientId);
		if (newSeq < 0)
		{
			log_debug("[HTTP] failed to get new response seq %s", clientId.toHex().data());
			return;
		}
		QByteArray newFrom = from;

		switch (packetType)
		{
		case ZhttpResponsePacket::Data:
			if (responsePacket != NULL)
			{
				out = *responsePacket;
				out.ids[0].id = clientId;
				out.ids[0].seq = newSeq;
				if (responseKey != NULL)
				{
					out.headers.removeAll("sec-websocket-accept");
					out.headers += HttpHeader("sec-websocket-accept", responseKey);
				}
				break;
			}
			return;
		case ZhttpResponsePacket::Credit:
			tempId.id = clientId;
			tempId.seq = newSeq;
			out.ids += tempId;
			out.type = packetType;
			out.credits = credits;
			break;
		default:
			tempId.id = clientId;
			tempId.seq = newSeq;
			out.ids += tempId;
			out.type = packetType;
			break;
		}

		out.from = instanceId;
		write(sessionType, out, newFrom);
	}

	void tryRequestCredit(const ZhttpResponsePacket &packet, const QByteArray &from, int credits, int seqNum)
	{
		std::weak_ptr<Private> self = q->d;

		const ZhttpResponsePacket::Id &id = packet.ids.first();

		// if this was not an error packet, send cancel
		if(packet.type != ZhttpResponsePacket::Error && packet.type != ZhttpResponsePacket::Cancel)
		{
			ZhttpRequestPacket out;
			out.from = instanceId;
			ZhttpRequestPacket::Id tempId;
			tempId.id = id.id; // id
			tempId.seq = seqNum; // seq
			out.ids += tempId;
			out.type = ZhttpRequestPacket::Credit;
			out.credits = credits;

			log_debug("[WS] sending credit packets client=%s, credit=%d", id.id.toHex().data(), credits);
			
			// is this for a websocket?
			ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(from, id.id));
			if(sock)
			{
				sock->handle(id.id, seqNum, out);
				if(self.expired())
					return;
			}
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
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client: OUT1", logprefix);

			client_out_sock->write(QList<QByteArray>() << buf);
		}
		else
		{
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client req: OUT2", logprefix);

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
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client: OUT3 %s", logprefix, instanceAddress.data());

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

		QByteArray packetId = packet.ids.first().id;
		int packetSeq = packet.ids.first().seq;

		// cache process
		if (gCacheEnable == true)
		{
			pause_cache_thread();

			int ccIndex = get_cc_index_from_clientId(packetId);
			if (packet.code == 101) // ws client init response code
			{
				if (ccIndex >= 0)
				{
					// cache client
					gWsCacheClientList[ccIndex].initFlag = true;
					gWsCacheClientList[ccIndex].lastResponseTime = time(NULL);
					gWsCacheClientList[ccIndex].lastResponseSeq = packetSeq;
					gWsCacheClientList[ccIndex].from = packet.from;
					log_debug("[WS] Initialized Cache client%d, %s", ccIndex, gWsCacheClientList[ccIndex].clientId.data());
					gWsInitResponsePacket = packet;
				}
				else
				{
					// real client
					log_debug("[WS] Initialized real client=%s", packetId.data());
				}
			}
			else
			{
				switch (packet.type)
				{
				case ZhttpResponsePacket::Cancel:
				case ZhttpResponsePacket::Close:
				case ZhttpResponsePacket::Error:
					{
						// set log level to debug
						//set_debugLogLevel(true);

						log_debug("[WS] switching client of error, condition=%s", packet.condition.data());

						// get error type
						QString conditionStr = QString(packet.condition);
						if (conditionStr.compare("remote-connection-failed", Qt::CaseInsensitive) == 0 ||
							conditionStr.compare("connection-timeout", Qt::CaseInsensitive) == 0)
						{
							log_debug("[WS] Sleeping for 10 seconds");
							sleep(10);
						}

						// if cache client0 is ON, start cache client1
						//switch_cacheClient(packetId, false);
					}
					break;
				case ZhttpResponsePacket::Credit:
					log_debug("[WS] skipping credit response");
					break;
				case ZhttpResponsePacket::Ping:
					log_debug("[WS] received ping response");
					break;
				case ZhttpResponsePacket::KeepAlive:
					log_debug("[WS] received keep-alive response");
					break;
				case ZhttpResponsePacket::Data:
					if (ccIndex >= 0)
					{
						// update data receive time
						gWsCacheClientList[ccIndex].lastResponseTime = time(NULL);

						// increase credit
						int creditSize = static_cast<int>(packet.body.size());
						//int seqNum = update_request_seq(packetId);
						//tryRequestCredit(packet, gWsCacheClientList[ccIndex].from, creditSize, seqNum);

						ZhttpRequestPacket out;
						out.type = ZhttpRequestPacket::Credit;
						out.credits = credits;
						send_ws_request_over_cacheclient(out, NULL, ccIndex);

						int ret = process_ws_cacheclient_response(packet, ccIndex);
						if (ret == 0)
						{
							resume_cache_thread();
							return;
						}
					}
					else
					{
						int ret = process_http_response(packet);
						if (ret == 0)
						{
							resume_cache_thread();
							return;
						}
					}
					break;
				default:
					break;
				}
			}

			resume_cache_thread();
		}

		update_client_response_seq(packetId, packetSeq);
		server_out_sock->write(QList<QByteArray>() << buf);
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

	void client_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void client_out_stream_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void server_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void client_req_readyRead()
	{
		std::weak_ptr<Private> self = q->d;

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
				if(self.expired())
					return;

				continue;
			}

			log_debug("zhttp/zws client req: received message for unknown request id");

			// NOTE: we don't respond with a cancel message in req mode
		}
	}

	void processClientIn(const QByteArray &receiver, const QByteArray &msg)
	{
		if(msg.length() < 1 || msg[0] != 'T')
		{
			log_warning("zhttp/zws client: received message with invalid format (missing type), skipping");
			return;
		}

		QVariant data = TnetString::toVariant(msg.mid(1));
		if(data.isNull())
		{
			log_warning("zhttp/zws client: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
		{
			if(!receiver.isEmpty())
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws client: IN %s", receiver.data());
			else
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws client: IN");
		}

		ZhttpResponsePacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zhttp/zws client: received message with invalid format (parse failed), skipping");
			return;
		}

		std::weak_ptr<Private> self = q->d;

		foreach(const ZhttpResponsePacket::Id &id, p.ids)
		{
			// is this for a websocket?
			ZWebSocket *sock = clientSocksByRid.value(ZWebSocket::Rid(instanceId, id.id));
			if(sock)
			{
				sock->handle(id.id, id.seq, p);
				if(self.expired())
					return;

				continue;
			}

			// is this for an http request?
			ZhttpRequest *req = clientReqsByRid.value(ZhttpRequest::Rid(instanceId, id.id));
			if(req)
			{
				req->handle(id.id, id.seq, p);
				if(self.expired())
					return;

				continue;
			}

			log_debug("zhttp/zws client: received message for unknown request id, skipping");
		}
	}

	void client_out_stream_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 3)
		{
			log_warning("zhttp/zws client: received router message with parts != 3, skipping");
			return;
		}

		processClientIn(QByteArray(), msg[2]);
	}

	void client_in_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 1)
		{
			log_warning("zhttp/zws client: received pub message with parts != 1, skipping");
			return;
		}

		int at = msg[0].indexOf(' ');
		if(at == -1)
		{
			log_warning("zhttp/zws client: received pub message with invalid format, skipping");
			return;
		}

		QByteArray receiver = msg[0].mid(0, at);
		QByteArray dataRaw = msg[0].mid(at + 1);

		processClientIn(receiver, dataRaw);
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
				if(p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel)
					send_response_to_client(WebSocketSession, ZhttpResponsePacket::Cancel, id.id, p.from);
				return;
			}

			if (gCacheEnable == true)
			{
				pause_cache_thread();

				// if requests from cache client
				int ccIndex = get_cc_index_from_init_request(p);
				if (ccIndex >= 0 && ccIndex < gWsCacheClientList.count())
				{
					gWsCacheClientList[ccIndex].initFlag = false;
					gWsCacheClientList[ccIndex].clientId = id.id;
					gWsCacheClientList[ccIndex].msgIdCount = -1;
					gWsCacheClientList[ccIndex].lastRequestSeq = id.seq;
					gWsCacheClientList[ccIndex].lastRequestTime = time(NULL);

					log_debug("[WS] passing the requests from cache client=%s", id.id.data());
				}
				else // if request from real client
				{
					log_debug("[WS] received init request from real client");
					if (get_main_cc_index() < 0)
					{
						log_warning("[WS] not initialized cache client, ignore");
						if(p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel)
							send_response_to_client(WebSocketSession, ZhttpResponsePacket::Cancel, id.id, p.from);
						resume_cache_thread();
						return;
					}
					else
					{
						// get resp key
						QByteArray responseKey = calculate_response_seckey_from_init_request(p);
						// register ws client
						register_ws_client(id.id, p.from, p.uri.toString());
						// respond with cached init packet
						send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, id.id, p.from, 0, &gWsInitResponsePacket, responseKey);
						resume_cache_thread();
						return;
					}
				}

				resume_cache_thread();
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

			q->socketReady();
		}
		else if(p.uri.scheme() == "https" || p.uri.scheme() == "http")
		{
			ZhttpRequest::Rid rid(p.from, id.id);

			ZhttpRequest *req = serverReqsByRid.value(rid);
			if(req)
			{
				log_warning("zhttp server: received message for existing request id, canceling");
				if(p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel)
					send_response_to_client(HttpSession, ZhttpResponsePacket::Cancel, id.id, p.from);
				return;
			}

			// cache process
			if (gCacheEnable == true)
			{
				pause_cache_thread();

				if (!p.headers.contains(HTTP_REFRESH_HEADER))
				{
					register_http_client(id.id, p.from, p.uri.toString());
				}
				else
				{
					// remove HTTP_REFRESH_HEADER header
					p.headers.removeAll(HTTP_REFRESH_HEADER);
				}

				resume_cache_thread();
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

			q->requestReady();
		}
		else
		{
			log_debug("zhttp/zws server: rejecting unsupported scheme: %s", qPrintable(p.uri.scheme()));
			if(p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel)
				send_response_to_client(UnknownSession, ZhttpResponsePacket::Cancel, id.id, p.from);
			return;
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

		std::weak_ptr<Private> self = q->d;

		for	(int i=0; i<p.ids.count(); i++)
		{
			QByteArray packetId = p.ids[i].id;

			// cache process
			if (gCacheEnable == true)
			{
				pause_cache_thread();

				// complete tasks from cache thread
				send_unsubscribe_request_over_cacheclient();

				// if request from cache client, skip
				if (gHttpClientMap.contains(packetId))
				{
					int ret = process_http_request(packetId, p, gHttpClientMap[packetId].urlPath);
					if (ret == 0)
					{
						resume_cache_thread();
						continue;
					}
				}
				else if (gWsClientMap.contains(packetId))
				{
					log_debug("[WS] received ws request from real client=%s", packetId.data());

					// if cancel/close request, remove client from the subscription client list
					switch (p.type)
					{
					case ZhttpRequestPacket::Cancel:
						unregister_client(packetId);
						//send_wsCloseResponse(packetId);
						resume_cache_thread();
						continue;
					case ZhttpRequestPacket::Close:
						send_response_to_client(WebSocketSession, ZhttpResponsePacket::Close, packetId, p.from);
						unregister_client(packetId);
						resume_cache_thread();
						continue;
					case ZhttpRequestPacket::KeepAlive:
						log_debug("[WS] received KeepAlive, ignoring");
						//send_pingResponse(packetId);
						resume_cache_thread();
						continue;
					case ZhttpRequestPacket::Pong:
						send_response_to_client(WebSocketSession, ZhttpResponsePacket::Credit, packetId, p.from, 0);
						resume_cache_thread();
						continue;
					case ZhttpRequestPacket::Ping:
						send_response_to_client(WebSocketSession, ZhttpResponsePacket::Pong, packetId, p.from);
						resume_cache_thread();
						continue;
					case ZhttpRequestPacket::Credit:
						resume_cache_thread();
						continue;
					case ZhttpRequestPacket::Data:
						// Send new credit packet
						send_response_to_client(WebSocketSession, ZhttpResponsePacket::Credit, packetId, p.from, static_cast<int>(p.body.size()));
						if (process_ws_stream_request(packetId, p) < 0)
						{
							resume_cache_thread();
							continue;
						}
						break;
					default:
						break;
					}
				}
				
				resume_cache_thread();
			}

			int newSeq = update_request_seq(packetId);
			if (newSeq >= 0)
				p.ids[i].seq = newSeq;
			else
				newSeq = p.ids[i].seq;

			// is this for a websocket?
			ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(p.from, packetId));
			if(sock)
			{
				sock->handle(packetId, newSeq, p);
				if(self.expired())
					return;

				continue;
			}

			// is this for an http request?
			ZhttpRequest *req = serverReqsByRid.value(ZhttpRequest::Rid(p.from, packetId));
			if(req)
			{
				req->handle(packetId, newSeq, p);
				if(self.expired())
					return;

				continue;
			}

			log_debug("zhttp/zws server: received message for unknown request id, skipping");
		}
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

	void timer_send_ping_to_client(QByteArray clientId)
	{
		if (gWsClientMap.contains(clientId))
		{
			log_debug("_[TIMER] send ping to client %s", clientId.data());
			send_response_to_client(
				WebSocketSession, 
				ZhttpResponsePacket::Ping,
				clientId,
				gWsClientMap[clientId].from);
			QTimer::singleShot(PING_INTERVAL * 1000, [=]() {
				timer_send_ping_to_client(clientId);
			});
		}
		else
		{
			log_debug("_[TIMER] exit ping for client %s", clientId.data());
		}
	}

	void refresh_cache(QByteArray itemId, QString urlPath)
	{
		log_debug("_[TIMER] cache refresh %s %s", itemId.toHex().data(), qPrintable(urlPath));
		if (!gCacheItemMap.contains(itemId))
		{
			log_debug("_[TIMER] exit refresh %s", itemId.toHex().data());
			return;
		}

		int timeInterval = get_next_cache_refresh_interval(itemId);
		if (gCacheItemMap[itemId].cachedFlag == true)
		{
			// delete old cache items if it`s not auto_refresh_unerase
			if ((gCacheItemMap[itemId].refreshFlag & AUTO_REFRESH_UNERASE) == 0)
			{
				qint64 currMTime = QDateTime::currentMSecsSinceEpoch();
				qint64 accessTimeoutMSeconds = gAccessTimeoutSeconds * 1000;
				qint64 accessDiff = currMTime - gCacheItemMap[itemId].lastAccessTime;
				if (accessDiff > accessTimeoutMSeconds)
				{
					// remove cache item
					log_debug("[CACHE] deleting cache item for access timeout %s", itemId.toHex().data());
					gCacheItemMap.remove(itemId);
					return;
				}
			}

			if (timeInterval > 0)
			{
				if (gCacheItemMap[itemId].proto == Scheme::http)
				{
					QByteArray reqBody = gCacheItemMap[itemId].requestPacket.body;
					QString newMsgId = QString("\"%1\"").arg(itemId.toHex().data());
					replace_id_field(reqBody, gCacheItemMap[itemId].orgMsgId, newMsgId);
					send_http_post_request_with_refresh_header(urlPath, reqBody, itemId.toHex().data());
				}
				else if (gCacheItemMap[itemId].proto == Scheme::websocket)
				{
					// Send client cache request packet for auto-refresh
					int ccIndex = get_cc_index_from_clientId(gCacheItemMap[itemId].cacheClientId);
					QString orgMsgId = gCacheItemMap[itemId].orgMsgId;
					gCacheItemMap[itemId].newMsgId = send_ws_request_over_cacheclient(gCacheItemMap[itemId].requestPacket, orgMsgId, ccIndex);
					gCacheItemMap[itemId].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
				}
			}
		}
		else
		{
			if (gCacheItemMap[itemId].retryCount > RETRY_RESPONSE_MAX_COUNT)
			{
				log_debug("[_TIMER] reached max retry count");
				return;
			}
			gCacheItemMap[itemId].retryCount++;
			// switch backend of the failed response
			if (gCacheItemMap[itemId].proto == Scheme::http)
			{
				urlPath = get_switched_http_backend_url(urlPath);
				QByteArray reqBody = gCacheItemMap[itemId].requestPacket.body;
				QString newMsgId = QString("\"%1\"").arg(itemId.toHex().data());
				replace_id_field(reqBody, gCacheItemMap[itemId].orgMsgId, newMsgId);
				send_http_post_request_with_refresh_header(urlPath, reqBody, itemId.toHex().data());
			}
			else if (gCacheItemMap[itemId].proto == Scheme::websocket)
			{
				// Send client cache request packet for auto-refresh
				int ccIndex = get_cc_next_index_from_clientId(gCacheItemMap[itemId].cacheClientId);
				gCacheItemMap[itemId].cacheClientId = gWsCacheClientList[ccIndex].clientId;
				urlPath = gWsCacheClientList[ccIndex].urlPath;
				QString orgMsgId = gCacheItemMap[itemId].orgMsgId;
				gCacheItemMap[itemId].newMsgId = send_ws_request_over_cacheclient(gCacheItemMap[itemId].requestPacket, orgMsgId, ccIndex);
				gCacheItemMap[itemId].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
			}
		}

		if (timeInterval > 0)
		{
			QTimer::singleShot(timeInterval * 1000, [=]() {
				refresh_cache(itemId, urlPath);
			});
		}
	}

	void register_cache_refresh(QByteArray itemId, QString urlPath)
	{
		if (!gCacheItemMap.contains(itemId))
		{
			log_debug("[REFRESH] Canceled cache item because it not exist %s", itemId.toHex().data());
			return;
		}

		log_debug("[REFRESH] Registered new cache refresh %s, %s", itemId.toHex().data(), qPrintable(urlPath));

		int timeInterval = get_next_cache_refresh_interval(itemId);
		if (timeInterval > 0)
		{
			QTimer::singleShot(timeInterval * 1000, [=]() {
				refresh_cache(itemId, urlPath);
			});
		}		
	}

	void unregister_client(const QByteArray& clientId)
	{
		if (gHttpClientMap.contains(clientId))
		{
			// delete client from gHttpClientMap
			gHttpClientMap.remove(clientId);
			log_debug("[HTTP] Deleted one client in gHttpClientMap, current count=%d", gHttpClientMap.size());
		}
		else
		{
			// cache lookup
			foreach(QByteArray itemId, gCacheItemMap.keys())
			{
				if (gCacheItemMap[itemId].clientMap.contains(clientId))
				{
					gCacheItemMap[itemId].clientMap.remove(clientId);
					log_debug("[WS] Deleted cached client clientId=%s, msgId=%d, subscriptionStr=%s", clientId.data(), gCacheItemMap[itemId].msgId, qPrintable(gCacheItemMap[itemId].subscriptionStr.left(16)));
				}
			}

			// delete client from gWsClientMap
			gWsClientMap.remove(clientId);
			log_debug("[WS] Deleted one client in gWsClientMap, current count=%d", gWsClientMap.size());
		}
	}

	void register_http_client(QByteArray packetId, QByteArray from, QString urlPath)
	{
		if (gHttpClientMap.contains(packetId))
		{
			log_debug("[HTTP] already exists http client id=%s", packetId.data());
			return;
		}

		struct ClientItem clientItem;
		clientItem.lastRequestSeq = 0;
		clientItem.lastResponseSeq = -1;
		clientItem.lastRequestTime = time(NULL);
		clientItem.lastResponseTime = time(NULL);
		clientItem.from = from;
		clientItem.urlPath = urlPath;
		gHttpClientMap[packetId] = clientItem;
		log_debug("[HTTP] added http client id=%s", packetId.data());

		return;
	}

	void register_ws_client(QByteArray packetId, QByteArray from, QString urlPath)
	{
		if (gWsClientMap.contains(packetId))
		{
			log_debug("[WS] already exists http client id=%s", packetId.data());
			return;
		}

		struct ClientItem clientItem;
		clientItem.lastRequestSeq = 0;
		clientItem.lastResponseSeq = -1;
		clientItem.lastRequestTime = time(NULL);
		clientItem.lastResponseTime = time(NULL);
		clientItem.from = from;
		clientItem.urlPath = urlPath;
		gWsClientMap[packetId] = clientItem;
		log_debug("[WS] added ws client id=%s", packetId.data());

		QTimer::singleShot(PING_INTERVAL * 1000, [=]() {
			timer_send_ping_to_client(packetId);
		});

		return;
	}

	void registerHttpCacheItem(
		const ZhttpRequestPacket &clientPacket, 
		QByteArray clientId, 
		const PacketMsg &packetMsg, 
		int backendNo)
	{
		// create new cache item
		struct CacheItem cacheItem;
		cacheItem.msgId = -1;
		cacheItem.newMsgId = -1;
		cacheItem.refreshFlag = 0x00;
		if (is_never_timeout_method(packetMsg.method, packetMsg.params))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_NEVER_TIMEOUT;
			log_debug("[HTTP] added refresh never timeout method");
		}
		if (gRefreshUneraseMethodList.contains(packetMsg.method, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_UNERASE;
			log_debug("[HTTP] added refresh unerase method");
		}
		if (gRefreshExcludeMethodList.contains(packetMsg.method, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_EXCLUDE;
			log_debug("[HTTP] added refresh exclude method");
		}
		if (gRefreshPassthroughMethodList.contains(packetMsg.method, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_PASSTHROUGH;
			log_debug("[HTTP] added refresh passthrough method");
		}
		cacheItem.lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.lastAccessTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.lastRequestTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.cachedFlag = false;

		cacheItem.methodName = packetMsg.method;

		// save the request packet with new id
		cacheItem.orgMsgId = packetMsg.id;
		cacheItem.requestPacket = clientPacket;
		cacheItem.clientMap[clientId].msgId = packetMsg.id;
		cacheItem.clientMap[clientId].from = clientPacket.from;
		cacheItem.proto = Scheme::http;
		cacheItem.retryCount = 0;
		cacheItem.httpBackendNo = backendNo;

		gCacheItemMap[packetMsg.paramsHash] = cacheItem;

		log_debug("[HTTP] Registered New Cache Item for id=%s method=\"%s\" backend=%d", qPrintable(packetMsg.id), qPrintable(packetMsg.method), backendNo);
	}

	int registerWsCacheItem(
		const ZhttpRequestPacket &clientPacket, 
		QByteArray clientId, 
		QString orgMsgId, 
		QString methodName,
		QString msgParams, 
		const QByteArray &methodNameParamsHashVal)
	{
		// create new cache item
		struct CacheItem cacheItem;

		int ccIndex = get_main_cc_index();
		if (ccIndex < 0)
			return -1;
		cacheItem.msgId = gWsCacheClientList[ccIndex].msgIdCount;
		cacheItem.newMsgId = gWsCacheClientList[ccIndex].msgIdCount;
		cacheItem.refreshFlag = 0x00;
		if (is_never_timeout_method(methodName, msgParams))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_NEVER_TIMEOUT;
			log_debug("[WS] added refresh never timeout method");
		}
		if (gRefreshUneraseMethodList.contains(methodName, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_UNERASE;
			log_debug("[WS] added refresh unerase method");
		}
		if (gRefreshExcludeMethodList.contains(methodName, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_EXCLUDE;
			log_debug("[WS] added refresh exclude method");
		}
		if (gRefreshPassthroughMethodList.contains(methodName, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_PASSTHROUGH;
			log_debug("[WS] added refresh passthrough method");
		}
		cacheItem.lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.lastAccessTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.cachedFlag = false;

		// save the request packet with new id
		cacheItem.orgMsgId = orgMsgId;
		cacheItem.requestPacket = clientPacket;
		cacheItem.clientMap[clientId].msgId = orgMsgId;
		cacheItem.clientMap[clientId].from = clientPacket.from;
		cacheItem.proto = Scheme::websocket;
		cacheItem.retryCount = 0;
		cacheItem.cacheClientId = gWsCacheClientList[ccIndex].clientId;

		cacheItem.methodName = methodName;

		// check cache/subscribe method
		if (is_cache_method(methodName))
		{
			cacheItem.methodType = CACHE_METHOD;
		}
		else if (is_subscribe_method(methodName))
		{
			cacheItem.methodType = SUBSCRIBE_METHOD;
		}

		gCacheItemMap[methodNameParamsHashVal] = cacheItem;

		return ccIndex;
	}

	void reply_http_cached_content(const QByteArray &cacheItemId, QString orgMsgId, const QByteArray &newPacketId, const QByteArray &from)
	{
		//// Send cached response
		ZhttpResponsePacket responsePacket = gCacheItemMap[cacheItemId].responsePacket;

		// replace id str
		replace_id_field(responsePacket.body, gCacheItemMap[cacheItemId].msgId, orgMsgId);

		// update "Content-Length" field
		int newContentLength = static_cast<int>(responsePacket.body.size());
		log_debug("[HTTP] body newlength=%d", newContentLength);
		// replace messageid
		QByteArray contentLengthHeader;
		contentLengthHeader.setNum(newContentLength);
		responsePacket.headers.removeAll("Content-Length");
		responsePacket.headers += HttpHeader("Content-Length", contentLengthHeader);

		int seqNum = 0;
		// update seq
		if (gHttpClientMap.contains(newPacketId))
		{
			seqNum = gHttpClientMap[newPacketId].lastResponseSeq + 1;
		}
		responsePacket.ids[0].id = newPacketId.data();
		responsePacket.ids[0].seq = seqNum;
		responsePacket.from = instanceId;
		
		write(HttpSession, responsePacket, from);
	}

	void send_http_response_to_client(const QByteArray &cacheItemId, const QByteArray &clientId)
	{
		ZhttpResponsePacket responsePacket = gCacheItemMap[cacheItemId].responsePacket;

		QString orgMsgId = gCacheItemMap[cacheItemId].clientMap[clientId].msgId;
		QByteArray orgFrom = gCacheItemMap[cacheItemId].clientMap[clientId].from;

		// replace messageid
		if (gCacheItemMap.contains(cacheItemId))
		{
			replace_id_field(responsePacket.body, gCacheItemMap[cacheItemId].msgId, orgMsgId);
		}
		else
		{
			log_debug("[HTTP] Unknown error for cache item");
			return;
		}		

		// update "Content-Length" field
		int newContentLength = static_cast<int>(responsePacket.body.size());
		log_debug("[HTTP] body newlength=%d", newContentLength);
		// replace messageid
		QByteArray contentLengthHeader;
		contentLengthHeader.setNum(newContentLength);
		responsePacket.headers.removeAll("Content-Length");
		responsePacket.headers += HttpHeader("Content-Length", contentLengthHeader);

		responsePacket.ids[0].id = clientId;
		int newSeq = get_client_new_response_seq(clientId);
		if (newSeq < 0)
		{
			log_debug("[HTTP] failed to get new response seq %s", clientId.toHex().data());
			return;
		}
		responsePacket.ids[0].seq = newSeq;

		write(HttpSession, responsePacket, orgFrom);
	}

	int process_http_request(QByteArray id, const ZhttpRequestPacket &p, const QString &urlPath)
	{
		QByteArray packetId = id;

		// parse json body
		PacketMsg packetMsg;
		if (parse_packet_msg(Scheme::http, p, packetMsg) < 0)
			return -1;

		// get method string
		if (packetMsg.id.isEmpty() || packetMsg.method.isEmpty())
		{
			log_debug("[HTTP] failed to get gMsgIdAttrName and gMsgMethodAttrName");
			return -1;
		}
		log_debug("[HTTP] new req msgId=%s method=%s msgParams=%s", qPrintable(packetMsg.id), qPrintable(packetMsg.method), qPrintable(packetMsg.params));

		if (is_cache_method(packetMsg.method))
		{
			if (gCacheItemMap.contains(packetMsg.paramsHash))
			{
				gCacheItemMap[packetMsg.paramsHash].lastAccessTime = QDateTime::currentMSecsSinceEpoch();

				if (gCacheItemMap[packetMsg.paramsHash].cachedFlag == true)
				{
					reply_http_cached_content(packetMsg.paramsHash, packetMsg.id, packetId, p.from);
					gHttpClientMap.remove(packetId);
					log_debug("[HTTP] Replied with Cache content for method \"%s\"", qPrintable(packetMsg.method));
					return 0;
				}
				else
				{
					log_debug("[HTTP] Already cache registered, but not added content \"%s\"", qPrintable(packetMsg.method));
					// add client to list
					gCacheItemMap[packetMsg.paramsHash].clientMap[packetId].msgId = packetMsg.id;
					gCacheItemMap[packetMsg.paramsHash].clientMap[packetId].from = p.from;
					log_debug("[HTTP] Adding new client id msgId=%s clientId=%s", qPrintable(packetMsg.id), packetId.data());
					gCacheItemMap[packetMsg.paramsHash].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
					return 0;
				}
			}
			else
			{
				log_debug("[HTTP] not found in cache");
			}

			int backendNo = -1;
			for (int i = 0; i < gHttpBackendUrlList.count(); i++)
			{
				if (urlPath == gHttpBackendUrlList[i])
				{
					backendNo = i;
					break;
				}				
			}

			// Register new cache item
			registerHttpCacheItem(p, packetId, packetMsg, backendNo);

			// register cache refresh
			register_cache_refresh(packetMsg.paramsHash, urlPath);
		}

		return -1;
	}

	int process_http_response(const ZhttpResponsePacket &response)
	{
		ZhttpResponsePacket p = response;
		QVariantMap jsonMap;
		QByteArray packetId = p.ids[0].id;

		// parse json body
		PacketMsg packetMsg;
		if (parse_packet_msg(Scheme::http, p, packetMsg) < 0)
			return -1;

		// convert to QByteArray
		QString tmpStr = packetMsg.id;
		QByteArray msgIdByte = QByteArray::fromHex(qPrintable(tmpStr.remove('\"')));

		if (gCacheItemMap.contains(msgIdByte))
		{
			QByteArray itemId = msgIdByte;
			
			// if not http, return
			if (gCacheItemMap[itemId].proto != Scheme::http)
			{
				log_debug("[HTTP] detected non http response with cache item id %s", qPrintable(packetMsg.id));
				return -1;
			}

			if (gCacheItemMap[itemId].cachedFlag == false && packetMsg.isResultNull == true && 
				gCacheItemMap[itemId].retryCount < RETRY_RESPONSE_MAX_COUNT)
			{
				log_debug("[HTTP] get NULL response, retrying %d", gCacheItemMap[itemId].retryCount);
				gCacheItemMap[itemId].lastAccessTime = QDateTime::currentMSecsSinceEpoch();

				return 0;
			}

			gCacheItemMap[itemId].retryCount = 0;
			gCacheItemMap[itemId].responsePacket = p;
			gCacheItemMap[itemId].msgId = 0;
			gCacheItemMap[itemId].newMsgId = 0;
			gCacheItemMap[itemId].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
			gCacheItemMap[itemId].cachedFlag = true;
			log_debug("[HTTP] Added/Updated Cache content for method=%s", qPrintable(packetMsg.method));
			// recover original msgId
			replace_id_field(gCacheItemMap[itemId].responsePacket.body, packetMsg.id, gCacheItemMap[itemId].msgId);

			// send response to all clients
			foreach(QByteArray cliId, gCacheItemMap[itemId].clientMap.keys())
			{
				send_http_response_to_client(itemId, cliId);
				gHttpClientMap.remove(cliId);
				log_debug("[HTTP] Sent Cache content to client id=%s", cliId.data());
			}
			gCacheItemMap[itemId].clientMap.clear();

			return 0;
		}
		else
		{
			// it`s not the response from switch-backend or auto-refresh
			foreach(QByteArray itemId, gCacheItemMap.keys())
			{
				if ((gCacheItemMap[itemId].proto == Scheme::http) && 
					(gCacheItemMap[itemId].requestPacket.ids[0].id == packetId) &&
					(gCacheItemMap[itemId].cachedFlag == false))
				{
					if (packetMsg.isResultNull == true && gCacheItemMap[itemId].retryCount < RETRY_RESPONSE_MAX_COUNT)
					{
						log_debug("[HTTP] get NULL response, retrying %d", gCacheItemMap[itemId].retryCount);
						gCacheItemMap[itemId].lastAccessTime = QDateTime::currentMSecsSinceEpoch();
						return 0;
					}
					gCacheItemMap[itemId].responsePacket = p;
					gCacheItemMap[itemId].responseHashVal = calculate_response_hash_val(p.body, 0);
					log_debug("[HTTP] responseHashVal=%s", gCacheItemMap[itemId].responseHashVal.toHex().data());
					gCacheItemMap[itemId].msgId = 0;
					gCacheItemMap[itemId].newMsgId = 0;
					gCacheItemMap[itemId].cachedFlag = true;
					gCacheItemMap[itemId].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
					log_debug("[HTTP] Added/Updated Cache content for method=%s", qPrintable(packetMsg.method));

					// recover original msgId
					replace_id_field(gCacheItemMap[itemId].responsePacket.body, packetMsg.id, gCacheItemMap[itemId].msgId);

					// send response to all clients
					foreach(QByteArray cliId, gCacheItemMap[itemId].clientMap.keys())
					{
						send_http_response_to_client(itemId, cliId);
						gHttpClientMap.remove(cliId);
						log_debug("[HTTP] Sent Cache content to client id=%s", cliId.data());
					}
					gCacheItemMap[itemId].clientMap.clear();

					return 0;
				}
			}
		}

		return -1;
	}

	int process_ws_cacheclient_response(const ZhttpResponsePacket &response, int cacheClientNumber)
	{
		ZhttpResponsePacket p = response;
		QVariantMap jsonMap;
		QByteArray packetId = p.ids[0].id;

		if (p.type != ZhttpResponsePacket::Data)
		{
			log_debug("[WS] passed cache client response");

			p.ids[0].seq = gWsCacheClientList[cacheClientNumber].lastResponseSeq + 1; // seq
			gWsCacheClientList[cacheClientNumber].lastResponseSeq = p.ids[0].seq;
			gWsCacheClientList[cacheClientNumber].lastResponseTime = time(NULL);

			//tryRespondEtc(WebSocketSession, packetId, p);
			return -1;
		}

		// check multi-part response
		int ret = check_multi_packets_for_ws_response(p);
		if (ret < 0)
			return -1;

		// parse json body
		PacketMsg packetMsg;
		if (parse_packet_msg(Scheme::websocket, p, packetMsg) < 0)
			return -1;

		// id
		int msgIdValue = is_convertible_to_int(packetMsg.id) ? packetMsg.id.toInt() : -1;
		
		// result
		QString msgResultStr = packetMsg.result;

		// if it is curie response without change, ignore			
		QString methodName = packetMsg.method;

		if (!packetMsg.subscription.isEmpty())
		{
			QString subscriptionStr = packetMsg.subscription;

			foreach(QByteArray itemId, gCacheItemMap.keys())
			{
				if (gCacheItemMap[itemId].subscriptionStr == subscriptionStr)
				{
					if (gCacheItemMap[itemId].cachedFlag == false)
					{
						// update subscription packet
						gCacheItemMap[itemId].subscriptionPacket = p;

						if (gCacheItemMap[itemId].msgId != -1)
						{
							gCacheItemMap[itemId].cachedFlag = true;
							log_debug("[WS] Added Subscription content for subscription method id=%d subscription=%s", gCacheItemMap[itemId].msgId, qPrintable(subscriptionStr));
							// send update subscribe to all clients
							foreach(QByteArray cliId, gCacheItemMap[itemId].clientMap.keys())
							{
								log_debug("[WS] Sending Subscription content to client id=%s", cliId.data());

								QString orgMsgId = gCacheItemMap[itemId].clientMap[cliId].msgId;
								QByteArray from = gCacheItemMap[itemId].clientMap[cliId].from;

								ZhttpResponsePacket out = gCacheItemMap[itemId].responsePacket;
								replace_id_field(out.body, gCacheItemMap[itemId].msgId, orgMsgId);
								replace_result_field(out.body, gCacheItemMap[itemId].subscriptionStr, gCacheItemMap[itemId].orgSubscriptionStr);
								send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, cliId, from, 0, &out);

								ZhttpResponsePacket out1 = gCacheItemMap[itemId].subscriptionPacket;
								replace_id_field(out1.body, gCacheItemMap[itemId].msgId, orgMsgId);
								replace_subscription_field(out1.body, gCacheItemMap[itemId].subscriptionStr, gCacheItemMap[itemId].orgSubscriptionStr);
								send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, cliId, from, 0, &out1);
							}
						}
					}
					else
					{
						if (jsonMap.contains(gSubscribeBlockAttrName) || jsonMap.contains(gSubscribeChangesAttrName))
						{
							QString msgBlockStr = jsonMap[gSubscribeBlockAttrName].toString().toLower();
							QString msgChangesStr = jsonMap[gSubscribeChangesAttrName].toString().toLower();
							ZhttpResponsePacket tempPacket = gCacheItemMap[itemId].subscriptionPacket;

							QString patternStr("\"block\":\"");
							qsizetype idxStart = tempPacket.body.indexOf(patternStr);
							if (idxStart >= 0)
							{
								qsizetype idxEnd = tempPacket.body.indexOf("\"", idxStart+9);
								tempPacket.body.replace(idxStart+9, idxEnd-(idxStart+9), QByteArray(qPrintable(msgBlockStr)));
							}
							else
							{
								log_debug("[WS] not found block in subscription cached response");
							}

							QStringList changesList = msgChangesStr.split("/");
							for ( const auto& changes : changesList )
							{
								QStringList changeList = changes.split("+");
								if (changeList.size() != 2)
								{
									log_debug("[WS] Invalid change list");
									continue;
								}

								QString patternStr(qPrintable("[\"" + changeList[0] + "\""));
								QString newPattern = "[\"";
								newPattern += changeList[0];
								newPattern += "\",\"";
								newPattern += changeList[1];
								newPattern += "\"]";

								qsizetype idxStart = 0;
								qsizetype idxEnd = 0;
								while (1)
								{
									idxStart = tempPacket.body.indexOf(patternStr, idxEnd);
									if (idxStart < 0)
										break;
									
									idxEnd = tempPacket.body.indexOf("]", idxStart+changeList[0].length());
									if (idxEnd > idxStart)
									{
										//QByteArray oldPattern = tempPacket.body.mid(idxStart, idxEnd-idxStart+1);
										//log_debug("[WS] replaced old=%s pattern=%s", tempPacket.body.data(), oldPattern.data());
										tempPacket.body.replace(idxStart, idxEnd-idxStart+1, qPrintable(newPattern));
										//log_debug("[WS] replaced new=%s pattern=%s", tempPacket.body.data(), qPrintable(newPattern));
										//log_debug("[WS] replaced at offset=%d", idxStart);
									}
									else
									{
										log_debug("[WS] not found change param in subscription cached response");
										break;
									}	
								}
								
							}

							gCacheItemMap[itemId].subscriptionPacket = tempPacket;
						}
						else // it`s for non state_subscribeStorage methods
						{
							gCacheItemMap[itemId].subscriptionPacket = p;
						}

						// update subscription last update time
						gCacheItemMap[itemId].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();

						// send update subscribe to all clients
						foreach(QByteArray cliId, gCacheItemMap[itemId].clientMap.keys())
						{
							log_debug("[WS] Sending Subscription update to client id=%s", cliId.data());

							QString orgMsgId = gCacheItemMap[itemId].clientMap[cliId].msgId;
							QByteArray from = gCacheItemMap[itemId].clientMap[cliId].from;

							ZhttpResponsePacket out1 = gCacheItemMap[itemId].subscriptionPacket;
							replace_id_field(out1.body, gCacheItemMap[itemId].msgId, orgMsgId);
							replace_subscription_field(out1.body, gCacheItemMap[itemId].subscriptionStr, gCacheItemMap[itemId].orgSubscriptionStr);
							send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, cliId, from, 0, &out1);
						}
					}

					return -1;
				}
			}

			// create new subscription item
			struct CacheItem cacheItem;
			cacheItem.msgId = -1;
			cacheItem.lastRequestTime = QDateTime::currentMSecsSinceEpoch();
			cacheItem.lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
			cacheItem.cachedFlag = false;
			cacheItem.methodType = CacheMethodType::SUBSCRIBE_METHOD;
			cacheItem.orgSubscriptionStr = subscriptionStr;
			cacheItem.subscriptionStr = subscriptionStr;
			cacheItem.cacheClientId = gWsCacheClientList[cacheClientNumber].clientId;
			cacheItem.subscriptionPacket = p;

			QByteArray subscriptionBytes = subscriptionStr.toLatin1();
			gCacheItemMap[subscriptionBytes] = cacheItem;
			log_debug("[WS] Registered Subscription for \"%s\"", qPrintable(subscriptionStr));

			// make invalild
			return -1;
		}

		if(msgIdValue < 0)
		{
			// make invalild
			log_debug("[WS] detected response without id");
			return -1;
		}

		foreach(QByteArray itemId, gCacheItemMap.keys())
		{
			if ((gCacheItemMap[itemId].proto == Scheme::websocket) && 
				(gCacheItemMap[itemId].newMsgId == msgIdValue) &&
				(gCacheItemMap[itemId].cacheClientId == packetId))
			{
				if (gCacheItemMap[itemId].methodType == CacheMethodType::CACHE_METHOD)
				{
					log_debug("[WS] Adding Cache content for method name=%s", qPrintable(gCacheItemMap[itemId].methodName));

					if (gCacheItemMap[itemId].cachedFlag == false && packetMsg.isResultNull == true && 
						gCacheItemMap[itemId].retryCount < RETRY_RESPONSE_MAX_COUNT)
					{
						log_debug("[WS] get NULL response, retrying %d", gCacheItemMap[itemId].retryCount);
						gCacheItemMap[itemId].lastAccessTime = QDateTime::currentMSecsSinceEpoch();
						gCacheItemMap[itemId].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();

						return 0;
					}
					
					gCacheItemMap[itemId].responsePacket = p;
					gCacheItemMap[itemId].responseHashVal = calculate_response_hash_val(p.body, msgIdValue);
					log_debug("[WS] responseHashVal=%s", gCacheItemMap[itemId].responseHashVal.toHex().data());
					gCacheItemMap[itemId].msgId = msgIdValue;
					gCacheItemMap[itemId].cachedFlag = true;

					// send response to all clients
					QString urlPath = "";
					foreach(QByteArray clientId, gCacheItemMap[itemId].clientMap.keys())
					{
						if (urlPath.isEmpty())
							urlPath = gWsClientMap[clientId].urlPath;
						log_debug("[WS] Sending Cache content to client id=%s", clientId.data());
						QString orgMsgId = gCacheItemMap[itemId].clientMap[clientId].msgId;
						QByteArray from = gCacheItemMap[itemId].clientMap[clientId].from;
						ZhttpResponsePacket out = gCacheItemMap[itemId].responsePacket;
						replace_id_field(out.body, gCacheItemMap[itemId].msgId, orgMsgId);
						send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, clientId, from, 0, &out);
					}
					gCacheItemMap[itemId].clientMap.clear();

					// make invalid
					//config.cacheConfig.cacheMethodList.clear();
					return -1;
				}
				else if (gCacheItemMap[itemId].methodType == CacheMethodType::SUBSCRIBE_METHOD)
				{
					log_debug("[WS] Adding Subscribe content for method name=%s", qPrintable(gCacheItemMap[itemId].methodName));
					
					// result
					if(msgResultStr.isNull())
					{
						return -1;
					}
					gCacheItemMap[itemId].responsePacket = p;
					gCacheItemMap[itemId].msgId = msgIdValue;
					gCacheItemMap[itemId].subscriptionStr = msgResultStr;
					if (gCacheItemMap[itemId].orgSubscriptionStr.isEmpty())
					{
						gCacheItemMap[itemId].orgSubscriptionStr = msgResultStr;
					}
					else
					{
						log_debug("[WS] Detected the original subscription string \"%s\"", qPrintable(gCacheItemMap[itemId].orgSubscriptionStr));
					}
					
					log_debug("[WS] Registered Subscription result for \"%s\"", qPrintable(msgResultStr));

					// update subscription last update time
					gCacheItemMap[itemId].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();

					// Search temp teim in SubscriptionItemMap
					QByteArray resultBytes = msgResultStr.toLatin1();
					if (gCacheItemMap.contains(resultBytes))
					{
						if (gCacheItemMap[resultBytes].msgId == -1)
						{
							gCacheItemMap[itemId].subscriptionPacket = gCacheItemMap[resultBytes].subscriptionPacket;
							gCacheItemMap[itemId].cachedFlag = true;
							gCacheItemMap.remove(resultBytes);
							log_debug("[WS] Added Subscription content for subscription method id=%d result=%s", msgIdValue, qPrintable(msgResultStr));
						}
					}

					if (gCacheItemMap[itemId].cachedFlag == true)
					{
						// send update subscribe to all clients
						foreach(QByteArray cliId, gCacheItemMap[itemId].clientMap.keys())
						{
							log_debug("[WS] Sending Subscription content to client id=%s", cliId.data());
							
							QString orgMsgId = gCacheItemMap[itemId].clientMap[cliId].msgId;
							QByteArray from = gCacheItemMap[itemId].clientMap[cliId].from;

							ZhttpResponsePacket out = gCacheItemMap[itemId].responsePacket;
							replace_id_field(out.body, gCacheItemMap[itemId].msgId, orgMsgId);
							replace_result_field(out.body, gCacheItemMap[itemId].subscriptionStr, gCacheItemMap[itemId].orgSubscriptionStr);
							send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, cliId, from, 0, &out);

							ZhttpResponsePacket out1 = gCacheItemMap[itemId].subscriptionPacket;
							replace_id_field(out1.body, gCacheItemMap[itemId].msgId, orgMsgId);
							replace_subscription_field(out1.body, gCacheItemMap[itemId].subscriptionStr, gCacheItemMap[itemId].orgSubscriptionStr);
							send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, cliId, from, 0, &out1);
						}
					}
											
					// make invalid
					return -1;
				}
			}
		}

		return 0;
	}

	int send_ws_request_over_cacheclient(const ZhttpRequestPacket &packet, QString orgMsgId, int ccIndex)
	{
		if (ccIndex < 0 || gWsCacheClientList[ccIndex].initFlag == false)
		{
			log_debug("[WS] Invalid cache client %d", ccIndex);
			return -1;
		}

		// Create new packet by cache client
		ZhttpRequestPacket p = packet;
		ClientItem *cacheClient = &gWsCacheClientList[ccIndex];

		ZhttpRequestPacket::Id tempId;
		tempId.id = cacheClient->clientId; // id
		tempId.seq = update_request_seq(cacheClient->clientId);
		p.ids.clear();
		p.ids += tempId;

		if (!orgMsgId.isEmpty())
		{
			int msgId = cacheClient->msgIdCount + 1;
			replace_id_field(p.body, orgMsgId, msgId);
			cacheClient->msgIdCount = msgId;
		}

		// log
		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
		{
			QString vrespStr = TnetString::variantToString(p.toVariant(), -1);
			QString logStr;
			if (vrespStr.length() > DEBUG_LOG_MAX_LENGTH)
			{
				logStr = vrespStr.leftRef(DEBUG_LOG_MAX_LENGTH/2) + "........" + vrespStr.rightRef(DEBUG_LOG_MAX_LENGTH/2);
			}
			else
			{
				logStr = vrespStr;
			}
			log_debug("[WS] send_ws_request_over_cacheclient: %s", qPrintable(logStr));
		}

		std::weak_ptr<Private> self = q->d;

		foreach(const ZhttpRequestPacket::Id &id, p.ids)
		{
			// is this for a websocket?
			ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(p.from, id.id));
			if(sock)
			{
				sock->handle(id.id, id.seq, p);
				if(self.expired())
					return -1;

				continue;
			}
		}
		return msgId;
	}

	int send_unsubscribe_request_over_cacheclient()
	{
		int itemCount = gUnsubscribeRequestList.count();
		if (itemCount > 0)
		{			
			UnsubscribeRequestItem reqItem = gUnsubscribeRequestList[0];
			gUnsubscribeRequestList.removeAt(0);

			// Create new packet by cache client
			ZhttpRequestPacket p;
			ZhttpRequestPacket::Id tempId;

			int ccIndex = get_cc_index_from_clientId(reqItem.cacheClientId);

			if (ccIndex < 0 || gWsCacheClientList[ccIndex].initFlag == false)
			{
				log_debug("[WS] Invalid cache client %d", ccIndex);
				return -1;
			}

			ClientItem *cacheClient = &gWsCacheClientList[ccIndex];

			tempId.id = gWsCacheClientList[ccIndex].clientId; // id
			tempId.seq = update_request_seq(cacheClient->clientId);
			p.ids.append(tempId);

			p.type = ZhttpRequestPacket::Data;
			p.from = reqItem.from;

			char bodyStr[1024];
			int msgId = gWsCacheClientList[ccIndex].msgIdCount;
			QString methodName = reqItem.unsubscribeMethodName;
			qsnprintf(bodyStr, 1024, "{\"id\":%d,\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":[\"%s\"]}", 
				msgId, qPrintable(methodName), qPrintable(reqItem.subscriptionStr));
			gWsCacheClientList[ccIndex].msgIdCount++;
			p.body = QByteArray(bodyStr);

			log_debug("[WS] send_unsubscribeRequest: %s", qPrintable(TnetString::variantToString(p.toVariant(), -1)));

			std::weak_ptr<Private> self = q->d;

			foreach(const ZhttpRequestPacket::Id &id, p.ids)
			{
				// is this for a websocket?
				ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(p.from, id.id));
				if(sock)
				{
					sock->handle(id.id, id.seq, p);
					if(self.expired())
						return -1;

					continue;
				}
			}
		}

		return 0;
	}

	int process_ws_stream_request(const QByteArray packetId, ZhttpRequestPacket &p)
	{
		int ret = check_multi_packets_for_ws_request(p);
		if (ret < 0)
			return -1;
		
		// parse json body
		PacketMsg packetMsg;
		if (parse_packet_msg(Scheme::websocket, p, packetMsg) < 0)
			return -1;

		// read msgIdStr (id) and methodName (method)
		QString msgIdStr = packetMsg.id;
		QString methodName = packetMsg.method;
		QString msgParams = packetMsg.params;
		if (msgIdStr.isEmpty() || methodName.isEmpty())
		{
			log_debug("[WS] failed to get gMsgIdAttrName and gMsgMethodAttrName");
			return 0;
		}

		// get method string			
		log_debug("[WS] Cache entry msgId=\"%s\" method=\"%s\" params=\"%s\"", qPrintable(msgIdStr), qPrintable(methodName), qPrintable(msgParams));

		// Params hash val
		QByteArray paramsHash = packetMsg.paramsHash;

		if (is_cache_method(methodName) || is_subscribe_method(methodName))
		{
			if (gCacheItemMap.contains(paramsHash) && gCacheItemMap[paramsHash].proto == Scheme::websocket)
			{
				gCacheItemMap[paramsHash].lastAccessTime = QDateTime::currentMSecsSinceEpoch();

				if (gCacheItemMap[paramsHash].cachedFlag == true)
				{
					int ccIndex = get_cc_index_from_clientId(gCacheItemMap[paramsHash].cacheClientId);
					if (ccIndex < 0 || gWsCacheClientList[ccIndex].initFlag == false)
					{
						ccIndex = get_main_cc_index();
						if (ccIndex < 0)
						{
							log_warning("[WS] not initialized cache client, ignore");
							return 0;
						}
					}

					log_debug("[WS] Repling with Cache content for method \"%s\"", qPrintable(methodName));
					QString orgMsgId = msgIdStr;
					QByteArray from = p.from;

					if (gCacheItemMap[paramsHash].methodType == CacheMethodType::CACHE_METHOD)
					{
						ZhttpResponsePacket out = gCacheItemMap[paramsHash].responsePacket;
						replace_id_field(out.body, gCacheItemMap[paramsHash].msgId, orgMsgId);
						send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, packetId, p.from, 0, &out);
					}
					else if (gCacheItemMap[paramsHash].methodType == CacheMethodType::SUBSCRIBE_METHOD)
					{
						ZhttpResponsePacket out = gCacheItemMap[paramsHash].responsePacket;
						replace_id_field(out.body, gCacheItemMap[paramsHash].msgId, orgMsgId);
						replace_result_field(out.body, gCacheItemMap[paramsHash].subscriptionStr, gCacheItemMap[paramsHash].orgSubscriptionStr);
						send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, packetId, p.from, 0, &out);

						ZhttpResponsePacket out1 = gCacheItemMap[paramsHash].subscriptionPacket;
						replace_id_field(out1.body, gCacheItemMap[paramsHash].msgId, orgMsgId);
						replace_subscription_field(out1.body, gCacheItemMap[paramsHash].subscriptionStr, gCacheItemMap[paramsHash].orgSubscriptionStr);
						send_response_to_client(WebSocketSession, ZhttpResponsePacket::Data, packetId, p.from, 0, &out1);

						// add client to list
						gCacheItemMap[paramsHash].clientMap[packetId].msgId = msgIdStr;
						gCacheItemMap[paramsHash].clientMap[packetId].from = p.from;
						log_debug("[WS] Adding new client id msgId=%s clientId=%s", qPrintable(msgIdStr), packetId.data());
						gCacheItemMap[paramsHash].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
					}
				}
				else
				{
					log_debug("[WS] Already cache registered, but not added content \"%s\"", qPrintable(methodName));
					// add client to list
					gCacheItemMap[paramsHash].clientMap[packetId].msgId = msgIdStr;
					gCacheItemMap[paramsHash].clientMap[packetId].from = p.from;
					log_debug("[WS] Adding new client id msgId=%s clientId=%s", qPrintable(msgIdStr), packetId.data());
					gCacheItemMap[paramsHash].lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
				}

				return -1;
			}
			else
			{
				// Register new cache item
				int ccIndex = registerWsCacheItem(p, packetId, msgIdStr, methodName, msgParams, paramsHash);
				if (ccIndex < 0)
				{
					log_warning("[WS] not initialized cache client, ignore");
					return 0;
				}
				log_debug("[WS] Registered New Cache Item for id=%s method=\"%s\"", qPrintable(msgIdStr), qPrintable(methodName));
				
				// Send new client cache request packet
				gCacheItemMap[paramsHash].newMsgId = send_ws_request_over_cacheclient(p, msgIdStr, ccIndex);
				gCacheItemMap[paramsHash].lastRequestTime = QDateTime::currentMSecsSinceEpoch();

				// register cache refresh
				register_cache_refresh(paramsHash, gWsCacheClientList[ccIndex].urlPath);
			}
			
			return -1;
		}

		// log unhitted method
		log_debug("[CACHE ITME] not hit method = %s", qPrintable(methodName));

		return 0;
	}
};

ZhttpManager::ZhttpManager(QObject *parent) :
	QObject(parent)
{
	d = std::make_shared<Private>(this);
}

ZhttpManager::~ZhttpManager() = default;

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

int ZhttpManager::estimateRequestHeaderBytes(const QString &method, const QUrl &uri, const HttpHeaders &headers)
{
	int total = method.toUtf8().length();

	total += uri.path(QUrl::FullyEncoded).length();

	if(uri.hasQuery())
		total += uri.query(QUrl::FullyEncoded).length() + 1; // +1 for question mark

	foreach(const HttpHeader &h, headers)
	{
		total += h.first.length();
		total += h.second.length();
	}

	return total;
}

int ZhttpManager::estimateResponseHeaderBytes(int code, const QByteArray &reason, const HttpHeaders &headers)
{
	int total = QString::number(code).length();
	total += reason.length();

	foreach(const HttpHeader &h, headers)
	{
		total += h.first.length();
		total += h.second.length();
	}

	return total;
}

void ZhttpManager::setCacheParameters(
	bool enable,
	const QStringList &httpBackendUrlList,
	const QStringList &wsBackendUrlList,
	const QStringList &cacheMethodList,
	const QStringList &subscribeMethodList,
	const QStringList &neverTimeoutMethodList,
	const QStringList &refreshUneraseMethodList,
	const QStringList &refreshExcludeMethodList,
	const QStringList &refreshPassthroughMethodList,
	const QStringList &cacheKeyItemList,
	const QString &msgIdFieldName,
	const QString &msgMethodFieldName,
	const QString &msgParamsFieldName
	)
{
	gCacheEnable = enable;
	gHttpBackendUrlList = httpBackendUrlList;
	gWsBackendUrlList = wsBackendUrlList;

	// method list
	foreach (QString method, cacheMethodList)
	{
		gCacheMethodList.append(method.toLower());
	}
	for (int i = 0; i < subscribeMethodList.count(); i++)
	{
		QStringList tmpList = subscribeMethodList[i].split(u'+');
		if (tmpList.count() == 2)
		{
			gSubscribeMethodMap[tmpList[0].toLower()] = tmpList[1];
		}
	}
	foreach (QString method, neverTimeoutMethodList)
	{
		gNeverTimeoutMethodList.append(method.toLower());
	}
	foreach (QString method, refreshUneraseMethodList)
	{
		gRefreshUneraseMethodList.append(method.toLower());
	}
	foreach (QString method, refreshExcludeMethodList)
	{
		gRefreshExcludeMethodList.append(method.toLower());
	}
	foreach (QString method, refreshPassthroughMethodList)
	{
		gRefreshPassthroughMethodList.append(method.toLower());
	}

	// cache key item list
	for (int i = 0; i < cacheKeyItemList.size(); ++i) 
	{
		int lastDot = cacheKeyItemList[i].lastIndexOf('.');
		if (lastDot != -1) {
			CacheKeyItem keyItem;
			keyItem.keyName = cacheKeyItemList[i].left(lastDot);
			QString flagVal = cacheKeyItemList[i].mid(lastDot + 1);
			if (flagVal == "JSON_VALUE")
				keyItem.flag = ItemFlag::JSON_VALUE;
			else if (flagVal == "JSON_PAIR")
				keyItem.flag = ItemFlag::JSON_PAIR;
			else if (flagVal == "RAW_VALUE")
				keyItem.flag = ItemFlag::RAW_VALUE;
			else
				continue;

			gCacheKeyItemList.append(keyItem);
		} 
		else 
		{
			continue;
		}
	}

	// attributes
	gMsgIdAttrName = msgIdFieldName;
	gMsgMethodAttrName = msgMethodFieldName;
	gMsgParamsAttrName = msgParamsFieldName;

	log_debug("[CONFIG] gHttpBackendUrlList");
	for (int i = 0; i < gHttpBackendUrlList.size(); ++i) {
		log_debug("%s", qPrintable(gHttpBackendUrlList[i]));
	}

	log_debug("[CONFIG] gWsBackendUrlList");
	for (int i = 0; i < gWsBackendUrlList.size(); ++i) {
		log_debug("%s", qPrintable(gWsBackendUrlList[i]));
	}

	log_debug("[CONFIG] gCacheMethodList");
	for (int i = 0; i < gCacheMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gCacheMethodList[i]));
	}

	log_debug("[CONFIG] gSubscribeMethodMap");
	for (const auto &key : gSubscribeMethodMap.keys()) {
		log_debug("%s:%s", qPrintable(key), qPrintable(gSubscribeMethodMap.value(key)));
	}

	log_debug("[CONFIG] gNeverTimeoutMethodList");
	for (int i = 0; i < gNeverTimeoutMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gNeverTimeoutMethodList[i]));
	}

	log_debug("[CONFIG] gRefreshUneraseMethodList");
	for (int i = 0; i < gRefreshUneraseMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gRefreshUneraseMethodList[i]));
	}

	log_debug("[CONFIG] gRefreshExcludeMethodList");
	for (int i = 0; i < gRefreshExcludeMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gRefreshExcludeMethodList[i]));
	}

	log_debug("[CONFIG] gRefreshPassthroughMethodList");
	for (int i = 0; i < gRefreshPassthroughMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gRefreshPassthroughMethodList[i]));
	}

	log_debug("[CONFIG] gCacheKeyItemList");
	for (int i = 0; i < gCacheKeyItemList.size(); ++i) {
		log_debug("%s, %d", qPrintable(gCacheKeyItemList[i].keyName), gCacheKeyItemList[i].flag);
	}

	log_debug("gMsgIdAttrName = %s", qPrintable(gMsgIdAttrName));
	log_debug("gMsgMethodAttrName = %s", qPrintable(gMsgMethodAttrName));
	log_debug("gMsgParamsAttrName = %s", qPrintable(gMsgParamsAttrName));

	if (gCacheEnable == true)
	{
		// create processes for cache client
		for (int i = 0; i < gWsBackendUrlList.count(); i++)
		{
			pid_t processId = create_process_for_cacheclient(gWsBackendUrlList[i], i);
			if (processId > 0)
			{
				ClientItem cacheClient;
				cacheClient.initFlag = false;
				cacheClient.processId = processId;
				cacheClient.urlPath = gWsBackendUrlList[i];
				cacheClient.lastResponseTime = time(NULL);

				gWsCacheClientList.append(cacheClient);
			}
		}

		gCacheThread = QtConcurrent::run(cache_thread);
	}
}

#include "zhttpmanager.moc"
