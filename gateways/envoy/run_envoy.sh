#!/bin/bash
# =============================================================
# AegisEngine — Envoy Gateway 启动脚本
#
# 使用方式:
#   ./run_envoy.sh                # 前台运行（Ctrl+C 停止）
#   ./run_envoy.sh -d             # 后台运行
#   ./run_envoy.sh --stop         # 停止后台运行的 Envoy
#   ./run_envoy.sh --check        # 检查运行状态
#   ./run_envoy.sh --config-only  # 只是验证配置
# =============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ENVOY_BIN="${SCRIPT_DIR}/envoy"
ENVOY_CONFIG="${SCRIPT_DIR}/envoy.yaml"
PID_FILE="${SCRIPT_DIR}/envoy.pid"
ENVOY_LOG="${SCRIPT_DIR}/envoy.log"

# 如果你有自定义的 Envoy 二进制路径，可以在这里设置
# CUSTOM_ENVOY_BIN="/usr/local/bin/envoy"

detect_envoy() {
    if [ -n "${CUSTOM_ENVOY_BIN:-}" ] && [ -x "$CUSTOM_ENVOY_BIN" ]; then
        echo "$CUSTOM_ENVOY_BIN"
        return
    fi
    if [ -x "$ENVOY_BIN" ]; then
        echo "$ENVOY_BIN"
        return
    fi
    if command -v envoy &>/dev/null; then
        echo "envoy"
        return
    fi
    echo ""
    return 1
}

start_foreground() {
    local ENVOY
    ENVOY=$(detect_envoy) || {
        echo "❌ Envoy 未找到！请先运行 download_envoy.sh 下载。"
        exit 1
    }
    echo "🚀 启动 Envoy（前台模式）..."
    echo "   配置: ${ENVOY_CONFIG}"
    echo "   Ctrl+C 停止"
    exec "$ENVOY" -c "$ENVOY_CONFIG" --log-level info
}

start_background() {
    local ENVOY
    ENVOY=$(detect_envoy) || {
        echo "❌ Envoy 未找到！请先运行 download_envoy.sh 下载。"
        exit 1
    }
    echo "🚀 启动 Envoy（后台模式）..."
    nohup "$ENVOY" -c "$ENVOY_CONFIG" --log-level info \
        > "$ENVOY_LOG" 2>&1 &
    echo $! > "$PID_FILE"
    echo "   PID: $(cat "$PID_FILE")"
    echo "   日志: ${ENVOY_LOG}"
    echo "   管理接口: http://127.0.0.1:9902"
}

stop_envoy() {
    if [ ! -f "$PID_FILE" ]; then
        echo "⚠️  没有找到 PID 文件（envoy.pid），尝试 pkill..."
        pkill envoy 2>/dev/null && echo "✅ Envoy 已停止" || echo "ℹ️  Envoy 未在运行"
        return
    fi
    local PID
    PID=$(cat "$PID_FILE")
    if kill "$PID" 2>/dev/null; then
        echo "✅ Envoy (PID $PID) 已停止"
    else
        echo "ℹ️  进程 $PID 已不存在"
    fi
    rm -f "$PID_FILE"
}

check_status() {
    if [ -f "$PID_FILE" ]; then
        local PID
        PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
            echo "✅ Envoy 正在运行 (PID: $PID)"
            echo "   管理接口: http://127.0.0.1:9902"
            echo "   日志: ${ENVOY_LOG}"
            return 0
        else
            echo "⚠️  发现 PID 文件但进程已不存在"
            rm -f "$PID_FILE"
        fi
    fi
    
    if pgrep -x envoy &>/dev/null; then
        echo "✅ Envoy 正在运行:"
        pgrep -x envoy -a
        return 0
    fi
    
    echo "❌ Envoy 未在运行"
    return 1
}

check_config() {
    local ENVOY
    ENVOY=$(detect_envoy) || {
        echo "❌ Envoy 未找到！请先运行 download_envoy.sh 下载。"
        exit 1
    }
    echo "🔍 验证 Envoy 配置..."
    "$ENVOY" -c "$ENVOY_CONFIG" --mode validate
    echo "✅ 配置验证通过！"
}

# ── 参数解析 ──
case "${1:-}" in
    -d|--daemon)
        start_background
        ;;
    --stop)
        stop_envoy
        ;;
    --check|--status)
        check_status
        ;;
    --config-only|--validate)
        check_config
        ;;
    --help|-h)
        echo "使用方法: $0 [选项]"
        echo ""
        echo "  不带参数        前台运行 Envoy"
        echo "  -d, --daemon    后台运行 Envoy"
        echo "  --stop          停止后台运行的 Envoy"
        echo "  --check/--status 检查 Envoy 运行状态"
        echo "  --config-only   仅验证配置文件"
        echo "  --help          显示此帮助"
        ;;
    *)
        start_foreground
        ;;
esac
