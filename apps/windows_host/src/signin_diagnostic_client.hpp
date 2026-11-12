#pragma once
#include "signin_diagnostic_inlet.hpp"

namespace host::signin::diagnostic::client {
enum class Code { prepared, refused, busy, expired, cancelled, failed, restorePending, unsupported, unverified };
struct Result {
  Code code=Code::refused;
  // Set BEFORE the first confirmation write. A failed/partial write is not
  // evidence that the service did nothing. Never automatically replay it.
  bool confirmationAttempted=false,observedResult=false;
};
// Explicit local operator action only. No endpoint/path/PID/hash/token inputs.
// Null stopEvent creates an owned event; all observations still expire in30s.
Result requestPreparation(uint32_t width,uint32_t height,HANDLE stopEvent=nullptr);
// Root owns the early main.cpp dispatch. First diagnostic mode is fixed1280x800.
// This parses intent only; requestPreparation independently authenticates self.
std::optional<Request> parseCommand(int argc,const wchar_t* const* argv);
}
