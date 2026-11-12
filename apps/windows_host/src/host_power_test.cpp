// PC power (pc-power-v1): hello, request decisions and fixed refusal codes
// with a substituted Windows call. This fixture is compiled with
// MULTITAP_HOST_POWER_TEST, so the real shutdown path does not even exist in
// it: nothing here can restart or shut down a machine. The only Windows read
// is the read-only wired-adapter listing at the end.
#include "host_power.hpp"
#include <fstream>
#include <iostream>
#include <regex>

namespace {
int failures = 0, checks = 0;
void require(bool value, const char* name) { ++checks; if (!value) { ++failures; std::cout << "FAILED: " << name << '\n'; } }
const std::string id = "0b0c0d0e-1111-4222-8333-444455556666";
host::Json request(const std::string& action, const std::string& requestId = id) {
  return host::JsonParser::parse("{\"type\":\"host.power.request\",\"requestId\":\"" + requestId + "\",\"action\":\"" + action + "\"}");
}
struct Fixture {
  std::vector<bool> calls; std::vector<std::string> sent; DWORD answer = ERROR_SUCCESS; bool fail = false;
  host::HostPower power{{[this](bool restart) -> DWORD { calls.push_back(restart); if (fail) throw std::runtime_error("fixture"); return answer; },
                         [] { return std::vector<host::WakeAdapter>{{"D8:BB:C1:12:34:56", true, std::nullopt}, {"00:11:22:33:44:55", false, false}}; }},
                        [this](const std::string& message) { sent.push_back(message); }};
};
std::string result(const std::string& state, const std::string& code) {
  return "{\"type\":\"host.power.result\",\"requestId\":\"" + id + "\",\"state\":\"" + state + "\",\"code\":\"" + code + "\"}";
}
}  // namespace

int main() {
  {
    Fixture f;
    require(f.power.hello() == "{\"type\":\"host.power.hello\",\"protocol\":\"pc-power-v1\",\"actions\":[\"shutdown\",\"reboot\"],\"wake\":[{\"mac\":\"D8:BB:C1:12:34:56\",\"wakeEnabled\":true,\"magicPacket\":null},{\"mac\":\"00:11:22:33:44:55\",\"wakeEnabled\":false,\"magicPacket\":false}]}", "hello announces both actions and the wired adapters");
    host::HostPower unreadable({[](bool) -> DWORD { return ERROR_SUCCESS; }, []() -> std::vector<host::WakeAdapter> { throw std::runtime_error("fixture"); }}, [](const std::string&) {});
    require(unreadable.hello() == "{\"type\":\"host.power.hello\",\"protocol\":\"pc-power-v1\",\"actions\":[\"shutdown\",\"reboot\"]}", "unreadable adapters omit the list so the broker keeps the last one");
    require(host::hostPowerHello(std::vector<host::WakeAdapter>{}).ends_with(",\"wake\":[]}"), "no wired adapter is an empty list");
  }
  {
    Fixture f;
    f.power.request(request("reboot"));
    require(f.calls == std::vector<bool>{true}, "다시 시작 asks Windows to restart once");
    require(f.sent == std::vector<std::string>{result("accepted", "")}, "accepted is reported");
    f.power.request(request("shutdown"));
    require(f.calls.size() == 1, "after an accepted request Windows is not asked again");
    require(f.sent.back() == result("refused", "shutdown_in_progress"), "a second request is refused as in progress");
  }
  {
    Fixture f;
    f.power.request(request("shutdown"));
    require(f.calls == std::vector<bool>{false}, "종료 asks Windows to power off, not restart");
  }
  {
    const std::vector<std::pair<DWORD, const char*>> answers{{ERROR_ACCESS_DENIED, "privilege_unavailable"}, {ERROR_PRIVILEGE_NOT_HELD, "privilege_unavailable"}, {ERROR_NOT_ALL_ASSIGNED, "privilege_unavailable"},
      {ERROR_SHUTDOWN_IN_PROGRESS, "shutdown_in_progress"}, {ERROR_SHUTDOWN_IS_SCHEDULED, "shutdown_in_progress"}, {ERROR_SHUTDOWN_USERS_LOGGED_ON, "other_users_signed_in"},
      {ERROR_MACHINE_LOCKED, "machine_locked"}, {ERROR_INVALID_PARAMETER, "shutdown_failed"}, {ERROR_NOT_READY, "shutdown_failed"}};
    for (const auto& [answer, code] : answers) {
      Fixture f; f.answer = answer;
      f.power.request(request("reboot"));
      require(f.sent == std::vector<std::string>{result("refused", code)}, code);
      f.answer = ERROR_SUCCESS; f.power.request(request("reboot"));
      require(f.calls.size() == 2 && f.sent.back() == result("accepted", ""), "a refused request does not block a later one");
    }
    Fixture thrown; thrown.fail = true; thrown.power.request(request("reboot"));
    require(thrown.sent == std::vector<std::string>{result("refused", "shutdown_failed")}, "an exception is a fixed failure, never its text");
  }
  {
    Fixture f;
    for (const auto* action : {"stop", "reset", "", "Reboot"}) f.power.request(request(action));
    require(f.calls.empty(), "unknown actions never reach Windows");
    require(f.sent.size() == 4 && f.sent[0] == result("refused", "invalid_request"), "unknown actions are refused as invalid_request");
    f.power.request(request("reboot", "not-a-request-id"));
    f.power.request(request("reboot", "0B0C0D0E-1111-4222-8333-444455556666"));
    require(f.calls.empty() && f.sent.size() == 4, "a request without a valid id is neither run nor answered");
  }
  {
    // The real request path: an ordinary shutdown or restart, never a flag
    // that closes applications without asking.
    std::ifstream file(MULTITAP_HOST_POWER_SOURCE); std::string line, code;
    while (std::getline(file, line)) { const auto at = line.find("//"); code += (at == std::string::npos ? line : line.substr(0, at)) + '\n'; }
    require(code.find("InitiateShutdownW(nullptr, message, 0, flags, reason)") != std::string::npos, "the source was read");
    for (const auto* flag : {"SHUTDOWN_FORCE_OTHERS", "SHUTDOWN_FORCE_SELF", "SHUTDOWN_GRACE_OVERRIDE", "SHUTDOWN_INSTALL_UPDATES", "EWX_FORCE"}) require(code.find(flag) == std::string::npos, flag);
    require(code.find("restart ? SHUTDOWN_RESTART : SHUTDOWN_POWEROFF") != std::string::npos, "only restart or power off");
    require(code.find("SHTDN_REASON_FLAG_PLANNED") != std::string::npos, "a planned reason code");
  }
  {
    // Read only: this machine's wired adapters (no setting is changed).
    const auto adapters = host::windowsWakeAdapters();
    const std::regex mac("[0-9A-F]{2}(:[0-9A-F]{2}){5}");
    bool valid = adapters.size() <= 8;
    for (const auto& adapter : adapters) valid = valid && std::regex_match(adapter.mac, mac);
    require(valid, "real wired adapters are at most eight upper-case MACs");
    std::cout << "wired adapters on this machine: " << adapters.size() << '\n';
    require(host::hostPowerHello(adapters).starts_with("{\"type\":\"host.power.hello\""), "the real list forms a hello");
  }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
