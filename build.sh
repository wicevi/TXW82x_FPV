#!/usr/bin/env bash
#
# NE102 one-shot build helper.
#
# Builds:
#   txw  - TXW82x firmware (cdk-make, Core -> App, FLASH config)
#   pmu  - N32L403 PMU firmware (arm-none-eabi-gcc via custom/pmu/Makefile)
#
# Successful builds are archived automatically to $ARCHIVE_DIR (default
# "archive" at the repo root) as <timestamp>/txw and <timestamp>/pmu, plus a
# flat copy in $ARCHIVE_DIR/latest for tools that always want the newest.
#
# Usage:
#   ./build.sh              interactive menu (default choice: TXW Core->App)
#   ./build.sh txw          build TXW Core->App, then archive   [default build]
#   ./build.sh core         build TXW Core only (no archive - App not relinked)
#   ./build.sh app          build TXW App only (after config/app changes)
#   ./build.sh txw-app      alias of "app"
#   ./build.sh pmu          build PMU, then archive
#   ./build.sh all          build TXW (Core->App) + PMU, then archive
#   ./build.sh clean        clean TXW Core + App + PMU
#   ./build.sh clean-core   clean TXW Core only
#   ./build.sh clean-app    clean TXW App only
#   ./build.sh clean-pmu    clean PMU only
#   ./build.sh help
#
# Environment overrides:
#   CDK_MAKE      path to cdk-make.exe     (default D:/C-SKY/CDK/cdk-make.exe)
#   PMU_GCC_PATH  dir of arm-none-eabi-gcc (default: STM32CubeIDE bundled GCC)
#   ARCHIVE_DIR   archive root             (default ./archive)
#
set -u -o pipefail

# ---------------------------------------------------------------------------
# configuration (env overridable)
# ---------------------------------------------------------------------------
CDK_MAKE="${CDK_MAKE:-D:/C-SKY/CDK/cdk-make.exe}"
PMU_GCC_PATH="${PMU_GCC_PATH:-E:/STM32CubeIDE_1.18.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344/tools/bin}"
ARCHIVE_DIR="${ARCHIVE_DIR:-archive}"
PMU_DIR="custom/pmu"
APP_BIN="project/txw82xApp/APP.bin"

cd "$(dirname "$0")"

log()  { printf '\033[1;32m[build]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn ]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[fail ]\033[0m %s\n' "$*"; exit 1; }

require_file() {
    [ -f "$1" ] || fail "required file not found: $1 (install path OK?)"
}

# ---------------------------------------------------------------------------
# TXW (CDK) builds
# ---------------------------------------------------------------------------
cdk() { # cdk <project-name> <build|clean>
    local prj="$1" cmd="$2"
    require_file "$CDK_MAKE"
    log "cdk-make: $prj ($cmd, FLASH)"
    "$CDK_MAKE" -w '.\project\txw82x.cdkws' -p "$prj" -d "$cmd" -c FLASH 2>&1 | tee "$BUILD_LOG"
    return "${PIPESTATUS[0]}"
}

build_txw_full() {
    BUILD_LOG="$(mktemp -t txw_core_build.XXXXXX)"
    cdk txw82xCore build || { fail "TXW Core build failed (log: $BUILD_LOG)"; }
    cdk txw82xApp  build || { fail "TXW App build failed (log: $BUILD_LOG)"; }
    [ -f "$APP_BIN" ] || fail "build finished but $APP_BIN missing"
    log "TXW OK: $APP_BIN ($(stat -c %s "$APP_BIN") bytes)"
}

build_txw_core_only() {
    BUILD_LOG="$(mktemp -t txw_core_build.XXXXXX)"
    cdk txw82xCore build || { fail "TXW Core build failed (log: $BUILD_LOG)"; }
    log "TXW Core OK (note: App not relinked - run 'app' or 'txw' to refresh APP.bin)"
}

build_txw_app_only() {
    BUILD_LOG="$(mktemp -t txw_app_build.XXXXXX)"
    cdk txw82xApp build || { fail "TXW App build failed (log: $BUILD_LOG)"; }
    [ -f "$APP_BIN" ] || fail "build finished but $APP_BIN missing"
    log "TXW OK: $APP_BIN ($(stat -c %s "$APP_BIN") bytes)"
}

clean_txw_core() {
    BUILD_LOG="$(mktemp -t txw_clean.XXXXXX)"
    cdk txw82xCore clean || warn "TXW Core clean reported an error (continuing)"
    log "TXW Core cleaned"
}

clean_txw_app() {
    BUILD_LOG="$(mktemp -t txw_clean.XXXXXX)"
    cdk txw82xApp clean || warn "TXW App clean reported an error (continuing)"
    log "TXW App cleaned"
}

# ---------------------------------------------------------------------------
# PMU (GCC) build
# ---------------------------------------------------------------------------
build_pmu() {
    require_file "$PMU_DIR/Makefile"
    if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        log "PMU build (arm-none-eabi-gcc from PATH)"
        make -C "$PMU_DIR" -j8
    elif [ -x "$PMU_GCC_PATH/arm-none-eabi-gcc.exe" ] || [ -x "$PMU_GCC_PATH/arm-none-eabi-gcc" ]; then
        log "PMU build (GCC_PATH=$PMU_GCC_PATH)"
        make -C "$PMU_DIR" GCC_PATH="$PMU_GCC_PATH" -j8
    else
        fail "arm-none-eabi-gcc not found; set PMU_GCC_PATH=<bin dir> and retry"
    fi
    [ -f "$PMU_DIR/build/pmu_ne102.bin" ] || fail "PMU build finished but bin missing"
    log "PMU OK: $PMU_DIR/build/pmu_ne102.bin"
}

