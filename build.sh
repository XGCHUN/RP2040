#!/bin/bash
# grblHAL quick build script
#
# Recommended flow (source once per terminal, then use short commands):
#   source ./build.sh            - if a board was chosen before, reuse it and install
#                                  the build/clean/rebuild/only-clean/menu commands into
#                                  the current shell; otherwise prompt to pick one.
#   source ./build.sh <BOARD>    - set the board directly by its grblHAL macro,
#                                  e.g. source ./build.sh BOARD_MOTIONDEV_2350
#
# After sourcing, just type:
#   build            - build (auto-configures if needed)
#   clean            - wipe build dir and rebuild
#   rebuild          - wipe + reconfigure + build
#   only-clean       - only wipe the build dir
#   menu             - re-pick the board
#
# The board list is scanned from my_machine.h (the BOARD_xxx -> boards/*_map.h
# mapping). Selecting a board:
#   - rewrites my_machine.h to enable the chosen #define BOARD_xxx (others off)
#   - passes the matching -DPICO_BOARD to cmake
#
# Without sourcing you can still call it directly:
#   ./build.sh [build|clean|rebuild|only-clean|menu]
#
# Environment variables:
#   PICO_BOARD=... ...  - override the SDK board passed to cmake (advanced)
#   JOBS=4         ...  - number of parallel build jobs

# Detect whether we are being sourced (must not use set -e / exit in that case).
_SOURCED=0
if [ -n "${ZSH_VERSION:-}" ]; then
    [[ "${ZSH_EVAL_CONTEXT:-}" == *:file ]] && _SOURCED=1
elif [ -n "${BASH_VERSION:-}" ]; then
    [ "${BASH_SOURCE[0]}" != "${0}" ] && _SOURCED=1
fi

# Resolve the script directory (works both when sourced and executed).
if [ -n "${BASH_VERSION:-}" ]; then
    _SELF="${BASH_SOURCE[0]}"
else
    _SELF="${(%):-%N}"
fi
GRBL_PROJECT_DIR="$(cd "$(dirname "${_SELF}")" && pwd)"
export GRBL_PROJECT_DIR

# Local cache mapping a grblHAL BOARD_xxx macro to the SDK PICO_BOARD the user
# picked from the SDK board list. Not committed (see .gitignore).
GRBL_BOARD_MAP_FILE="${GRBL_PROJECT_DIR}/.build_board_map"

# Map a grblHAL BOARD_xxx macro to the SDK PICO_BOARD passed to cmake.
# Resolution order:
#   1. built-in mapping below (boards wired up with a custom pico_boards header
#      + PICO_PLATFORM in CMakeLists.txt);
#   2. the local cache file (a previous interactive SDK-board choice).
# If neither matches, prints NOTHING; the caller then prompts to pick an SDK
# board and stores the choice in the cache.
_grbl_pico_board() {
    case "$1" in
        BOARD_MOTIONDEV_2350) echo "motiondev_2350"; return ;;
        BOARD_RP2350_PLUS_W)  echo "rp2350_plus_w"; return ;;
    esac
    if [ -f "${GRBL_BOARD_MAP_FILE}" ]; then
        awk -F'|' -v m="$1" '$1==m {print $2; found=1} END{exit !found}' "${GRBL_BOARD_MAP_FILE}"
    fi
}

# Reverse-engineer the Pico SDK path: PICO_SDK_PATH env > CMakeCache > ~/.pico-sdk.
_grbl_sdk_path() {
    if [ -n "${PICO_SDK_PATH:-}" ] && [ -d "${PICO_SDK_PATH}" ]; then
        echo "${PICO_SDK_PATH}"
        return
    fi
    local cache="${GRBL_PROJECT_DIR}/build/CMakeCache.txt"
    if [ -f "${cache}" ]; then
        local p
        p="$(sed -nE 's/^PICO_SDK_PATH:[^=]*=(.*)$/\1/p' "${cache}" | head -1)"
        [ -n "${p}" ] && [ -d "${p}" ] && { echo "${p}"; return; }
    fi
    # Fall back to the newest SDK under ~/.pico-sdk/sdk.
    local newest
    newest="$(ls -d "${HOME}"/.pico-sdk/sdk/*/ 2>/dev/null | sort -V | tail -1)"
    [ -n "${newest}" ] && echo "${newest%/}"
}

