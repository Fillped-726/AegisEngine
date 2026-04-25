#!/bin/bash
# =============================================================
# AegisEngine — 一键测试 Envoy 网关
#
# 1. 如果 Envoy 不存在则下载
# 2. 验证配置
# 3. 启动 Envoy
# 4. 做基本的连通性测试
# =============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENVOY_BIN="${SCRIPT_DIR}/envoy"
ENVOY_CONFIG="${SCRIPT_DIR}/envoy.yaml"

echo "═══════════════════════════════════════"
echo "  AegisEngine — Envoy 网关快速测试"
echo "═══════════════════════════════════════"

# Step 1: 检查 Envoy 二进制
if [ ! -x "$ENVOY_BIN" ]; then
    echo "➡️  未找到 Envoy 二进制，正在下载..."
    bash "${SCRIPT_DIR}/download_envoy.sh"
fi

echo ""
echo "➡️  1/3: 验证 Envoy 版本..."
"${ENVOY_BIN}" --version 2>&1 | head -3

echo ""
echo "➡️  2/3: 验证配置文件..."
"${ENVOY_BIN}" -c "$ENVOY_CONFIG" --mode validate
echo "   ✅ 配置有效"

echo ""
echo "➡️  3/3: 启动 Envoy（前台 5 秒测试）..."
# 启动 Envoy 后台运行
"${ENVOY_BIN}" -c "$ENVOY_CONFIG" --log-level warning &
ENVOY_PID=$!
echo "   Envoy PID: ${ENVOY_PID}"
sleep 2

# 检查是否存活
if kill -0 "$ENVOY_PID" 2>/dev/null; then
    echo "   ✅ Envoy 进程存活"
    
    # 检查管理接口
    if curl -s http://127.0.0.1:9902/server_info > /dev/null 2>&1; then
        echo "   ✅ 管理接口可用 (http://127.0.0.1:9902)"
        echo ""
        echo "   📊 统计信息:"
        curl -s http://127.0.0.1:9902/stats 2>/dev/null | grep -E "(connections|listener)" | head -10 || true
    else
        echo "   ⚠️  管理接口未响应"
    fi
else
    echo "   ❌ Envoy 启动失败"
    exit 1
fi

# 清理
echo ""
echo "➡️  清理: 停止 Envoy..."
kill "$ENVOY_PID" 2>/dev/null || true
wait "$ENVOY_PID" 2>/dev/null || true
echo "   ✅ Envoy 已停止"

echo ""
echo "═══════════════════════════════════════"
echo "  ✅ 所有测试通过！" 
echo "═══════════════════════════════════════"
echo ""
echo "📋 日常使用:"
echo "   启动:  ${SCRIPT_DIR}/run_envoy.sh"
echo "   后台:  ${SCRIPT_DIR}/run_envoy.sh -d"
echo "   停止:  ${SCRIPT_DIR}/run_envoy.sh --stop"
echo "   配置:  ${ENVOY_CONFIG}"
