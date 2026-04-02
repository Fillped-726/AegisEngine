// include/aegis/common/actor_utils.h
#pragma once

#pragma once

namespace aegis::core
{
    // [DEPENDENCY: Actor state container, message payload]
    class Actor;
    class ActorMessage;

    // [INTENT: Enqueue msg to target actor & trigger execution/wake]
    // [SCOPE: Core engine transport logic; external to GateServer]
    void dispatch_msg(Actor *actor, ActorMessage *msg);
}