# List all SDK board names (basename of src/boards/include/boards/*.h).
_grbl_sdk_boards() {
    local sdk dir
    sdk="$(_grbl_sdk_path)"
    dir="${sdk}/src/boards/include/boards"
    [ -d "${dir}" ] || return 1
    local f
    for f in "${dir}"/*.h; do
        [ -f "${f}" ] || continue
        local b="${f##*/}"
        echo "${b%.h}"
    done
}

# Prompt the user to pick an SDK board for a grblHAL macro, cache and echo it.
_grbl_pick_sdk_board() {
    local macro="$1"
    local -a boards
    local b
    while IFS= read -r b; do
        boards+=("${b}")
    done < <(_grbl_sdk_boards)

    if [ "${#boards[@]}" -eq 0 ]; then
        echo "!! Could not list SDK boards (SDK path: $(_grbl_sdk_path))" >&2
        return 1
    fi

    echo ">> '${macro}' has no PICO_BOARD configured. Pick one from the SDK:" >&2
    local i
    for i in "${!boards[@]}"; do
        printf "  %3d) %s\n" "$((i + 1))" "${boards[$i]}" >&2
    done

    local choice
    while true; do
        printf "Enter number [1-%d] (or type a name): " "${#boards[@]}" >&2
        read -r choice
        if [[ "${choice}" =~ ^[0-9]+$ ]] && [ "${choice}" -ge 1 ] && [ "${choice}" -le "${#boards[@]}" ]; then
            b="${boards[$((choice - 1))]}"
            break
        fi
        # Allow typing a board name directly (validated against the list).
        for i in "${!boards[@]}"; do
            [ "${boards[$i]}" = "${choice}" ] && { b="${choice}"; break 2; }
        done
        echo "   Invalid input, please try again." >&2
    done

    # Persist macro -> pico_board in the cache (replace any old entry).
    local tmp="${GRBL_BOARD_MAP_FILE}.tmp"
    [ -f "${GRBL_BOARD_MAP_FILE}" ] && grep -v "^${macro}|" "${GRBL_BOARD_MAP_FILE}" > "${tmp}" 2>/dev/null
    echo "${macro}|${b}" >> "${tmp}"
    mv "${tmp}" "${GRBL_BOARD_MAP_FILE}"
    echo ">> Saved: ${macro} -> PICO_BOARD=${b} (cached in ${GRBL_BOARD_MAP_FILE})" >&2

    echo "${b}"
}

# Scan my_machine.h for the "BOARD_xxx -> boards/yyy_map.h" mapping and print
# lines "MACRO|map_file|display_name". display_name comes from BOARD_NAME in the
# map file when available, otherwise the macro name.
_grbl_boards() {
    local mm="${GRBL_PROJECT_DIR}/my_machine.h"
    [ -f "${mm}" ] || return 0
    local macro map name
    # Match the board dispatch: "#ifdef BOARD_XXX" or "defined(BOARD_XXX)" /
    # "defined BOARD_XXX", followed by an include of boards/yyy_map.h (which may
    # be on the next line).
    grep -oE '#ifdef[ \t]+BOARD_[A-Z0-9_]+|defined[ (]*BOARD_[A-Z0-9_]+|boards/[A-Za-z0-9_]+_map\.h' "${mm}" \
    | awk '
        /BOARD_/ { m=$0; sub(/^.*BOARD_/,"BOARD_",m); gsub(/[^A-Z0-9_]/,"",m); macro=m; next }
        /boards\// { if (macro!="") { print macro "|" $0; macro="" } }
    ' | while IFS='|' read -r macro map; do
        name=""
        if [ -f "${GRBL_PROJECT_DIR}/${map}" ]; then
            name="$(grep -oE '#define[ \t]+BOARD_NAME[ \t]+"[^"]*"' "${GRBL_PROJECT_DIR}/${map}" \
                    | head -1 | sed -E 's/.*"([^"]*)".*/\1/')"
        fi
        [ -z "${name}" ] && name="${macro}"
        echo "${macro}|${map}|${name}"
    done
}

