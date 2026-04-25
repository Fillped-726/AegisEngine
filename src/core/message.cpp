#include "aegis/core/message/message.h"

namespace aegis::core
{
    MessageFinalizer g_message_finalizers[1024] = {nullptr};
}