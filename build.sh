#!/usr/bin/env bash
# build.sh — Single-shot Linux build & test script for NRL-Server
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
OBJ_DIR="${BUILD_DIR}/objects"

CFLAGS="-std=c11 -O2 -g -Wall -Wextra"
CFLAGS="${CFLAGS} -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE"
CFLAGS="${CFLAGS} -I${ROOT_DIR}/include"
LDFLAGS="-lrt -lm -lpthread"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass()  { echo -e "${GREEN}[PASS]${NC} $1"; }
fail()  { echo -e "${RED}[FAIL]${NC} $1"; exit 1; }
info()  { echo -e "${YELLOW}[INFO]${NC} $1"; }

clean() {
    info "Cleaning build artifacts..."
    rm -rf "${OBJ_DIR}" "${BIN_DIR}/nrl-serverd" "${BIN_DIR}/nrlctl"
    mkdir -p "${OBJ_DIR}" "${BIN_DIR}"
    pass "Clean complete"
}

build_core() {
    info "Building nrl_core (INT4 lattice)..."
    gcc ${CFLAGS} -c "${ROOT_DIR}/src/nrl_core.c" -o "${OBJ_DIR}/nrl_core.o"
    info "Building nrl_gate..."
    gcc ${CFLAGS} -c "${ROOT_DIR}/src/nrl_gate.c" -o "${OBJ_DIR}/nrl_gate.o"
    info "Building nrl_sandbox..."
    gcc ${CFLAGS} -c "${ROOT_DIR}/src/nrl_sandbox.c" -o "${OBJ_DIR}/nrl_sandbox.o"
    info "Building nrl_http..."
    gcc ${CFLAGS} -c "${ROOT_DIR}/src/nrl_http.c" -o "${OBJ_DIR}/nrl_http.o"
    info "Linking nrl-serverd..."
    gcc ${CFLAGS} -o "${BIN_DIR}/nrl-serverd" \
        "${OBJ_DIR}/nrl_core.o" "${OBJ_DIR}/nrl_gate.o" \
        "${OBJ_DIR}/nrl_sandbox.o" "${OBJ_DIR}/nrl_http.o" \
        "${ROOT_DIR}/src/server_main.c" ${LDFLAGS}
    info "Linking nrlctl..."
    gcc ${CFLAGS} -o "${BIN_DIR}/nrlctl" "${ROOT_DIR}/src/ctl_main.c" ${LDFLAGS}
    pass "Build complete"
}

cd "${ROOT_DIR}"
if [ "${1:-}" = "clean" ]; then clean; exit 0; fi
echo "============================================"
echo "  NRL-Server v1 -- Build"
echo "============================================"
echo ""
if grep -q avx2 /proc/cpuinfo 2>/dev/null; then pass "AVX2 detected"; fi
if [ -f /sys/fs/cgroup/cgroup.controllers ]; then pass "cgroups v2 detected"; fi
clean
build_core
echo ""
echo "Binaries:"
echo "  ${BIN_DIR}/nrl-serverd"
echo "  ${BIN_DIR}/nrlctl"
