#include "ss_bridge.grpc.pb.h"
#include "aegis/core/message.h"   // 你定义的 RpcMessage 所在头文件
#include "aegis/core/scheduler.h" // 调度器

using grpc::ServerContext;
using grpc::Status;

class BattleControlImpl final : public aegis::ss::bridge::BattleControl::Service
{
private:
    // 目标 Actor 指针 (RoomManager)
    // 必须确保该 Actor 在 gRPC 服务运行期间存活
    aegis::core::Actor *room_manager_;

public:
    // 构造时传入负责处理房间逻辑的 Actor
    explicit BattleControlImpl(aegis::core::Actor *room_manager)
        : room_manager_(room_manager) {}

    // 1. 实现 CreateRoom
    Status CreateRoom(ServerContext *context,
                      const aegis::ss::bridge::CreateRoomReq *request,
                      aegis::ss::bridge::CreateRoomRes *response) override
    {
        // A. 构造 Actor 消息 (使用 new，因为要传递指针)
        auto *msg = new aegis::core::RPCCreateRoomMsg(*request);

        // B. 获取 Future (必须在发送前获取)
        auto future = msg->promise.get_future();

        // C. 发送消息给 RoomManager Actor
        // 逻辑复用你提供的: push -> dispatch
        if (room_manager_ && room_manager_->push(msg))
        {
            aegis::core::Scheduler::instance().dispatch(room_manager_);
        }
        else
        {
            // 发送失败 (队列满或 Actor 已销毁)
            delete msg; // 必须手动销毁，否则泄漏
            return Status(grpc::UNAVAILABLE, "RoomManager unavailable");
        }

        // D. 阻塞等待结果 (这是 gRPC 线程，阻塞是安全的)
        // 这里的 get() 会一直等到 Actor 执行 msg.Reply()
        *response = future.get();

        return Status::OK;
    }

    // 2. 实现 TerminateRoom
    Status TerminateRoom(ServerContext *context,
                         const aegis::ss::bridge::TerminateRoomReq *request,
                         aegis::ss::bridge::TerminateRoomRes *response) override
    {
        auto *msg = new aegis::core::RPCTerminateRoomMsg(*request);
        auto future = msg->promise.get_future();

        if (room_manager_ && room_manager_->push(msg))
        {
            aegis::core::Scheduler::instance().dispatch(room_manager_);
        }
        else
        {
            delete msg;
            return Status(grpc::UNAVAILABLE, "RoomManager unavailable");
        }

        *response = future.get();
        return Status::OK;
    }
};