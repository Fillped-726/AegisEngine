#pragma once

#include <memory>
#include <thread>
#include <string>
#include <grpcpp/grpcpp.h>
#include "ss_bridge.grpc.pb.h"
#include "battle_control_impl.h"
#include "common/aegisLog.h"

namespace aegis::rpc
{

    class RpcServerLoader
    {
    public:
        // 传入 RoomManager 的指针 (必须保证 RoomManager 活得比 RPC Server 长)
        RpcServerLoader(const std::string &addr, aegis::core::Actor *room_manager)
            : server_address_(addr), service_impl_(room_manager) {}

        ~RpcServerLoader()
        {
            Stop();
        }

        // 启动 gRPC Server (非阻塞，内部开线程)
        void Start()
        {
            if (running_)
                return;
            running_ = true;

            // 启动独立线程，防止卡死主线程
            server_thread_ = std::thread([this]()
                                         {
                grpc::ServerBuilder builder;
                
                // 监听地址 (e.g., "0.0.0.0:50051")
                // Insecure 意味着没有 SSL 证书，内网通信通常是安全的
                builder.AddListeningPort(server_address_, grpc::InsecureServerCredentials());
                
                // 注册服务
                builder.RegisterService(&service_impl_);
                
                // 构建并启动
                server_ = builder.BuildAndStart();
                
                if (server_) {
                    Log::instance().info("[RPC] Server listening on {}", server_address_);
                    // 阻塞等待，直到 Shutdown 被调用
                    server_->Wait(); 
                } else {
                    Log::instance().error("[RPC] Failed to start server on {}", server_address_);
                } });

            // 线程分离还是 join？建议在 Stop 中 join，这里先 detach 或者保留 handle
            // 为了安全退出，我们在 Stop 里处理
        }

        // 优雅退出
        void Stop()
        {
            if (!running_)
                return;
            running_ = false;

            Log::instance().info("[RPC] Shutting down...");

            if (server_)
            {
                // 给一个截止时间，强制关闭
                // 这里的 Context 是为了告诉 gRPC 把正在处理的请求处理完
                server_->Shutdown();
            }

            if (server_thread_.joinable())
            {
                server_thread_.join();
            }

            Log::instance().info("[RPC] Shutdown complete.");
        }

    private:
        std::string server_address_;
        BattleControlImpl service_impl_; // 我们的业务实现
        std::unique_ptr<grpc::Server> server_;
        std::thread server_thread_;
        std::atomic<bool> running_{false};
    };

}