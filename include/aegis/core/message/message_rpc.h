/**
 * @file message_rpc.h
 * @deprecated Moved to aegis/rpc/rpc_message.h
 *
 * This is a transitional compatibility wrapper.
 * Include aegis/rpc/rpc_message.h directly in new code.
 */
#pragma once
#include "aegis/rpc/rpc_message.h"

// 桥接：让现有 aegis::core 命名空间下的代码能透明访问 aegis::rpc 的类型
namespace aegis::core
{
    using aegis::rpc::RpcId;
    using aegis::rpc::RpcResponseMsg;
    using aegis::rpc::RpcReplyMode;

    using aegis::rpc::AssignCampReq;
    using aegis::rpc::AssignCampRes;
    using aegis::rpc::CreateDungeonReq;
    using aegis::rpc::CreateDungeonRes;
    using aegis::rpc::JoinDungeonReq;
    using aegis::rpc::JoinDungeonRes;
    using aegis::rpc::LeaveDungeonReq;
    using aegis::rpc::LeaveDungeonRes;

    using aegis::rpc::RPCAssignCampMsg;
    using aegis::rpc::RPCCreateRoomMsg;
    using aegis::rpc::RPCTerminateRoomMsg;
    using aegis::rpc::RPCCreateDungeonMsg;
    using aegis::rpc::RPCJoinDungeonMsg;
    using aegis::rpc::RPCLeaveDungeonMsg;
}
