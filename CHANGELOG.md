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