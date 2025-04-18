/*
 * Copyright (C) 2014-2015 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#include "zrpcrequest.h"

#include <assert.h>
#include <boost/signals2.hpp>
#include "packet/zrpcrequestpacket.h"
#include "packet/zrpcresponsepacket.h"
#include "zrpcmanager.h"
#include "uuidutil.h"
#include "log.h"
#include "timer.h"
#include "defercall.h"

using Connection = boost::signals2::scoped_connection;

class ZrpcRequest::Private
{
public:
	ZrpcRequest *q;
	ZrpcManager *manager;
	QList<QByteArray> reqHeaders;
	QByteArray from;
	QByteArray id;
	QString method;
	QVariantHash args;
	bool success;
	QVariant result;
	ErrorCondition condition;
	QByteArray conditionString;
	std::unique_ptr<Timer> timer;
	Connection timerConnection;
	DeferCall deferCall;

	Private(ZrpcRequest *_q) :
		q(_q),
		manager(0),
		success(false),
		condition(ErrorGeneric)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(timer)
		{
			timerConnection.disconnect();
			timer.reset();
		}

		if(manager)
		{
			manager->unlink(q);
			manager = 0;
		}
	}

	void respond(const QVariant &value)
	{
		ZrpcResponsePacket p;
		p.id = id;
		p.success = true;
		p.value = value;
		manager->write(reqHeaders, p);
	}

	void respondError(const QByteArray &condition, const QVariant &value)
	{
		ZrpcResponsePacket p;
		p.id = id;
		p.success = false;
		p.condition = condition;
		p.value = value;
		manager->write(reqHeaders, p);
	}

	void handle(const QList<QByteArray> &headers, const ZrpcRequestPacket &packet)
	{
		reqHeaders = headers;
		from = packet.from;
		id = packet.id;
		method = packet.method;
		args = packet.args;
	}

	void handle(const ZrpcResponsePacket &packet)
	{
		cleanup();

		success = packet.success;
		if(success)
		{
			result = packet.value;
			q->onSuccess();
		}
		else
		{
			if(packet.condition == "bad-format")
				condition = ErrorFormat;
			else
				condition = ErrorGeneric;

			conditionString = packet.condition;

			result = packet.value;
			q->onError();
		}

		q->finished();
	}

	void doStart()
	{
		if(!manager->canWriteImmediately())
		{
			success = false;
			condition = ErrorUnavailable;
			conditionString = "service-unavailable";
			cleanup();
			q->finished();
			return;
		}

		ZrpcRequestPacket p;
		p.id = id;
		p.method = method;
		p.args = args;

		if(manager->timeout() >= 0)
		{
			timer = std::make_unique<Timer>();
			timerConnection = timer->timeout.connect(boost::bind(&Private::timer_timeout, this));
			timer->setSingleShot(true);
			timer->start(manager->timeout());
		}

		manager->write(p);
	}

	void timer_timeout()
	{
		success = false;
		condition = ErrorTimeout;
		conditionString = "timeout";
		cleanup();
		q->finished();
	}
};

ZrpcRequest::ZrpcRequest()
{
	d = new Private(this);
}

ZrpcRequest::ZrpcRequest(ZrpcManager *manager)
{
	d = new Private(this);
	setupClient(manager);
}

ZrpcRequest::~ZrpcRequest()
{
	destroyed();
	delete d;
}

QByteArray ZrpcRequest::from() const
{
	return d->from;
}

QByteArray ZrpcRequest::id() const
{
	return d->id;
}

QString ZrpcRequest::method() const
{
	return d->method;
}

QVariantHash ZrpcRequest::args() const
{
	return d->args;
}

bool ZrpcRequest::success() const
{
	return d->success;
}

QVariant ZrpcRequest::result() const
{
	return d->result;
}

ZrpcRequest::ErrorCondition ZrpcRequest::errorCondition() const
{
	return d->condition;
}

QByteArray ZrpcRequest::errorConditionString() const
{
	return d->conditionString;
}

void ZrpcRequest::start(const QString &method, const QVariantHash &args)
{
	d->method = method;
	d->args = args;
	d->deferCall.defer([=] { d->doStart(); });
}

void ZrpcRequest::respond(const QVariant &result)
{
	d->respond(result);
}

void ZrpcRequest::respondError(const QByteArray &condition, const QVariant &result)
{
	d->respondError(condition, result);
}

void ZrpcRequest::setError(ErrorCondition condition, const QVariant &result)
{
	d->success = false;
	d->condition = condition;
	d->result = result;
}

void ZrpcRequest::onSuccess()
{
	// by default, do nothing
}

void ZrpcRequest::onError()
{
	// by default, do nothing
}

void ZrpcRequest::setupClient(ZrpcManager *manager)
{
	d->id = UuidUtil::createUuid();
	d->manager = manager;
	d->manager->link(this);
}

void ZrpcRequest::setupServer(ZrpcManager *manager)
{
	d->manager = manager;
}

void ZrpcRequest::handle(const QList<QByteArray> &headers, const ZrpcRequestPacket &packet)
{
	assert(d->manager);

	d->handle(headers, packet);
}

void ZrpcRequest::handle(const ZrpcResponsePacket &packet)
{
	assert(d->manager);

	d->handle(packet);
}
