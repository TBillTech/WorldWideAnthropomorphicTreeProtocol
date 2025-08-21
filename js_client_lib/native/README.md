# WWATP QUIC N-API Addon

A thin Node.js N-API wrapper around the C facade in `common/transport/include/quic_connector_c.h`.

Build:

- Ensure `libwwatp_quic.so` is built in `../../build`.
- From `js_client_lib/` run:

```
npm run build:native
```

This produces `js_client_lib/native/build/Release/wwatp_quic_native.node`.

Runtime:

- `transport/native_quic.js` will prefer this addon if present; otherwise it falls back to the ffi-napi loader.
- You can also set `WWATP_QUIC_SO` to help the FFI path when the addon is absent.
- You can also set `WWATP_QUIC_FFI_VERBOSE`=1 to help debug FFI messaging and I/O. 

