#!/bin/bash

# ==========================================
# Aegis Protocol Generator (SSP Hermetic Build)
# ==========================================

ROOT_DIR=$(pwd)
PROTO_SRC="$ROOT_DIR/shared/proto"
CPP_OUT="$ROOT_DIR/shared/cpp_out"
GO_OUT="$ROOT_DIR/shared/pb"
BUILD_DIR="$ROOT_DIR/build"

echo "📍 Protocol Source: $PROTO_SRC"

# ---------------------------------------------------------
# [关键修改] 定义本地编译工具的路径
# 这些工具只有在你运行过 cmake build 后才会存在
# ---------------------------------------------------------

# 1. 查找 protoc 
# 逻辑：找名字以 protoc 开头的文件 -> 排除 CMake 临时文件 -> 排除 protoc-gen 插件 -> 取第一个
PROTOC_TOOL=$(find "$BUILD_DIR" -name "protoc*" -type f -executable | grep -v "CMakeFiles" | grep -v "protoc-gen" | head -n 1)

# 2. 查找 grpc_cpp_plugin
GRPC_PLUGIN_TOOL=$(find "$BUILD_DIR" -name "grpc_cpp_plugin" -type f -executable | grep -v "CMakeFiles" | head -n 1)

# 3. 严格检查工具是否存在
if [ ! -f "$PROTOC_TOOL" ] || [ ! -f "$GRPC_PLUGIN_TOOL" ]; then
    echo "❌ CRITICAL ERROR: Local build tools not found!"
    echo "   You must build the toolchain first."
    echo "   Please run this command in root:"
    echo "   cmake --build build --target protoc grpc_cpp_plugin -j\$(nproc)"
    exit 1
fi

echo "🔧 Using Hermetic Tools:"
echo "   Protoc: $PROTOC_TOOL"
echo "   Plugin: $GRPC_PLUGIN_TOOL"

# ---------------------------------------------------------
# 清理旧文件
rm -rf "$CPP_OUT"
rm -rf "$GO_OUT"
mkdir -p "$CPP_OUT"
mkdir -p "$GO_OUT"

# ---------------------------------------------------------
# 4. 生成 C++ 代码 (使用本地工具)
# ---------------------------------------------------------
echo "⚙️  Generating C++ Stubs..."

"$PROTOC_TOOL" \
       -I="$PROTO_SRC" \
       --cpp_out="$CPP_OUT" \
       --grpc_out="$CPP_OUT" \
       --plugin=protoc-gen-grpc="$GRPC_PLUGIN_TOOL" \
       "$PROTO_SRC"/*.proto

if [ $? -ne 0 ]; then
    echo "❌ C++ Generation Failed"
    exit 1
fi

# # ---------------------------------------------------------
# # 5. 生成 Go 代码
# # (Go 依然可以用系统插件，但建议用新版 protoc 驱动)
# # ---------------------------------------------------------
# echo "⚙️  Generating Go Stubs..."
# MODULE_NAME="github.com/Fillped-726/AegisEngine"

# # 注意：这里也改用了 $PROTOC_TOOL，确保解析 proto 文件的行为一致
# "$PROTOC_TOOL" \
#        -I="$PROTO_SRC" \
#        --go_out="$GO_OUT" \
#        --go_opt=module=$MODULE_NAME \
#        --go-grpc_out="$GO_OUT" \
#        --go-grpc_opt=module=$MODULE_NAME \
#        "$PROTO_SRC"/*.proto

# if [ $? -ne 0 ]; then
#     echo "❌ Go Generation Failed"
#     exit 1
# fi

echo "✅ All Contracts Signed & Stubs Generated!"
echo "   C++ Path: $CPP_OUT"
# echo "   Go Path:  $GO_OUT"