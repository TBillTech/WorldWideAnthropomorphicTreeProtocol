#!/usr/bin/env bash
# Used by CMakeLists.txt (MinGW ExternalProject_Add build steps for nghttp3/ngtcp2).
# Normalizes generated build-file timestamps so automake's auto-remake rule doesn't
# fire (it can misfire as literal `fail`/`eval` "command not found" errors when the
# maintainer tools it expects aren't on PATH), then invokes the given make program.
set -e

find . \( -name Makefile -o -name Makefile.in -o -name configure -o -name aclocal.m4 \
  -o -name config.h.in -o -name config.h -o -name stamp-h1 -o -name config.status \) \
  -exec touch {} +

make_program="$1"
shift
exec "$make_program" "$@"
