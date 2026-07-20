#!/bin/bash
# grblHAL 快速编译脚本
# 用法:
#   ./build.sh          - 编译项目
#   ./build.sh clean    - 清除构建目录后重新编译
#   ./build.sh rebuild  - 清除后重新配置并编译
#   ./build.sh only-clean - 仅清除构建目录

set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

# 默认板子类型，按需修改
BOARD="${BOARD:-pico}"

# 并行编译线程数，默认使用全部 CPU 核心
JOBS="${JOBS:-$(nproc)}"

action="${1:-build}"

do_clean() {
    echo ">> 清除构建目录..."
    rm -rf "${BUILD_DIR}"
    echo ">> 清除完成"
}

do_configure() {
    echo ">> 配置 CMake (PICO_BOARD=${BOARD})..."
    cmake -B "${BUILD_DIR}" -S "${PROJECT_DIR}" \
        -DPICO_BOARD="${BOARD}"
    echo ">> 配置完成"
}

do_build() {
    # 如果 build 目录不存在或没有 Makefile，先配置
    if [ ! -f "${BUILD_DIR}/Makefile" ]; then
        do_configure
    fi
    echo ">> 开始编译 (${JOBS} 线程)..."
    cmake --build "${BUILD_DIR}" -j "${JOBS}"
    echo ">> 编译完成"
    echo ">> 输出文件: ${BUILD_DIR}/grblHAL.uf2"
}

case "${action}" in
    build)
        do_build
        ;;
    clean)
        do_clean
        do_build
        ;;
    rebuild)
        do_clean
        do_configure
        do_build
        ;;
    only-clean)
        do_clean
        ;;
    *)
        echo "用法: $0 [build|clean|rebuild|only-clean]"
        echo ""
        echo "  build       - 编译 (默认，如无配置会自动 cmake)"
        echo "  clean       - 清除后重新编译"
        echo "  rebuild     - 清除 + 重新配置 + 编译"
        echo "  only-clean  - 仅清除构建目录"
        echo ""
        echo "环境变量:"
        echo "  BOARD=pico_w  ./build.sh    - 指定板子类型"
        echo "  JOBS=4        ./build.sh    - 指定编译线程数"
        exit 1
        ;;
esac
