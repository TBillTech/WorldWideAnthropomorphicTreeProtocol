/*
 * ngtcp2
 *
 * Copyright (c) 2020 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include <vector>
#include <deque>
#include <string>
#include <string_view>
#include <functional>

#include <ngtcp2/ngtcp2_crypto.h>

#include "tls_client_session_boringssl.h"
#include "network.h"
#include "shared.h"
#include "config_base.h" // Include the new config_base.h file

using namespace ngtcp2;

class ClientBase {
public:
  ClientBase();
  ~ClientBase();

  ngtcp2_conn *conn() const;

  int write_transport_params(const char *path,
                             const ngtcp2_transport_params *params);
  int read_transport_params(const char *path, ngtcp2_transport_params *params);

  void write_qlog(const void *data, size_t datalen);

  ngtcp2_crypto_conn_ref *conn_ref();

  void ticket_received();

protected:
  ngtcp2_crypto_conn_ref conn_ref_;
  TLSClientSession tls_session_;
  FILE *qlog_;
  ngtcp2_conn *conn_;
  ngtcp2_ccerr last_error_;
  bool ticket_received_;
};

void qlog_write_cb(void *user_data, uint32_t flags, const void *data,
                   size_t datalen);