clean_pmu() {
    make -C "$PMU_DIR" clean
    log "PMU cleaned"
}

# ---------------------------------------------------------------------------
# archive
# ---------------------------------------------------------------------------
archive_results() { # archive_results <txw|pmu|all>
    local what="$1"
    local ts dest
    ts="$(date +%Y%m%d_%H%M%S)"
    dest="$ARCHIVE_DIR/$ts"
    mkdir -p "$dest/txw" "$dest/pmu" "$ARCHIVE_DIR/latest"

    if [ "$what" = txw ] || [ "$what" = all ]; then
        # timestamped folder keeps the traceability artifacts (bin + map);
        # the 48MB elf goes only to latest/ (for debugging)
        local p
        for p in "project/txw82xApp/APP.bin" \
                 "project/txw82xApp/project.map"; do
            if [ -f "$p" ]; then
                cp -f "$p" "$dest/txw/"
                cp -f "$p" "$ARCHIVE_DIR/latest/"
            else
                warn "missing txw artifact: $p"
            fi
        done
        if [ -f "project/txw82xApp/project.elf" ]; then
            cp -f "project/txw82xApp/project.elf" "$ARCHIVE_DIR/latest/"
        fi
    fi

    if [ "$what" = pmu ] || [ "$what" = all ]; then
        local p
        for p in "$PMU_DIR/build/pmu_ne102.elf" \
                 "$PMU_DIR/build/pmu_ne102.hex" \
                 "$PMU_DIR/build/pmu_ne102.bin" \
                 "$PMU_DIR/build/pmu_ne102.map"; do
            if [ -f "$p" ]; then
                cp -f "$p" "$dest/pmu/"
                cp -f "$p" "$ARCHIVE_DIR/latest/"
            else
                warn "missing pmu artifact: $p"
            fi
        done
    fi

    # build-info.txt: provenance of this archive
    local rev="unknown"
    rev="$(git describe --tags --always --dirty 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    {
        echo "archived : $(date '+%Y-%m-%d %H:%M:%S')"
        echo "revision : $rev"
        echo "scope    : $what"
        echo
        echo "--- files (size / md5) ---"
        ( cd "$dest" && find . -type f ! -name build-info.txt -exec ls -l {} \; && \
                         find . -type f ! -name build-info.txt -exec md5sum {} \; )
    } > "$dest/build-info.txt" 2>/dev/null
    cp -f "$BUILD_LOG" "$dest/" 2>/dev/null || true

    log "archived to $dest (flat copy in $ARCHIVE_DIR/latest)"
}

# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------
do_all()        { build_txw_full; build_pmu; archive_results all; }
do_txw()        { build_txw_full; archive_results txw; }
do_core()       { build_txw_core_only; }
do_app()        { build_txw_app_only; archive_results txw; }
do_pmu()        { build_pmu; archive_results pmu; }
do_clean_all()  { clean_txw_core; clean_txw_app; clean_pmu; }

usage() {
    cat <<'EOF'
NE102 build tool
  ./build.sh              interactive menu (default choice: TXW Core->App)
  ./build.sh txw          TXW Core->App, archive              [default build]
  ./build.sh core         TXW Core only (no archive - App not relinked)
  ./build.sh app          TXW App only (quick, after config/app edits), archive
  ./build.sh txw-app      alias of "app"
  ./build.sh pmu          PMU (N32L403 GCC), archive
  ./build.sh all          TXW (Core->App) + PMU, archive both
  ./build.sh clean        clean TXW Core + App + PMU
  ./build.sh clean-core   clean TXW Core only
  ./build.sh clean-app    clean TXW App only
  ./build.sh clean-pmu    clean PMU only

Environment overrides:
  CDK_MAKE=D:/C-SKY/CDK/cdk-make.exe
  PMU_GCC_PATH=<dir of arm-none-eabi-gcc>
  ARCHIVE_DIR=<archive root, default ./archive>
EOF
}

menu() {
    local choice
    while true; do
        echo
        echo "=============================================="
        echo " NE102 build tool  (archive -> $ARCHIVE_DIR)"
        echo "=============================================="
        echo " 1) TXW  Core->App, archive        [default]"
        echo " 2) TXW  Core only"
        echo " 3) TXW  App only, archive"
        echo " 4) PMU  only, archive"
        echo " 5) All  (TXW Core->App + PMU), archive"
        echo " 6) Clean all (Core + App + PMU)"
        echo " 7) Clean TXW Core"
        echo " 8) Clean TXW App"
        echo " 9) Clean PMU"
        echo " 0) Exit"
        printf "Select [1]: "
        read -r choice
        choice="${choice:-1}"
        case "$choice" in
            1) do_txw ;;
            2) do_core ;;
            3) do_app ;;
            4) do_pmu ;;
            5) do_all ;;
            6) do_clean_all ;;
            7) clean_txw_core ;;
            8) clean_txw_app ;;
            9) clean_pmu ;;
            0) exit 0 ;;
            *) warn "invalid choice: $choice" ;;
        esac
    done
}

BUILD_LOG="$(mktemp -t build.XXXXXX.log)"

case "${1:-}" in
    all)         do_all ;;
    txw)         do_txw ;;
    core)        do_core ;;
    app|txw-app) do_app ;;
    pmu)         do_pmu ;;
    clean)       do_clean_all ;;
    clean-core)  clean_txw_core ;;
    clean-app)   clean_txw_app ;;
    clean-pmu)   clean_pmu ;;
    help|-h|--help) usage ;;
    "")          menu ;;
    *)           usage; fail "unknown command: $1" ;;
esac
