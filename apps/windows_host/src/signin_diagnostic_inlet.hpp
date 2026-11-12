#pragma once
#include "signin_stream_state.hpp"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace host::signin::diagnostic {
inline constexpr const char* protocol="signin-diagnostic-inlet-v1";
// These fixed, protected values come from the installed release qualification,
// not an operator packet or a hash dynamically accepted from the current file.
struct ToolPin {
  std::filesystem::path path;
  Nonce sha256{};
  uint64_t bytes=0;
  std::string buildTag; // exact fixed PE FileVersion: major.minor.patch.revision
};
struct Request { uint32_t width=0,height=0; };
enum class Code { prepared, refused, busy, expired, cancelled, failed, restorePending, unsupported };
struct Result { bool ok=false;Code code=Code::refused; };
std::wstring endpoint(Process service);
std::optional<Request> parseRequest(const std::string&,const Nonce& challenge);
std::optional<ToolPin> parseToolPinMetadata(const std::string& bytes,
    const std::filesystem::path& fixedSibling,const std::string& compiledHostVersion);
// Fixed self-bin only, actual installed SYSTEM service only. Missing/old pins
// disable the inlet; current image bytes are never adopted as a new allow pin.
std::optional<ToolPin> loadInstalledToolPin();

class VerifiedOperator {
 public:
  VerifiedOperator()=default;
  VerifiedOperator(VerifiedOperator&&)noexcept=default;
  VerifiedOperator& operator=(VerifiedOperator&&)noexcept=default;
  VerifiedOperator(const VerifiedOperator&)=delete;
  VerifiedOperator& operator=(const VerifiedOperator&)=delete;
  bool current()const;
  Process identity()const;
  HANDLE processHandle()const; // borrowed query/wait handle; no injection rights
  uint32_t consoleSession()const;
  Nonce operation()const;
  uint64_t issuedTick()const;
  uint64_t deadlineTick()const;
  // Copies only the revocable live check, never creates another consent owner.
  std::function<bool()> consent()const;
 private:
  struct State;std::shared_ptr<State> state_;
  explicit VerifiedOperator(std::shared_ptr<State> state):state_(std::move(state)){}
  friend class Server;
  friend struct TestAccess; // defined only by the own-process test TU
};

// Existing boot service owns this object/thread/stop event. Constructor starts
// nothing. This endpoint never dispatches reset, capture, input or arbitrary CLI.
class Server {
 public:
  using Handler=std::function<Result(VerifiedOperator&&,const Request&)>;
  Server(Process service,ToolPin fixedTool):service_(service),tool_(std::move(fixedTool)){}
  bool run(HANDLE stopEvent,const Handler&);
 private:
  Process service_;ToolPin tool_;
  std::optional<VerifiedOperator> verify(HANDLE pipe,HANDLE stopEvent)const;
  static bool converse(HANDLE pipe,HANDLE stopEvent,VerifiedOperator,const Handler&);
  friend struct TestAccess;
};
}
