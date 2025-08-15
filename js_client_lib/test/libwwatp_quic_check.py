#!/usr/bin/env python3
"""
Small loader/checker for libwwatp_quic.so using Python ctypes.

It attempts to:
  1) Load the shared library
  2) Create a QUIC session using a minimal opts struct (url, cert, key)
  3) Immediately close the session

Usage (env overrides):
  WWATP_QUIC_SO   Path to libwwatp_quic.so (defaults to ../../build/libwwatp_quic.so)
  WWATP_URL       Target URL (default: https://127.0.0.1:12345/init/wwatp/)
  WWATP_CERT      Client certificate path (default: ../../test_instances/data/cert.pem)
  WWATP_KEY       Client private key path (default: ../../test_instances/data/private_key.pem)

Note: The underlying C API will attempt to connect during session creation. If no
server is listening, creation will likely fail. This script prints the library's
last error string in that case, which still confirms the .so loads correctly.
"""

from __future__ import annotations

import os
import sys
from ctypes import (
    CDLL,
    POINTER,
    Structure,
    byref,
    c_char_p,
    c_int,
    c_uint32,
    c_void_p,
    c_int64,
    c_size_t,
    c_uint8,
)
from pathlib import Path
import subprocess
import time
import socket
import errno
import signal


def _default_paths():
    here = Path(__file__).resolve()
    repo_root = here.parents[2]
    so_path = repo_root / "build" / "libwwatp_quic.so"
    server_bin = repo_root / "build" / "wwatp_server"
    server_cfg = repo_root / "js_client_lib" / "test" / "resources" / "wwatp_server_config.yaml"
    cert_path = repo_root / "test_instances" / "data" / "cert.pem"
    key_path = repo_root / "test_instances" / "data" / "private_key.pem"
    return so_path, server_bin, server_cfg, cert_path, key_path


class wwatp_quic_session_opts_t(Structure):
    # Must match common/transport/include/quic_connector_c.h exactly
    _fields_ = [
        ("url", c_char_p),              # const char*
        ("authority", c_char_p),        # const char* (optional)
        ("cert_file", c_char_p),        # const char* (optional)
        ("key_file", c_char_p),         # const char* (optional)
        ("ca_file", c_char_p),          # const char* (optional)
        ("insecure_skip_verify", c_int), # int
        ("timeout_ms", c_uint32),       # uint32_t
    ]


def load_lib(path: Path) -> CDLL:
    lib = CDLL(str(path))
    # Bind the minimal API we need
    lib.wwatp_quic_create_session.argtypes = [POINTER(wwatp_quic_session_opts_t)]
    lib.wwatp_quic_create_session.restype = c_void_p

    lib.wwatp_quic_close_session.argtypes = [c_void_p]
    lib.wwatp_quic_close_session.restype = None

    lib.wwatp_quic_open_bidi_stream.argtypes = [c_void_p, c_char_p]
    lib.wwatp_quic_open_bidi_stream.restype = c_void_p

    lib.wwatp_quic_stream_write.argtypes = [c_void_p, POINTER(c_uint8), c_size_t, c_int]
    lib.wwatp_quic_stream_write.restype = c_int64

    lib.wwatp_quic_stream_read.argtypes = [c_void_p, POINTER(c_uint8), c_size_t, c_uint32]
    lib.wwatp_quic_stream_read.restype = c_int64

    lib.wwatp_quic_stream_close.argtypes = [c_void_p]
    lib.wwatp_quic_stream_close.restype = None

    # Optional: set per-stream WWATP request signal (e.g., 0x17 GET_FULL_TREE_REQUEST)
    try:
        lib.wwatp_quic_stream_set_wwatp_signal.argtypes = [c_void_p, c_uint8]
        lib.wwatp_quic_stream_set_wwatp_signal.restype = None
    except AttributeError:
        # Older library may not have this symbol; default is GET_FULL_TREE
        pass

    lib.wwatp_quic_last_error.argtypes = []
    lib.wwatp_quic_last_error.restype = c_char_p
    return lib


def _is_udp_port_in_use(port: int, host: str = "127.0.0.1") -> bool:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind((host, port))
        return False
    except OSError as e:
        if e.errno in (errno.EADDRINUSE, errno.EACCES):
            return True
        # Treat other errors as not in use (best effort)
        return False
    finally:
        try:
            s.close()
        except Exception:
            pass


def _maybe_generate_certs(cert: Path, key: Path):
    if cert.exists() and key.exists():
        return
    if os.environ.get("WWATP_GEN_CERTS") != "1":
        return
    cert.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "openssl",
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-keyout",
        str(key),
        "-out",
        str(cert),
        "-days",
        "365",
        "-subj",
        "/CN=127.0.0.1",
    ]
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except Exception as e:
        print(f"[warn] Failed to auto-generate certs via openssl: {e}")


