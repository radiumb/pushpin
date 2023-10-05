/*
 * Copyright (C) 2022 Fanout, Inc.
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

#ifndef RUST_JWT_H
#define RUST_JWT_H

#include <QtGlobal>

// NOTE: must match values on the rust side
#define JWT_KEYTYPE_SECRET 0
#define JWT_KEYTYPE_EC 1
#define JWT_KEYTYPE_RSA 2
#define JWT_ALGORITHM_HS256 0
#define JWT_ALGORITHM_ES256 1
#define JWT_ALGORITHM_RS256 2

extern "C"
{
	struct JwtEncodingKey
	{
		int type;
		void *key;
	};

	struct JwtDecodingKey
	{
		int type;
		void *key;
	};

	struct JwtBuffer
	{
		quint8 *data;
		size_t len;
	};

	JwtEncodingKey jwt_encoding_key_from_secret(const quint8 *data, size_t len);
	JwtEncodingKey jwt_encoding_key_from_pem(const quint8 *data, size_t len);
	void jwt_encoding_key_destroy(void *key);

	JwtDecodingKey jwt_decoding_key_from_secret(const quint8 *data, size_t len);
	JwtDecodingKey jwt_decoding_key_from_pem(const quint8 *data, size_t len);
	void jwt_decoding_key_destroy(void *key);

	void jwt_str_destroy(char *s);

	int jwt_encode(int alg, const char *claim, const void *key, char **out_token);
	int jwt_decode(int alg, const char *token, const void *key, char **out_claim);
}

#endif
