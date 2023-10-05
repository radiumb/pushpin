/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

#ifndef LISTENPORT_H
#define LISTENPORT_H

#include <QHostAddress>

class ListenPort
{
public:
	QHostAddress addr;
	int port;
	bool ssl;
	QString localPath;
	int mode;
	QString user;
	QString group;

	ListenPort() :
		port(-1),
		ssl(false),
		mode(-1)
	{
	}

	ListenPort(const QHostAddress &_addr, int _port, bool _ssl, const QString &_localPath = QString(), int _mode = -1, const QString &_user = QString(), const QString &_group = QString()) :
		addr(_addr),
		port(_port),
		ssl(_ssl),
		localPath(_localPath),
		mode(_mode),
		user(_user),
		group(_group)
	{
	}
};

#endif
