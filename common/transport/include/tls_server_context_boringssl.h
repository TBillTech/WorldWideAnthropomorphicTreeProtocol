/*
 * ngtcp2
 *
 * Copyright (c) 2021 ngtcp2 contributors
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

#include <openssl/ssl.h>
#include <string>

#include "shared.h"

using namespace ngtcp2;

struct TLSServerConfig {
  std::string groups;
  bool quiet;
  bool verify_client;
  uint64_t max_streams_bidi;
  uint64_t max_streams_uni;
  uint64_t max_stream_data_bidi_local;
  uint64_t max_stream_data_bidi_remote;
  uint64_t max_stream_data_uni;
  uint64_t max_data;
};

class TLSServerContext {
public:
  TLSServerContext(const TLSServerConfig& config);
  ~TLSServerContext();

  int init(const char *private_key_file, const char *cert_file,
           AppProtocol app_proto);

  SSL_CTX *get_native_handle() const;

  void enable_keylog();
  
  const TLSServerConfig& getConfig() const { return config_; }

private:
  SSL_CTX *ssl_ctx_;
  TLSServerConfig config_;
};
