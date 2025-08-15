#include <napi.h>
#include <stdint.h>
#include <string.h>
extern "C" {
#include "quic_connector_c.h"
}

using Napi::Env;
using Napi::Function;
using Napi::Object;
using Napi::String;
using Napi::Number;
using Napi::Boolean;
using Napi::Value;
using Napi::External;

namespace {

Napi::Value LastError(const Napi::CallbackInfo& info) {
  Env env = info.Env();
  const char* msg = wwatp_quic_last_error();
  return String::New(env, msg ? msg : "");
}

Napi::Value CreateSession(const Napi::CallbackInfo& info) {
  Env env = info.Env();
  if (!info[0].IsObject()) {
    Napi::TypeError::New(env, "opts object required").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto optsObj = info[0].As<Object>();
  wwatp_quic_session_opts_t opts{};
  auto getStr = [&](const char* key) -> std::string {
    if (!optsObj.Has(key)) return {};
    auto v = optsObj.Get(key);
    if (!v.IsString()) return {};
    return v.As<String>().Utf8Value();
  };
  std::string url = getStr("url");
  std::string authority = getStr("authority");
  std::string cert = getStr("cert_file");
  std::string key = getStr("key_file");
  std::string ca = getStr("ca_file");
  opts.url = url.empty() ? nullptr : url.c_str();
  opts.authority = authority.empty() ? nullptr : authority.c_str();
  opts.cert_file = cert.empty() ? nullptr : cert.c_str();
  opts.key_file = key.empty() ? nullptr : key.c_str();
  opts.ca_file = ca.empty() ? nullptr : ca.c_str();
  opts.insecure_skip_verify = optsObj.Has("insecure_skip_verify") && optsObj.Get("insecure_skip_verify").ToBoolean().Value() ? 1 : 0;
  if (optsObj.Has("timeout_ms")) opts.timeout_ms = optsObj.Get("timeout_ms").ToNumber().Uint32Value();

  wwatp_quic_session_t* sess = wwatp_quic_create_session(&opts);
  if (!sess) {
    Napi::Error::New(env, std::string("create_session failed: ") + (wwatp_quic_last_error() ?: "unknown")).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return External<wwatp_quic_session_t>::New(env, sess);
}

Napi::Value CloseSession(const Napi::CallbackInfo& info) {
  Env env = info.Env();
  if (!info[0].IsExternal()) return env.Undefined();
  auto sess = info[0].As<External<wwatp_quic_session_t>>().Data();
  if (sess) wwatp_quic_close_session(sess);
  return env.Undefined();
}

Napi::Value OpenBidiStream(const Napi::CallbackInfo& info) {
  Env env = info.Env();
  if (!info[0].IsExternal() || !info[1].IsString()) {
    Napi::TypeError::New(env, "(session, path) required").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto sess = info[0].As<External<wwatp_quic_session_t>>().Data();
  std::string p = info[1].As<String>().Utf8Value();
  auto st = wwatp_quic_open_bidi_stream(sess, p.c_str());
  if (!st) {
    Napi::Error::New(env, std::string("open_bidi_stream failed: ") + (wwatp_quic_last_error() ?: "unknown")).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return External<wwatp_quic_stream_t>::New(env, st);
}

Napi::Value StreamWrite(const Napi::CallbackInfo& info) {
  Env env = info.Env();
  if (!info[0].IsExternal() || !info[1].IsTypedArray()) {
    Napi::TypeError::New(env, "(stream, Uint8Array, endStream?) required").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto st = info[0].As<External<wwatp_quic_stream_t>>().Data();
  auto u8 = info[1].As<Napi::Uint8Array>();
  int end = (info.Length() > 2 && info[2].ToBoolean().Value()) ? 1 : 0;
  int64_t rc = wwatp_quic_stream_write(st, u8.Data(), u8.ByteLength(), end);
  return Number::New(env, static_cast<double>(rc));
}

Napi::Value StreamRead(const Napi::CallbackInfo& info) {
  Env env = info.Env();
  if (!info[0].IsExternal()) {
    Napi::TypeError::New(env, "(stream, maxLen?, timeoutMs?) required").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto st = info[0].As<External<wwatp_quic_stream_t>>().Data();
  size_t maxLen = info.Length() > 1 && info[1].IsNumber() ? info[1].As<Number>().Uint32Value() : 65536;
  uint32_t timeoutMs = info.Length() > 2 && info[2].IsNumber() ? info[2].As<Number>().Uint32Value() : 0;
  std::string buf;
  buf.resize(maxLen);
  int64_t rc = wwatp_quic_stream_read(st, reinterpret_cast<uint8_t*>(&buf[0]), maxLen, timeoutMs);
  if (rc <= 0) return Napi::Uint8Array::New(env, 0);
  auto out = Napi::Uint8Array::New(env, static_cast<size_t>(rc));
  memcpy(out.Data(), buf.data(), static_cast<size_t>(rc));
  return out;
}

Napi::Value StreamClose(const Napi::CallbackInfo& info) {
  Env env = info.Env();
  if (!info[0].IsExternal()) return env.Undefined();
  auto st = info[0].As<External<wwatp_quic_stream_t>>().Data();
  if (st) wwatp_quic_stream_close(st);
  return env.Undefined();
}

Object Init(Env env, Object exports) {
  exports.Set("lastError", Function::New(env, LastError));
  exports.Set("createSession", Function::New(env, CreateSession));
  exports.Set("closeSession", Function::New(env, CloseSession));
  exports.Set("openBidiStream", Function::New(env, OpenBidiStream));
  exports.Set("write", Function::New(env, StreamWrite));
  exports.Set("read", Function::New(env, StreamRead));
  exports.Set("closeStream", Function::New(env, StreamClose));
  return exports;
}

NODE_API_MODULE(wwatp_quic_native, Init)

}  // namespace
