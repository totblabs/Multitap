#include "signin_worker_channel.hpp"
#include "pipe_packet.hpp"
#include <iostream>
#include <thread>

using namespace host;
using namespace host::signin;
using namespace host::signin::channel;
namespace {
unsigned checks = 0;
void expect(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
Init fixture() {
  Init out; auto& c = out.context;
  c.stream[0] = 1; c.service = {100, UINT64_MAX - 1}; c.serviceIsLocalSystem = true;
  c.lease = {"ROOT\\DISPLAY\\0000", 9}; c.leaseOwned = true;
  c.baseline[0] = 2; c.nonce[0] = 3; c.consoleSession = 1;
  c.workerGeneration = 7; c.mediaEpoch = UINT16_MAX;
  c.configuredSid = "S-1-5-21-1-2-3-1001"; c.ownedOutputMask = 1;
  c.assigned = c.clientOptIn = c.clientTransitions = c.signinRuntimeQualified = true;
  out.binding = {c.stream, c.service, {200, UINT64_MAX - 2}, c.lease, c.baseline, c.nonce,
    c.consoleSession, c.workerGeneration, c.mediaEpoch, Role::signin};
  out.target = {1, -7, 2, 3, 1920, 1080, -1920, 0, 1}; out.serviceQuery = 123;
  return out;
}
bool rejected(const Json& value) { try { (void)parseInit(value); return false; } catch (...) { return true; } }
void strictInit() {
  const auto source = fixture(); const auto packet = initPacket(source);
  const auto copy = parseInit(JsonParser::parse(packet));
  expect(sameBinding(copy.binding, source.binding), "64-bit process creation values must survive JSON exactly");
  expect(sameTarget(copy.target, source.target), "Signed geometry and adapter fields must round trip");
  expect(copy.context.configuredSid == source.context.configuredSid, "Configured account must round trip");
  expect(copy.serviceQuery == source.serviceQuery, "Delegated handle must round trip");
  auto changed = source; changed.context.assigned = false;
  bool refused = false; try { (void)initPacket(changed); } catch (...) { refused = true; }
  expect(refused, "Service cannot serialize missing assignment authority");
  for (unsigned i = 0; i < 18; ++i) {
    auto value = JsonParser::parse(packet); auto& object = std::get<Json::Object>(value.value);
    auto& binding = std::get<Json::Object>(object.at("binding").value);
    auto& target = std::get<Json::Object>(object.at("target").value);
    switch (i) {
      case 0: object.emplace("verified", Json{true}); break;
      case 1: object.erase("serviceQuery"); break;
      case 2: object.at("serviceQuery").value = 123.0; break;
      case 3: object.at("serviceQuery").value = std::string("0123"); break;
      case 4: object.at("serviceQuery").value = std::string("18446744073709551615"); break;
      case 5: binding.at("epoch").value = 65536.0; break;
      case 6: binding.at("epoch").value = 0.0; break;
      case 7: binding.at("generation").value = 1.5; break;
      case 8: binding.at("nonce").value = std::string(64, '0'); break;
      case 9: binding.at("baseline").value = std::string(64, 'F'); break;
      case 10: binding.at("role").value = std::string("user"); break;
      case 11: object.at("configuredSid").value = std::string("S-1-5-18"); break;
      case 12: target.at("mask").value = 3.0; break;
      case 13: target.at("width").value = 1921.0; break;
      case 14: target.at("height").value = 8194.0; break;
      case 15: target.at("left").value = 2147483648.0; break;
      case 16: std::get<Json::Object>(binding.at("service").value).at("created").value = std::string("18446744073709551616"); break;
      default: binding.at("worker") = binding.at("service"); break;
    }
    expect(rejected(value), "Malformed authority/epoch/geometry must be refused");
  }
  for (size_t i = 0; i < packet.size(); ++i) {
    bool failed = false;
    try { (void)parseInit(JsonParser::parse(packet.substr(0, i))); } catch (...) { failed = true; }
    expect(failed, "Every truncated initialization must be refused");
  }
  bool duplicate = false;
  try { (void)JsonParser::parse("{\"type\":1,\"type\":2}"); } catch (...) { duplicate = true; }
  expect(duplicate, "Duplicate JSON keys must not replace authority fields");
}
struct Authority final : native::LeaseAuthority {
  bool allowed = true; bool raises = false; mutable unsigned calls = 0;
  bool current(const Binding&, std::span<const native::OwnedTarget>) const override {
    ++calls; if (raises) throw std::runtime_error("Synthetic authority observation failure"); return allowed;
  }
};
std::string leaseRequest(const Init& value, const char* serial = "1") {
  const auto init = JsonParser::parse(initPacket(value));
  return "{\"type\":\"signin.worker.lease-v1\",\"serial\":" + jsonQuote(serial) +
    ",\"binding\":" + jsonObject(init.get("binding")) + ",\"target\":" + jsonObject(init.get("target")) + '}';
}
void authorityReplies() {
  const auto value = fixture(); const auto request = leaseRequest(value); Authority authority;
  const auto reply = JsonParser::parse(answerLease(JsonParser::parse(request), value.binding, {&value.target, 1}, authority));
  expect(reply.get("current").boolean() && authority.calls == 1, "Exact request must consult borrowed service authority");
  authority.allowed = false;
  expect(!JsonParser::parse(answerLease(JsonParser::parse(request), value.binding, {&value.target, 1}, authority)).get("current").boolean(), "Revocation must reach worker");
  authority.raises = true;
  expect(!JsonParser::parse(answerLease(JsonParser::parse(request), value.binding, {&value.target, 1}, authority)).get("current").boolean(), "Observation exceptions must deny");
  auto stale = value.binding; ++stale.generation;
  bool failed = false; const auto previous = authority.calls;
  try { (void)answerLease(JsonParser::parse(request), stale, {&value.target, 1}, authority); } catch (...) { failed = true; }
  expect(failed && authority.calls == previous, "Stale worker cannot ask for another generation's lease");
  auto other = value.target; ++other.targetId; failed = false;
  try { (void)answerLease(JsonParser::parse(request), value.binding, {&other, 1}, authority); } catch (...) { failed = true; }
  expect(failed && authority.calls == previous, "Another output cannot reach service authority");
}
// Real named-pipe I/O in this unelevated test process. Fake authority is explicit;
// this exercises framing, interleaving and reply correlation, not SYSTEM access.
void actualPipe(unsigned scenario) {
  const auto value = fixture(); const auto name = L"\\\\.\\pipe\\Multitap.Signin.ChannelTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(scenario);
  native::Handle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
  if (!server) throw std::runtime_error("Test named pipe creation failed");
  std::atomic<bool> stopped{false}; std::mutex writing; std::exception_ptr workerFailure;
  bool workerResult = false; unsigned deferred = 0;
  std::thread child([&] {
    try {
      native::Handle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
      if (!client) throw std::runtime_error("Test named pipe open failed");
      auto bound = value.binding; bound.service.pid = GetCurrentProcessId();
      channel::WorkerLease lease(client.get(), bound, value.target, stopped, writing,
        [&](const Json& item) { ++deferred; return item.get("type").string() == "session.control.test"; });
      workerResult = lease.current(bound, {&value.target, 1});
    } catch (...) { workerFailure = std::current_exception(); }
  });
  struct Join { std::thread& thread; ~Join() { if (thread.joinable()) thread.join(); } } join{child};
  native::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); OVERLAPPED connection{}; connection.hEvent = event.get();
  const bool connected = ConnectNamedPipe(server.get(), &connection) != FALSE;
  const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
  if (!connected && error == ERROR_IO_PENDING) {
    if (WaitForSingleObject(event.get(), 2000) != WAIT_OBJECT_0) { CancelIoEx(server.get(), &connection); DWORD count = 0; GetOverlappedResult(server.get(), &connection, &count, TRUE); throw std::runtime_error("Test connection timeout"); }
    DWORD count = 0; if (!GetOverlappedResult(server.get(), &connection, &count, FALSE)) throw std::runtime_error("Test connection failed");
  } else if (!connected && error != ERROR_PIPE_CONNECTED) throw std::runtime_error("Test connection failed");
  bool text = false; std::vector<uint8_t> bytes;
  if (!readPipePacket(server.get(), text, bytes, stopped, std::chrono::seconds(2)) || !text) throw std::runtime_error("Test lease request missing");
  auto bound = value.binding; bound.service.pid = GetCurrentProcessId(); Authority authority;
  authority.allowed = scenario != 1;
  auto reply = JsonParser::parse(answerLease(JsonParser::parse(std::string(bytes.begin(), bytes.end())), bound, {&value.target, 1}, authority));
  if (scenario == 2) std::get<Json::Object>(reply.value).at("serial").value = std::string("2");
  if (scenario == 3) {
    const std::string control = "{\"type\":\"session.control.test\"}";
    if (!pipePacket(server.get(), true, {control.begin(), control.end()}, stopped, writing)) throw std::runtime_error("Test interleave write failed");
  }
  if (scenario == 4) std::get<Json::Object>(reply.value).at("current").value = std::string("true");
  const auto output = jsonObject(reply);
  if (!pipePacket(server.get(), true, {output.begin(), output.end()}, stopped, writing)) throw std::runtime_error("Test reply write failed");
  child.join(); if (workerFailure) std::rethrow_exception(workerFailure);
  expect(workerResult == (scenario == 0 || scenario == 3), "Actual pipe must deny revocation, stale replies and wrong types");
  if (scenario == 3) expect(deferred == 1, "Interleaved control must remain owned by the active child loop");
}
}
int main() {
  try {
    strictInit(); authorityReplies(); for (unsigned scenario = 0; scenario < 5; ++scenario) actualPipe(scenario);
    std::cout << "{\"kind\":\"signin-worker-channel-tests\",\"checks\":" << checks << ",\"actualLocalPipeCases\":5,\"systemWorkersSpawned\":0,\"captureCalls\":0,\"inputCalls\":0,\"runtimeQualified\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