# Interactive picker. Writes the chosen macro to .build_board and exports BOARD.
# my_machine.h is NOT modified; the macro is passed to cmake via -DGRBL_BOARD.
_grbl_select_board() {
    local board_file="${GRBL_PROJECT_DIR}/.build_board"
    local -a macros maps names
    local macro map name
    while IFS='|' read -r macro map name; do
        macros+=("${macro}")
        maps+=("${map}")
        names+=("${name}")
    done < <(_grbl_boards)

    if [ "${#macros[@]}" -eq 0 ]; then
        echo "!! No boards found in my_machine.h"
        return 1
    fi

    echo ">> Select target board:"
    local i
    for i in "${!macros[@]}"; do
        printf "  %d) %-24s %s\n" "$((i + 1))" "${names[$i]}" "${macros[$i]}"
    done

    local choice
    while true; do
        printf "Enter number [1-%d]: " "${#macros[@]}"
        read -r choice
        if [[ "${choice}" =~ ^[0-9]+$ ]] && [ "${choice}" -ge 1 ] && [ "${choice}" -le "${#macros[@]}" ]; then
            break
        fi
        echo "   Invalid input, please try again."
    done

    local sel="${macros[$((choice - 1))]}"
    _grbl_set_board "${sel}"
}

# Set the board by macro name: validate, persist and export. Does NOT touch
# my_machine.h; the macro is injected at cmake time via -DGRBL_BOARD.
_grbl_set_board() {
    local sel="$1"
    local board_file="${GRBL_PROJECT_DIR}/.build_board"

    # Validate against the scanned list.
    local ok=0 macro map name
    while IFS='|' read -r macro map name; do
        [ "${macro}" = "${sel}" ] && ok=1
    done < <(_grbl_boards)
    if [ "${ok}" -eq 0 ]; then
        echo "!! Unknown board macro: ${sel}"
        echo "   Available:"
        _grbl_boards | awk -F'|' '{printf "     %s\n", $1}'
        return 1
    fi

    echo "${sel}" > "${board_file}"
    export BOARD="${sel}"

    local pb
    pb="$(_grbl_pico_board "${sel}")"
    if [ -n "${pb}" ]; then
        echo ">> Board selected: ${sel} (PICO_BOARD=${pb})"
    else
        echo ">> Board selected: ${sel}"
        echo "   No PICO_BOARD mapping yet; 'build' will let you pick one from the"
        echo "   SDK board list (the choice is remembered)."
    fi
}

# Resolve the board macro to use: BOARD env var > .build_board file.
_grbl_resolve_board() {
    local board_file="${GRBL_PROJECT_DIR}/.build_board"
    if [ -n "${BOARD:-}" ]; then
        echo "${BOARD}"
    elif [ -s "${board_file}" ]; then
        cat "${board_file}"
    else
        echo ""
    fi
}

_grbl_clean() {
    local build_dir="${GRBL_PROJECT_DIR}/build"
    echo ">> Cleaning build dir..."
    rm -rf "${build_dir}"
    echo ">> Clean done"
}

_grbl_configure() {
    local build_dir="${GRBL_PROJECT_DIR}/build"
    local pico_board="$1"
    local grbl_board="$2"
    echo ">> Configuring cmake (PICO_BOARD=${pico_board}, GRBL_BOARD=${grbl_board})..."
    cmake -B "${build_dir}" -S "${GRBL_PROJECT_DIR}" \
        -DPICO_BOARD="${pico_board}" -DGRBL_BOARD="${grbl_board}" || return 1
    # Track both so a change in either triggers a reconfigure.
    echo "${pico_board}|${grbl_board}" > "${build_dir}/.configured_board"
    echo ">> Configure done"
}

