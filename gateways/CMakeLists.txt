cmake_minimum_required(VERSION 3.20)
project(AegisGateways)

# Envoy 配置验证（如果系统中安装了 envoy 二进制）
find_program(ENVOY_BIN envoy)
if(ENVOY_BIN)
    add_custom_target(validate_envoy_config
        COMMAND ${ENVOY_BIN} -c ${CMAKE_CURRENT_SOURCE_DIR}/envoy/envoy.yaml --mode validate
        COMMENT "🔍 Validating Envoy gateway configuration..."
    )
    message(STATUS "✅ Envoy binary found at: ${ENVOY_BIN}")
else()
    message(WARNING "⚠️  Envoy binary not found. Skipping config validation target.")
endif()
