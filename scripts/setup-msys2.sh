#!/usr/bin/env bash
# Setup helper: run inside an MSYS2 MINGW64 shell.
# Installs the toolchain packages required for M0..M3.

set -euo pipefail

if [[ "${MSYSTEM:-}" != "MINGW64" ]]; then
    echo "ERROR: please run inside an MSYS2 MINGW64 shell (not MSYS, not UCRT)." >&2
    exit 1
fi

pacman -S --needed --noconfirm \
    git \
    base-devel \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-qt6-base \
    mingw-w64-x86_64-qt6-tools \
    mingw-w64-x86_64-openssl \
    mingw-w64-x86_64-gmp \
    autoconf \
    automake \
    libtool \
    bison \
    flex \
    gperf \
    gettext-devel

cat <<'EOF'

Toolchain installed. Next steps:

  1. Fetch external deps (strongSwan source, Wintun SDK):
       ./scripts/fetch-deps.sh

  2. Build everything:
       ./scripts/build-charon.sh           # charon-svc + swanctl
       cmake -S . -B build -G "MinGW Makefiles"
       cmake --build build                  # Qt GUI + wintun-spike

EOF
