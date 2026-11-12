#pragma once
#include "machine.hpp"
#include "json_object.hpp"
#include "signin_backend_policy.hpp"

namespace host::signin {
// Always OFF: the in-stream sign-in screen never
// worked end to end; a signed-out or locked Windows is reached through the
// Proxmox console or a restart with automatic login. A saved true is ignored,
// so this Host neither negotiates nor offers it, nor restores leftovers for it.
inline bool signinStreamingEnabled(const Json&){return false;}
}
