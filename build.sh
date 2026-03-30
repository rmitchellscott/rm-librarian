#!/bin/bash
set -e

echo "Building librarian for both architectures..."

echo ""
echo "=== Building for aarch64 (Paper Pro) ==="
docker run --rm -v $(pwd):/build eeems/remarkable-toolchain:latest-rmpp bash -c "
  cd /build && \
  . /opt/codex/*/*/environment-setup-* && \
  export XOVI_REPO=/tmp/xovi && \
  git clone https://github.com/asivery/xovi \$XOVI_REPO 2>/dev/null || true && \
  python3 \$XOVI_REPO/util/xovigen.py -o xovi.cpp -H xovi.h librarian.xovi && \
  qmake . && \
  make && \
  mv librarian.so librarian-aarch64.so && \
  make clean
"
echo "Built: librarian-aarch64.so"

echo ""
echo "=== Building for armv7 (reMarkable 2) ==="
docker run --rm -v $(pwd):/build eeems/remarkable-toolchain:latest-rm2 bash -c "
  cd /build && \
  . /opt/codex/*/*/environment-setup-* && \
  export XOVI_REPO=/tmp/xovi && \
  git clone https://github.com/asivery/xovi \$XOVI_REPO 2>/dev/null || true && \
  python3 \$XOVI_REPO/util/xovigen.py -o xovi.cpp -H xovi.h librarian.xovi && \
  qmake . && \
  make && \
  mv librarian.so librarian-armv7.so && \
  make clean
"
echo "Built: librarian-armv7.so"

echo ""
echo "=========================================="
echo "Build complete!"
echo "  - librarian-aarch64.so"
echo "  - librarian-armv7.so"
echo "=========================================="
