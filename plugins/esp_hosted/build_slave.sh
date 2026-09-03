#!/usr/bin/env bash
#
# build_slave.sh - helper to build the ESP-Hosted slave firmware (runs on the
#                  ESP32-C5/C6 co-processor), wrapping idf.py with simple
#                  subcommands.
#
# What it does:
#   * Prompts for the ESP-IDF install path (once) and sources its export.sh so
#     the IDF environment / tools are available.
#   * Wraps idf.py behind friendly subcommands (config via `slave_menuconfig`).
#
# Recommended usage (source once, then call commands directly):
#   source ./build_slave.sh      # prompts for IDF path, loads env, registers cmds
#   slave_build                  # build
#   slave_clean                  # clean
#   slave_flash_monitor          # flash + monitor
#
# After sourcing, these functions are available in your shell:
#   slave_build          Copy sdkconfig then build the firmware
#   slave_clean          Remove build output (idf.py fullclean)
#   slave_flash          Copy sdkconfig then flash to the co-processor
#   slave_monitor        Open the serial monitor
#   slave_flash_monitor  Flash then monitor
#   slave_menuconfig     Open the IDF configuration menu
#   slave_set_target <t> Set the chip target (e.g. esp32c6, esp32c5)
#   slave_env            (Re)load the IDF environment
#
# You can also run it directly for a one-shot command (env not kept afterwards):
#   ./build_slave.sh <command> [target]

# Detect whether we are being sourced. Only harden the shell (set -euo
# pipefail) when executed directly; doing so while sourced would apply those
# options to the user's interactive shell and can kill the terminal on any
# non-zero command (e.g. a failed lookup), which looks like a random crash.
if [ -n "${BASH_SOURCE[0]:-}" ] && [ "${BASH_SOURCE[0]}" != "$0" ]; then
    _is_sourced=1
else
    _is_sourced=0
    set -euo pipefail
fi

msg()  { printf '\033[1;32m>> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!! %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31mXX %s\033[0m\n' "$*" >&2; return 1 2>/dev/null || exit 1; }

# --- Resolve paths ---------------------------------------------------------
# Directory that contains this script (works for both `source` and direct run).
if [ -n "${BASH_SOURCE[0]:-}" ]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
else
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
fi

# Repo root is two levels up from this script (plugins/esp_hosted -> repo root).
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

SLAVE_DIR="${REPO_ROOT}/managed_components/esp-hosted-mcu/slave"

# --- Helpers ---------------------------------------------------------------
ensure_idf_env() {
    # Already sourced? Then nothing to do.
    if command -v idf.py >/dev/null 2>&1; then
        return 0
    fi

    DEFAULT_IDF_PATH="${IDF_PATH:-$HOME/.esp/esp-idf/esp-idf_v5.5}"
    printf "ESP-IDF install path [%s]: " "${DEFAULT_IDF_PATH}"
    read -r IDF_INSTALL_PATH
    IDF_INSTALL_PATH="${IDF_INSTALL_PATH:-${DEFAULT_IDF_PATH}}"
    # Expand a leading ~ if the user typed one.
    IDF_INSTALL_PATH="${IDF_INSTALL_PATH/#\~/$HOME}"

    EXPORT_SH="${IDF_INSTALL_PATH}/export.sh"
    [ -f "${EXPORT_SH}" ] || die "export.sh not found at ${EXPORT_SH} (is the path correct?)"

    msg "Sourcing ESP-IDF environment from ${EXPORT_SH}"
    # shellcheck disable=SC1090
    . "${EXPORT_SH}"

    command -v idf.py >/dev/null 2>&1 || die "idf.py not available after sourcing export.sh."
}

run_idf() {
    ensure_idf_env
    (
        cd "${SLAVE_DIR}"
        msg "idf.py $*"
        idf.py "$@"
    )
}

usage() {
    sed -n '2,40p' "${BASH_SOURCE[0]:-$0}" | sed 's/^# \{0,1\}//'
}

# --- Commands (usable directly once sourced) -------------------------------
# Commands are prefixed with `slave_` so they don't shadow common commands
# (env, clean, ...) in your interactive shell after sourcing.
slave_build()         { run_idf build; }
slave_clean()         { run_idf fullclean; }
slave_flash()         { run_idf flash; }
slave_monitor()       { run_idf monitor; }
slave_flash_monitor() { run_idf flash monitor; }
slave_menuconfig()    { run_idf menuconfig; }
slave_set_target()    { [ "${1:-}" ] || { die "set-target requires a target, e.g. esp32c6"; return; }; run_idf set-target "$1"; }
slave_env()           { ensure_idf_env && msg "IDF environment ready."; }

dispatch() {
    local CMD="${1:-}"
    shift 2>/dev/null || true

    case "${CMD}" in
    build)                     slave_build "$@" ;;
    clean)                     slave_clean "$@" ;;
    flash)                     slave_flash "$@" ;;
    monitor)                   slave_monitor "$@" ;;
    flash-monitor|flashmonitor) slave_flash_monitor "$@" ;;
    menuconfig)                slave_menuconfig "$@" ;;
    set-target)                slave_set_target "$@" ;;
    env)                       slave_env "$@" ;;
    -h|--help|help|"")
        usage
        ;;
    *)
        warn "Unknown command: ${CMD}"
        usage
        die "See usage above."
        ;;
    esac
}

# --- Entry point -----------------------------------------------------------
if [ "${_is_sourced}" -eq 1 ]; then
    # Sourced: load the IDF env once and register the command functions
    # (build, clean, flash, ...) into the current shell. Nothing is run.
    ensure_idf_env
    msg "Sourced. Now just type: slave_build | slave_clean | slave_flash | slave_monitor | slave_flash_monitor | slave_menuconfig | slave_set_target <chip>"
else
    # Executed directly: dispatch the requested command (defaults to help).
    dispatch "$@"
fi
