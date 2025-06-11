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
#include <unordered_map>
#include <string>
#include <string_view>
#include <functional>
#include <span>

#include <ngtcp2/ngtcp2_crypto.h>

#include "config_base.h"
#include "tls_server_session_boringssl.h"
#include "network.h"
#include "shared.h"

using namespace ngtcp2;

struct Buffer {
  Buffer(const uint8_t *data, size_t datalen);
  explicit Buffer(size_t datalen);

  size_t size() const { return tail - begin; }
  size_t left() const { return buf.data() + buf.size() - tail; }
  uint8_t* wpos() { return tail; }
  std::span<const uint8_t> data() const { return {begin, size()}; }
  void push(size_t len) { tail += len; }
  void reset() { tail = begin; }

  std::vector<uint8_t> buf;
  // begin points to the beginning of the buffer.  This might point to
  // buf.data() if a buffer space is allocated by this object.  It is
  // also allowed to point to the external shared buffer.
  uint8_t *begin;
  // tail points to the position of the buffer where write should
  // occur.
  uint8_t *tail;
};

class HandlerBase {
public:
  HandlerBase();
  ~HandlerBase();

  ngtcp2_conn *conn() const;

  TLSServerSession *get_session() { return &tls_session_; }

  ngtcp2_crypto_conn_ref *conn_ref();

protected:
  ngtcp2_crypto_conn_ref conn_ref_;
  TLSServerSession tls_session_;
  ngtcp2_conn *conn_;
  ngtcp2_ccerr last_error_;
};
