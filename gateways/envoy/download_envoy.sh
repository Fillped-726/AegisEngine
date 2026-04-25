#!/bin/bash
# =============================================================
# AegisEngine — 下载 Envoy 二进制
# 
# 从 Envoy 官方发布页下载最新的二进制文件
# 支持 Linux x86_64
# =============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENVOY_BIN="${SCRIPT_DIR}/envoy"

# ── 版本配置 ──
# 可以手动指定版本，留空则获取最新稳定版
ENVOY_VERSION="${ENVOY_VERSION:-}"

# Envoy 官方发布 URL 模板
# 格式: https://github.com/envoyproxy/envoy/releases/download/v{version}/envoy-{version}-linux-x86_64
DOWNLOAD_BASE="https://github.com/envoyproxy/envoy/releases"

get_latest_version() {
    echo "🔍 获取最新的 Envoy 稳定版本..."
    local LATEST
    LATEST=$(curl -sL "${DOWNLOAD_BASE}/latest" -o /dev/null -w '%{url_effective}')
    # 从 URL 中提取 X.Y.Z 版本号，如 /tag/v1.37.2 → 1.37.2
    LATEST=$(echo "$LATEST" | grep -oP 'v?\K\d+\.\d+\.\d+')
    echo "${LATEST#v}"
}

download_envoy() {
    local VERSION="${1}"
    # 注意：官方发布资产名称是 envoy-{version}-linux-x86_64（没有 v 前缀）
    local FILENAME="envoy-${VERSION}-linux-x86_64"
    local URL="${DOWNLOAD_BASE}/download/v${VERSION}/${FILENAME}"
    
    echo "⬇️  下载 Envoy v${VERSION}..."
    echo "   来源: ${URL}"
    
    if command -v wget &>/dev/null; then
        wget -q --show-progress -O "$ENVOY_BIN" "$URL"
    elif command -v curl &>/dev/null; then
        curl -#L -o "$ENVOY_BIN" "$URL"
    else
        echo "❌ 需要 wget 或 curl 来下载"
        exit 1
    fi
    
    if [ ! -f "$ENVOY_BIN" ]; then
        echo "❌ 下载失败！文件未创建"
        exit 1
    fi
    
    chmod +x "$ENVOY_BIN"
    echo "✅ 下载完成: ${ENVOY_BIN}"
}

verify_envoy() {
    echo "🔍 验证 Envoy 二进制..."
    if "${ENVOY_BIN}" --version 2>&1 | head -5; then
        echo "✅ 二进制验证通过！"
    else
        echo "❌ 二进制无法运行"
        exit 1
    fi
}

# ── 主流程 ──
echo "═══════════════════════════════"
echo "  AegisEngine — Envoy 下载工具"
echo "═══════════════════════════════"

# 如果已存在，询问是否重新下载
if [ -f "$ENVOY_BIN" ]; then
    echo "⚠️  Envoy 已存在: ${ENVOY_BIN}"
    "${ENVOY_BIN}" --version 2>&1 | head -3
    read -rp "是否重新下载？[y/N] " CONFIRM
    if [[ ! "$CONFIRM" =~ ^[Yy] ]]; then
        echo "跳过下载，使用现有二进制。"
        exit 0
    fi
fi

# 确定版本
VERSION="${ENVOY_VERSION:-$(get_latest_version)}"
echo "📦 目标版本: v${VERSION}"

# 下载
download_envoy "$VERSION"

# 验证
verify_envoy

echo ""
echo "🎉 Envoy v${VERSION} 已就绪！"
echo "   路径: ${ENVOY_BIN}"
echo "   启动: ${SCRIPT_DIR}/run_envoy.sh"
echo "   配置: ${SCRIPT_DIR}/envoy.yaml"
