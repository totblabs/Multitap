// Root may COMPILE this translation unit with the Host's real toolchain.
// It deliberately has no main and MUST NOT run the real Windows adapter.
#include "../../apps/windows_host/src/windows_autologon_native.hpp"
static_assert(host::autologon::names.size() == 6, "fixed Winlogon allowlist");
