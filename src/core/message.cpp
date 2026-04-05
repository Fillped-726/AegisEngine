#include "aegis/core/message.h"

namespace aegis::core
{
    MessageFinalizer g_message_finalizers[256] = {nullptr};
}