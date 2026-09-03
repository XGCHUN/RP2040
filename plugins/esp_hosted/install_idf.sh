#!/usr/bin/env bash
#
# install_idf.sh - interactive ESP-IDF setup for building the ESP-Hosted slave
#                  firmware (runs on the ESP32-C5/C6 co-processor).
#
# The RP2350 grblHAL firmware does NOT need ESP-IDF; this is only for the
# co-processor firmware (managed_components/esp-hosted-mcu).
#
# Just run it and answer the prompts:
#   ./install_idf.sh

set -euo pipefail

msg()  { printf '\033[1;32m>> %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31mXX %s\033[0m\n' "$*" >&2; exit 1; }

command -v git >/dev/null 2>&1 || die "git is required but not found."
command -v python3 >/dev/null 2>&1 || die "python3 is required but not found."

IDF_REPO="https://github.com/espressif/esp-idf.git"

# --- Ask for the version ---------------------------------------------------
printf "ESP-IDF version to install [v5.5]: "
read -r IDF_VERSION
IDF_VERSION="${IDF_VERSION:-v5.5}"

# --- Ask for the install path ----------------------------------------------
DEFAULT_PATH="$HOME/.esp/esp-idf/esp-idf_${IDF_VERSION}"
printf "Install path [%s]: " "${DEFAULT_PATH}"
read -r IDF_PATH
IDF_PATH="${IDF_PATH:-${DEFAULT_PATH}}"
# Expand a leading ~ if the user typed one.
IDF_PATH="${IDF_PATH/#\~/$HOME}"

# --- Check whether the path already exists ---------------------------------
REUSE=0
if [ -e "${IDF_PATH}" ]; then
    if [ -d "${IDF_PATH}/.git" ]; then
        printf "Path already contains an ESP-IDF checkout. Reuse and switch to %s? [Y/n]: " "${IDF_VERSION}"
        read -r ans
        case "${ans}" in
            [Nn]*) die "Aborted. Choose another path and re-run." ;;
            *)     REUSE=1 ;;
        esac
    else
        die "${IDF_PATH} already exists and is not an ESP-IDF checkout. Choose another path."
    fi
fi

msg "version=${IDF_VERSION}  path=${IDF_PATH}  targets=all"

# --- Clone / update --------------------------------------------------------
if [ "${REUSE}" -eq 1 ]; then
    msg "Reusing existing checkout, switching to ${IDF_VERSION}..."
    git -C "${IDF_PATH}" fetch --tags --depth 1 origin "${IDF_VERSION}" || \
        git -C "${IDF_PATH}" fetch --tags origin
    git -C "${IDF_PATH}" checkout "${IDF_VERSION}"
    git -C "${IDF_PATH}" submodule update --init --recursive --depth 1
else
    msg "Cloning ESP-IDF ${IDF_VERSION} (shallow, with submodules)..."
    mkdir -p "$(dirname "${IDF_PATH}")"
    git clone --branch "${IDF_VERSION}" --depth 1 --recurse-submodules --shallow-submodules \
        "${IDF_REPO}" "${IDF_PATH}"
fi

# --- Install toolchains for all supported chips ----------------------------
msg "Installing toolchains for all supported chips..."
( cd "${IDF_PATH}" && ./install.sh all )

# --- Done ------------------------------------------------------------------
msg "ESP-IDF ${IDF_VERSION} is ready at ${IDF_PATH}"
echo
echo "Enter the environment in each new shell with:"
echo "  . \"${IDF_PATH}/export.sh\""
echo
echo "Then build/flash the slave firmware for the co-processor, e.g.:"
echo "  cd managed_components/esp-hosted-mcu"
echo "  idf.py set-target esp32c6   # or esp32c5"
echo "  idf.py build flash monitor"