def _spawn_server_if_needed(server_bin: Path, config: Path, root: Path, cert: Path, key: Path):
    # Return (proc or None, started: bool)
    if _is_udp_port_in_use(12345, "127.0.0.1"):
        return None, False
    if not server_bin.exists():
        print(f"[warn] Server binary not found: {server_bin}. Skipping spawn.")
        return None, False
    if not config.exists():
        print(f"[warn] Server config not found: {config}. Skipping spawn.")
        return None, False
    if not (cert.exists() and key.exists()):
        print(f"[warn] Missing cert/key at {cert} and {key}. Set WWATP_GEN_CERTS=1 to auto-generate.")
        return None, False
    print(f"[info] Spawning server: {server_bin} {config}")
    proc = subprocess.Popen([str(server_bin), str(config)], cwd=str(root))
    # Wait for UDP bind
    deadline = time.time() + 15.0
    while time.time() < deadline:
        if _is_udp_port_in_use(12345, "127.0.0.1"):
            print("[info] Server ready (UDP bound)")
            return proc, True
        time.sleep(0.2)
    # Timed out
    try:
        proc.terminate()
    except Exception:
        pass
    print("[warn] Timed out waiting for server UDP bind; proceeding anyway.")
    return None, False


def main(argv: list[str]) -> int:
    so_default, server_bin, server_cfg, cert_default, key_default = _default_paths()

    so_path = Path(os.environ.get("WWATP_QUIC_SO", so_default))
    url = os.environ.get("WWATP_URL", "https://127.0.0.1:12345")
    cert = Path(os.environ.get("WWATP_CERT", cert_default))
    key = Path(os.environ.get("WWATP_KEY", key_default))
    root = Path(__file__).resolve().parents[2]

    print(f"[info] lib path: {so_path}")
    print(f"[info] url:      {url}")
    print(f"[info] cert:     {cert}")
    print(f"[info] key:      {key}")

    if not so_path.exists():
        print(f"[error] Shared library not found: {so_path}", file=sys.stderr)
        return 2

    try:
        lib = load_lib(so_path)
    except OSError as e:
        print(f"[error] Failed to load library: {e}", file=sys.stderr)
        return 3

    # Optional: start server
    _maybe_generate_certs(cert, key)
    proc = None
    started = False
    try:
        proc, started = _spawn_server_if_needed(server_bin, server_cfg, root, cert, key)
    except Exception as e:
        print(f"[warn] Failed to spawn server: {e}")

    # Build opts
    opts = wwatp_quic_session_opts_t(
        url=url.encode("utf-8"),
        authority=None,
        cert_file=str(cert).encode("utf-8"),
        key_file=str(key).encode("utf-8"),
        ca_file=None,
        insecure_skip_verify=1,  # allow local/dev without full verification
        timeout_ms=5000,
    )

    print("[info] Creating session...")
    session = lib.wwatp_quic_create_session(byref(opts))
    if not session:
        err = lib.wwatp_quic_last_error()
        msg = err.decode("utf-8", errors="replace") if err else "unknown error"
        print(f"[fail] create_session returned NULL: {msg}")
        # Non-zero exit still useful as a loader check succeeded; return 4 to indicate connect failure
        return 4

    print("[ok]   Session created successfully.")

    # Exercise stream open/write/read/close
    st = None
    try:
        print("[info] Opening bidi stream to /init/wwatp/ ...")
        st = lib.wwatp_quic_open_bidi_stream(session, b"/init/wwatp/")
        if not st:
            err = lib.wwatp_quic_last_error()
            msg = err.decode("utf-8", errors="replace") if err else "unknown error"
            print(f"[warn] open_bidi_stream returned NULL: {msg}")
        else:
            print("[ok]   Stream opened.")
            # Set request signal to GET_FULL_TREE (0x17) if available
            if hasattr(lib, 'wwatp_quic_stream_set_wwatp_signal'):
                try:
                    lib.wwatp_quic_stream_set_wwatp_signal(st, c_uint8(0x17))
                    print("[info] set WWATP signal: GET_FULL_TREE_REQUEST (0x17)")
                except Exception as e:
                    print(f"[warn] failed to set WWATP signal: {e}")
            # Send a zero-length final request body; server should respond with full tree
            wrote = lib.wwatp_quic_stream_write(st, None, c_size_t(0), c_int(1))
            print(f"[info] write (zero-len, end_stream) rc: {wrote}")
            # Try a short read
            buf_len = 4096
            buf = (c_uint8 * buf_len)()
            r = lib.wwatp_quic_stream_read(st, buf, c_size_t(buf_len), c_uint32(1500))
            if r > 0:
                data = bytes(buf[:r])
                print(f"[ok]   read {r} bytes: {data[:32].hex()}...")
            else:
                print(f"[info] read returned: {r}")
    finally:
        if st:
            try:
                lib.wwatp_quic_stream_close(st)
            except Exception as e:
                print(f"[warn] stream_close exception: {e}")
            else:
                print("[ok]   Stream closed.")

    print("[info] Closing session...")
    try:
        lib.wwatp_quic_close_session(session)
    except Exception as e:  # noqa: BLE001 - broad by design for best-effort close
        print(f"[warn] Exception during close: {e}")
        # Still treat as success since session was created
        return 0
    finally:
        if started and proc is not None:
            try:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=5)
                except Exception:
                    proc.kill()
            except Exception:
                pass

    print("[ok]   Session closed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
