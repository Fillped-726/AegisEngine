# Changelog (变更日志)

本项目的所有显著更改都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
并且本项目遵守 [Semantic Versioning](https://semver.org/lang/zh-CN/) (语义化版本控制)。

## [Unreleased] - 待发布
> 这里通常记录在 dev 分支开发中、尚未合并到 main 的功能。
> 例如：即将进行的 RAII 重构和 Protobuf 协议支持。

## [v0.1.0] - 2026-01-01
### Added (新增)
- **Core Runtime**: 引入 `liburing` 依赖，封装了基于 `io_uring` 的核心异步运行时 (`Ring` 类)。
- **Coroutine Support**: 实现了 C++20 协程基础设施，包括 `Task` (Promise Type) 和基础 `Awaiter` 机制。
- **Async I/O**: 实现了 Socket 的异步操作封装，支持 `co_await` 调用：
    - `AsyncAccept`: 异步接收连接。
    - `AsyncRead` / `AsyncWrite`: 异步读写数据。
- **Network**: 封装了 RAII 风格的 `Socket` 类，支持 Bind, Listen 及基础错误处理。
- **Demo**: 实现了一个单线程、基于 Proactor 模式的 **TCP Echo Server**，支持并发连接回显。
- **Build**: 添加了 CMake 构建系统配置 (CMakeLists.txt)。

### Performance (性能)
- 初步验证了 Zero-Copy (零拷贝) 的数据提交机制（通过 io_uring SQ/CQ 队列）。
- 实现了无回调 (Callback-free) 的线性异步编程模型。

## [v0.1.1] - 2026-01-02
### Security (安全)
- 修复了 `Awaiter` 类的内存布局隐患，显式继承 `BaseAwaiter` 以确保 ABI 安全。
- 引入 `UniqueFd` RAII 包装类，防止 Socket 文件描述符泄漏。
- 增强了 `bind/listen` 阶段的错误处理，防止端口占用导致未捕获异常。

### Refactor (重构)
- 移除了核心调度器中危险的 `reinterpret_cast`，用`static_cast`进行代替。

## [v0.2.0] - 2026-01-08

### 🚀 Major Features (核心特性)
- **Protocol Layer (协议层)**: 实现了从“原始字节流”到“逻辑数据包”的架构升级。
    - 引入 `Packet` 类：封装了包头 (Length + MsgID) 解析与大端序转换，提供面向对象的 `parse()` 接口。
    - 引入 `Connection` 类：接管 Socket 所有权，维护会话状态。
- **TCP Sticky/Partial Packet Handling (粘包/半包处理)**:
    - 实现了 `Stash` (暂存区) 机制：在 `Connection` 层自动处理 TCP 数据流切分。
    - 实现了 `read_exactly` 语义：确保读取完整的数据包后再返回给业务层。
- **Generic Coroutine Support (泛型协程)**:
    - 重构 `Task` 为模板类 `Task<T>`，支持 `co_await` 返回具体对象 (如 `std::unique_ptr<Packet>`)。
    - 引入 `DetachedTask`：专门用于 `handle_client` 等即发即忘 (Fire-and-Forget) 的顶层协程，防止句柄过早销毁。

### 🐛 Bug Fixes & Stability (修复与稳定性)
- **Critical Crash Fixes**:
    - 修复了协程返回临时对象导致句柄销毁引发的 Segmentation Fault。
    - 修复了 `Double Resume` (重复唤醒) 问题：将协程调度从 Eager Execution (立即执行) 改为 **Lazy Execution (懒执行)**。
- **Compilation**: 修复了 `Task<void>` 特化版本的编译错误，引入 `if constexpr` 优化模板分支。
- **Resource Safety**: 修正了 `Socket` 类的移动语义 (Move Semantics)，使用 `std::exchange` 防止文件描述符 Double Close。

### 🛠 Improvements (改进)
- **Test Suite**: 升级 Python 测试脚本。
    - 支持 Protobuf 协议收发。
    - 新增 **粘包测试**：模拟一次发送多个数据包，验证服务器拆包能力。
- **Code Style**: 全面现代 C++ 化，移除手动指针偏移和 `reinterpret_cast`，改用 RAII 和类型安全的解析方式。