#!/bin/bash
set -e

if [ -n "$1" ]; then
  arch="$1"
  cd /build
  . /opt/codex/*/*/environment-setup-*
  export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt

  rm -rf .rmlines-src .rmlines-build libs
  cp -r librm_lines .rmlines-src
  sed -i 's/rm_lines SHARED/rm_lines STATIC/' .rmlines-src/CMakeLists.txt
  cmake -S .rmlines-src -B .rmlines-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  cmake --build .rmlines-build --target rm_lines -j"$(nproc)"
  mkdir -p libs
  cp .rmlines-build/librm_lines.a libs/
  cp "$(find .rmlines-build -name libsimdutf.a | head -1)" libs/

  if [ -d /build/xovi-repo ]; then
    export XOVI_REPO=/build/xovi-repo
  else
    export XOVI_REPO=/tmp/xovi
    git clone --depth 1 https://github.com/asivery/xovi "$XOVI_REPO" 2>/dev/null || true
  fi
  python3 "$XOVI_REPO"/util/xovigen.py -o xovi.cpp -H xovi.h librarian.xovi
  qmake .
  make -j"$(nproc)"
  mv librarian.so "librarian-$arch.so"
  "$STRIP" --strip-all "librarian-$arch.so"

  make clean
  rm -rf .rmlines-src .rmlines-build libs
  exit 0
fi

echo "Building librarian for both architectures..."

build_arch() {
  local image="$1" arch="$2"
  echo ""
  echo "=== Building $arch ==="
  docker run --rm -v "$(pwd)":/build "$image" bash /build/build.sh "$arch"
  echo "Built: librarian-$arch.so"
}

build_arch eeems/remarkable-toolchain:latest-rmpp aarch64
build_arch eeems/remarkable-toolchain:latest-rm2 armv7

echo ""
echo "Build complete: librarian-aarch64.so, librarian-armv7.so"