_grbl_build() {
    local build_dir="${GRBL_PROJECT_DIR}/build"
    local jobs="${JOBS:-$(nproc)}"
    local macro
    macro="$(_grbl_resolve_board)"
    if [ -z "${macro}" ]; then
        echo "!! No board selected yet. Run:  source ./build.sh   or   menu"
        return 1
    fi

    local pico_board="${PICO_BOARD:-$(_grbl_pico_board "${macro}")}"
    if [ -z "${pico_board}" ]; then
        # No mapping yet: let the user pick an SDK board (choice is cached).
        pico_board="$(_grbl_pick_sdk_board "${macro}")" || return 1
        [ -z "${pico_board}" ] && return 1
    fi

    # Switching PICO_BOARD or the board macro requires a reconfigure (CMake cache
    # variable + platform + injected compile definition), so wipe when it changes.
    local want="${pico_board}|${macro}"
    local configured=""
    [ -f "${build_dir}/.configured_board" ] && configured="$(cat "${build_dir}/.configured_board")"

    if [ ! -f "${build_dir}/Makefile" ]; then
        _grbl_configure "${pico_board}" "${macro}" || return 1
    elif [ "${configured}" != "${want}" ]; then
        echo ">> Board changed: '${configured:-none}' -> '${want}', wiping and reconfiguring..."
        _grbl_clean
        _grbl_configure "${pico_board}" "${macro}" || return 1
    fi

    echo ">> Building ${macro} (${jobs} jobs)..."
    cmake --build "${build_dir}" -j "${jobs}" || return 1
    echo ">> Build done"
    echo ">> Output: ${build_dir}/grblHAL.uf2"

    # Copy every grblHAL.* artifact to a board-named copy in the build dir,
    # e.g. grblHAL.uf2 -> motiondev_2350.uf2, grblHAL.elf.map -> motiondev_2350.elf.map
    local f dest
    for f in "${build_dir}"/grblHAL.*; do
        [ -f "${f}" ] || continue
        dest="${build_dir}/${pico_board}.${f#"${build_dir}/grblHAL."}"
        cp -f "${f}" "${dest}"
    done
    echo ">> Board-named copies: ${build_dir}/${pico_board}.*"
}

# Short commands installed into the current shell when sourced.
build()      { _grbl_build "$@"; }
clean()      { _grbl_clean ; }
rebuild()    { _grbl_clean && _grbl_build; }
only-clean() { _grbl_clean; }
menu()       { _grbl_select_board && echo ">> You can now just run:  build"; }

# ---------- When sourced: pick/set board, install commands, do not build ----------
if [ "${_SOURCED}" -eq 1 ]; then
    if [ -n "${1:-}" ]; then
        _grbl_set_board "$1"
    elif [ -s "${GRBL_PROJECT_DIR}/.build_board" ]; then
        export BOARD="$(cat "${GRBL_PROJECT_DIR}/.build_board")"
        echo ">> Using saved board: ${BOARD} (run 'menu' to change)"
    else
        _grbl_select_board
    fi
    echo ">> Commands ready: build | clean | rebuild | only-clean | menu"
    return 0 2>/dev/null || true
fi

# ---------- When executed directly ----------
set -e
case "${1:-build}" in
    build)      _grbl_build ;;
    clean)      _grbl_clean ;;
    rebuild)    _grbl_clean; _grbl_build ;;
    only-clean) _grbl_clean ;;
    menu)       _grbl_select_board; echo ">> Now 'source ./build.sh' to get short commands, or run ./build.sh" ;;
    *)
        echo "Usage: $0 [build|clean|rebuild|only-clean|menu]"
        echo ""
        echo "Recommended: 'source ./build.sh' to pick a board (scanned from"
        echo "my_machine.h) and get the short commands in your shell."
        echo ""
        echo "Environment variables:"
        echo "  PICO_BOARD=... ./build.sh  - override the SDK board for cmake"
        echo "  JOBS=4         ./build.sh  - number of parallel build jobs"
        exit 1
        ;;
esac
