# 构建与部署

> CMake + vcpkg + 依赖管理 + 启动流程

---

## 1. 构建系统

CMake + vcpkg manifest mode (`vcpkg.json`)。

### 构建命令

```bash
# Release
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Debug (ASan)
cmake -B out/build/asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build out/build/asan --parallel
```

### 重要目录

```
AegisEngine/
├── build/bin/              # 可执行文件输出
├── build/lib/              # 库文件输出
├── build/services/gate/    # GateServer 构建产物
├── vcpkg_installed/        # vcpkg 下载的依赖
└── logs/                   # 运行日志输出
```

---

## 2. 依赖项

| 库 | 用途 | 类型 |
|----|------|------|
| **Linux io_uring** (liburing) | 所有异步 I/O | 编译/运行必需 |
| **Protobuf** (3.x+) | 网络协议序列化 | vcpkg manifest |
| **BehaviorTree.CPP** (4.x) | NPC 行为树 | vcpkg manifest |
| **moodycamel::ConcurrentQueue** | 无锁队列 | 头文件 (header-only) |
| **spdlog** | 日志 | vcpkg manifest |
| **GTest** | 单元测试 | vcpkg (test only) |
| **Google Benchmark** | 性能基准 | vcpkg (test only) |

**环境要求**:
- GCC 13+ 或 Clang 16+
- C++20 标准
- Linux 5.10+ (io_uring)
- liburing-dev

**不支持 Windows/macOS 服务器**（io_uring 绑定 Linux）。

---

## 3. 启动流程

### 3.1 编译运行 GateServer

```bash
# 从项目根目录
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel -j$(nproc)

# 运行 GateServer
./build/services/gate/gate_server
```

### 3.2 启动顺序

GateServer::main():

```
1. 忽略 SIGPIPE
2. GateServer::init("logs/gate_server.log", 4)
   ├─ tune_fd_limit()             ← 调大文件描述符上限
   ├─ Log::init_config()          ← 初始化 spdlog 日志
   ├─ load_handlers()             ← 注册所有业务 Handler
   └─ Scheduler::start(4)         ← 启动 4 个 Worker 线程

3. GameApp::instance().init()
   ├─ 创建 RoomManager
   ├─ 创建主城 SceneActor (500x500)
   ├─ 创建并注册主城 NPC
   └─ 设置 default_scene_id()

4. GateServer::run(8888)
   └─ accept_loop(8888) 在 Worker 0 上接受连接
      └─ 每个连接分配到 Worker 0~3
```

### 3.3 配置

日志和服务器配置在 `logs/` 下。当前 GateServer 硬编码:
- Worker 数: 4（可以通过 `main.cpp` 修改）
- 监听端口: 8888
- 主城大小: 500x500，AOI cell 大小: 10

---

## 4. 测试

```bash
# 单元测试
./build/tests/unit/test_room_manager

# 集成测试
./build/tests/integration/test_aoi_grid

# 性能基准
./build/tests/benchmark/benchmark_robot
./build/tests/benchmark/bm_aoi_grid
```

### 测试文件结构

```
tests/
├── unit/
│   ├── test_ai.cpp              # 行为树测试
│   ├── test_room_manager.cpp    # RoomManager 测试
│   └── CMakeLists.txt
├── integration/
│   ├── test_aoi_grid.cpp        # AOI 网格集成测试
│   └── test_architecture.cpp    # 架构验证测试
└── benchmark/
    ├── benchmark_robot.cpp       # 机器人压测 (768 行)
    └── bm_aoi_grid.cpp           # AOI 网格性能基准
```

---

## 5. 项目演进路线 (Git 日志)

| Commit | 消息 | 内容 |
|--------|------|------|
| `3276af8` | 目录变更 | `core/` → `game/` 拆分，新增单元测试 |
| `486c3ea` | 断点 | RPC 协程化、SeqID、Packet 格式改写、benchmark 机器人 |
| `3dcff18` | 客户端和服务端连接+欢迎界面+加入营地 | 客服端连接闭环 |
| `f5dfc1c` | gate_server重构 | GameApp 抽取、RoomManager 重构、handler_loader 重写 |
| `80fe8c2` | message重构 | 6 文件拆分、uint16 type_id |
| `68395ec` | v2.0-v2.3 | 同步bug修复、行为树、攻击扣血、怪物 AI |
| `da7ede5` | thread-per-core | Worker 独立 io_uring 实例 |
| `d797971` | 架构升级 | 大幅重构 |
| `77c22f6` | actor+scheduler | Actor 模型基础框架 |
| `bccb713` | Stream to Packet | 协议层定稿 |
| `f07087b` | Safety Hardening & RAII | 安全性强化 |
