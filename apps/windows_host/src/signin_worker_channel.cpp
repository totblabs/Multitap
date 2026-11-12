#include "signin_worker_channel.hpp"
#include "pipe_packet.hpp"
#include <charconv>
#include <limits>

namespace host::signin::channel {
namespace {
[[noreturn]] void invalid() { throw std::runtime_error("Invalid private sign-in channel packet"); }
void fields(const Json& value, std::initializer_list<const char*> names) {
  const auto* object = std::get_if<Json::Object>(&value.value);
  if (!object || object->size() != names.size()) invalid();
  for (const char* name : names) if (!object->count(name)) invalid();
}
uint32_t number(const Json& value, uint32_t low, uint32_t high) {
  const auto* actual = std::get_if<double>(&value.value);
  if (!actual || !std::isfinite(*actual) || *actual < low || *actual > high || std::floor(*actual) != *actual) invalid();
  return uint32_t(*actual);
}
int32_t signedNumber(const Json& value) {
  const auto* actual = std::get_if<double>(&value.value);
  if (!actual || !std::isfinite(*actual) || *actual < INT32_MIN || *actual > INT32_MAX || std::floor(*actual) != *actual) invalid();
  return int32_t(*actual);
}
uint64_t integer(const Json& value) {
  const auto* actual = std::get_if<std::string>(&value.value);
  uint64_t result = 0;
  if (!actual || actual->empty() || actual->size() > 20 || (actual->size() > 1 && actual->front() == '0')) invalid();
  const auto parsed = std::from_chars(actual->data(), actual->data() + actual->size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != actual->data() + actual->size()) invalid();
  return result;
}
template<size_t N> std::string hex(const std::array<uint8_t, N>& bytes) {
  constexpr char chars[] = "0123456789abcdef";
  std::string out; out.reserve(N * 2);
  for (auto byte : bytes) { out += chars[byte >> 4]; out += chars[byte & 15]; }
  return out;
}
template<size_t N> std::array<uint8_t, N> unhex(const Json& value) {
  const auto text = value.string();
  if (text.size() != N * 2) invalid();
  std::array<uint8_t, N> out{};
  const auto digit = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return uint8_t(c - '0');
    if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
    invalid();
  };
  for (size_t i = 0; i < N; ++i) out[i] = uint8_t((digit(text[i * 2]) << 4) | digit(text[i * 2 + 1]));
  if (!nonzero(out)) invalid();
  return out;
}
std::string processPacket(const Process& process) {
  return "{\"pid\":" + std::to_string(process.pid) + ",\"created\":" + jsonQuote(std::to_string(process.created)) + '}';
}
Process process(const Json& value) {
  fields(value, {"pid", "created"});
  const Process result{number(value.get("pid"), 1, UINT32_MAX - 1), integer(value.get("created"))};
  if (!valid(result)) invalid();
  return result;
}
std::string bindingPacket(const Binding& binding) {
  return "{\"stream\":" + jsonQuote(hex(binding.stream)) + ",\"service\":" + processPacket(binding.service) +
    ",\"worker\":" + processPacket(binding.worker) + ",\"instance\":" + jsonQuote(binding.lease.instance) +
    ",\"leaseGeneration\":" + std::to_string(binding.lease.generation) + ",\"baseline\":" + jsonQuote(hex(binding.baseline)) +
    ",\"nonce\":" + jsonQuote(hex(binding.nonce)) + ",\"console\":" + std::to_string(binding.consoleSession) +
    ",\"generation\":" + std::to_string(binding.generation) + ",\"epoch\":" + std::to_string(binding.mediaEpoch) + ",\"role\":\"signin\"}";
}
Binding binding(const Json& value) {
  fields(value, {"stream", "service", "worker", "instance", "leaseGeneration", "baseline", "nonce", "console", "generation", "epoch", "role"});
  Binding out;
  out.stream = unhex<16>(value.get("stream")); out.service = process(value.get("service")); out.worker = process(value.get("worker"));
  out.lease = {value.get("instance").string(), number(value.get("leaseGeneration"), 1, UINT32_MAX)};
  out.baseline = unhex<32>(value.get("baseline")); out.nonce = unhex<32>(value.get("nonce"));
  out.consoleSession = number(value.get("console"), 1, UINT32_MAX - 1);
  out.generation = number(value.get("generation"), 1, UINT32_MAX);
  out.mediaEpoch = uint16_t(number(value.get("epoch"), 1, UINT16_MAX)); out.role = Role::signin;
  if (!valid(out.lease) || out.worker == out.service || value.get("role").string() != "signin") invalid();
  return out;
}
std::string targetPacket(const native::OwnedTarget& target) {
  return "{\"adapterLow\":" + std::to_string(target.adapterLow) + ",\"adapterHigh\":" + std::to_string(target.adapterHigh) +
    ",\"source\":" + std::to_string(target.sourceId) + ",\"target\":" + std::to_string(target.targetId) +
    ",\"width\":" + std::to_string(target.width) + ",\"height\":" + std::to_string(target.height) +
    ",\"left\":" + std::to_string(target.left) + ",\"top\":" + std::to_string(target.top) + ",\"mask\":1}";
}
native::OwnedTarget target(const Json& value) {
  fields(value, {"adapterLow", "adapterHigh", "source", "target", "width", "height", "left", "top", "mask"});
  native::OwnedTarget out;
  out.adapterLow = number(value.get("adapterLow"), 0, UINT32_MAX); out.adapterHigh = signedNumber(value.get("adapterHigh"));
  out.sourceId = number(value.get("source"), 0, UINT32_MAX); out.targetId = number(value.get("target"), 0, UINT32_MAX);
  out.width = number(value.get("width"), 16, 8192); out.height = number(value.get("height"), 16, 8192);
  out.left = signedNumber(value.get("left")); out.top = signedNumber(value.get("top"));
  out.mask = uint8_t(number(value.get("mask"), 1, 1));
  if ((!out.adapterLow && !out.adapterHigh) || (out.width & 1) || (out.height & 1) || uint64_t(out.width) * out.height > 33554432) invalid();
  return out;
}
}
bool sameBinding(const Binding& a, const Binding& b) {
  return a.stream == b.stream && a.service == b.service && a.worker == b.worker && a.lease == b.lease &&
    a.baseline == b.baseline && a.nonce == b.nonce && a.consoleSession == b.consoleSession &&
    a.generation == b.generation && a.mediaEpoch == b.mediaEpoch && a.role == b.role;
}
bool sameTarget(const native::OwnedTarget& a, const native::OwnedTarget& b) { return a == b; }
std::string initPacket(const Init& value) {
  if (!native::validBinding(value.binding, value.context) || value.target.mask != 1 || !value.serviceQuery || value.serviceQuery >= UINTPTR_MAX) invalid();
  const auto out = "{\"type\":\"signin.worker.bind-v1\",\"binding\":" + bindingPacket(value.binding) +
    ",\"configuredSid\":" + jsonQuote(value.context.configuredSid) + ",\"target\":" + targetPacket(value.target) +
    ",\"serviceQuery\":" + jsonQuote(std::to_string(value.serviceQuery)) +
    (value.context.unknownAccountAllowed ? ",\"unknownAccountAllowed\":true}" : "}");
  // Round-trip through the same strict parser so a bad service observation is
  // refused locally, before it can launch a capture with a different contract.
  (void)parseInit(JsonParser::parse(out)); return out;
}
Init parseInit(const Json& value) {
  const auto* object=std::get_if<Json::Object>(&value.value);
  const bool explicitUnknown=object&&object->count("unknownAccountAllowed");
  if(explicitUnknown)fields(value, {"type", "binding", "configuredSid", "target", "serviceQuery", "unknownAccountAllowed"});
  else fields(value, {"type", "binding", "configuredSid", "target", "serviceQuery"});
  if (value.get("type").string() != "signin.worker.bind-v1") invalid();
  Init out; out.binding = binding(value.get("binding")); out.target = target(value.get("target"));
  out.serviceQuery = integer(value.get("serviceQuery"));
  if (!out.serviceQuery || out.serviceQuery >= UINTPTR_MAX) invalid();
  auto& c = out.context; const auto& b = out.binding;
  c.stream = b.stream; c.service = b.service; c.serviceIsLocalSystem = true;
  c.lease = b.lease; c.leaseOwned = true; c.baseline = b.baseline; c.nonce = b.nonce;
  c.consoleSession = b.consoleSession; c.workerGeneration = b.generation; c.mediaEpoch = b.mediaEpoch;
  c.ownedOutputMask = 1; c.configuredSid = value.get("configuredSid").string();
  if(explicitUnknown){const auto* allowed=std::get_if<bool>(&value.get("unknownAccountAllowed").value);if(!allowed)invalid();c.unknownAccountAllowed=*allowed;}
  // These are private service admission claims, NOT runtime proof. The child
  // must authenticate this packet, then pass the live Permit checks. This
  // parser is deliberately not used by any public capability/offer path.
  c.assigned = c.clientOptIn = c.clientTransitions = c.signinRuntimeQualified = true;
  if (!native::validBinding(b, c)) invalid();
  return out;
}
native::Handle acceptInitService(const Init& value, HANDLE pipe, uint32_t servicePid) {
  ULONG peer = 0; DWORD flags = 0;
  if (!GetNamedPipeServerProcessId(pipe, &peer) || peer != servicePid || peer != value.binding.service.pid ||
      value.binding.worker != native::processIdentity(GetCurrentProcess())) invalid();
  const HANDLE service = reinterpret_cast<HANDLE>(uintptr_t(value.serviceQuery));
  if (!GetHandleInformation(service, &flags) || (flags & HANDLE_FLAG_INHERIT) ||
      native::processIdentity(service) != value.binding.service || WaitForSingleObject(service, 0) != WAIT_TIMEOUT) invalid();
  return native::Handle(service);
}
static std::string answerLeaseKind(const Json& value, const Binding& expected, std::span<const native::OwnedTarget> targets,
                        const native::LeaseAuthority& authority, bool releaseOnly) {
  fields(value, {"type", "serial", "binding", "target"});
  if (value.get("type").string() != (releaseOnly ? "signin.worker.release-lease-v1" : "signin.worker.lease-v1")) invalid();
  const auto sequence = integer(value.get("serial"));
  const auto requested = binding(value.get("binding")); const auto output = target(value.get("target"));
  if (!sequence || targets.size() != 1 || !sameBinding(requested, expected) || !sameTarget(output, targets.front())) invalid();
  // Authority failure produces only a denial; exceptions are never an allow.
  bool current = false; try { current = releaseOnly ? authority.currentForRelease(expected, targets) : authority.current(expected, targets); } catch (...) {}
  return "{\"type\":" + jsonQuote(releaseOnly ? "signin.worker.release-lease.result-v1" : "signin.worker.lease.result-v1") + ",\"serial\":" + jsonQuote(std::to_string(sequence)) +
    ",\"binding\":" + bindingPacket(expected) + ",\"current\":" + (current ? "true" : "false") + '}';
}
std::string answerLease(const Json& value, const Binding& expected, std::span<const native::OwnedTarget> targets, const native::LeaseAuthority& authority) {
  return answerLeaseKind(value, expected, targets, authority, false);
}
std::string answerReleaseLease(const Json& value, const Binding& expected, std::span<const native::OwnedTarget> targets, const native::LeaseAuthority& authority) {
  return answerLeaseKind(value, expected, targets, authority, true);
}
WorkerLease::WorkerLease(HANDLE pipe, const Binding& binding, const native::OwnedTarget& target,
                         const std::atomic<bool>& stopped, std::mutex& writing, Defer defer)
  : pipe_(pipe), binding_(binding), target_(target), stopped_(stopped), writing_(writing), defer_(std::move(defer)) {}
bool WorkerLease::current(const Binding& request, std::span<const native::OwnedTarget> targets) const { return query(request, targets, false); }
bool WorkerLease::currentForRelease(const Binding& request, std::span<const native::OwnedTarget> targets) const { return query(request, targets, true); }
bool WorkerLease::query(const Binding& request, std::span<const native::OwnedTarget> targets, bool releaseOnly) const {
  std::lock_guard queryLock(querying_);
  if (stopped_ || !sameBinding(request, binding_) || targets.size() != 1 || targets.front() != target_ || serial_ == UINT64_MAX) return false;
  const auto serial = ++serial_;
  const auto packet = "{\"type\":" + jsonQuote(releaseOnly ? "signin.worker.release-lease-v1" : "signin.worker.lease-v1") + ",\"serial\":" + jsonQuote(std::to_string(serial)) +
    ",\"binding\":" + bindingPacket(binding_) + ",\"target\":" + targetPacket(target_) + '}';
  const std::atomic<int64_t> deadline{streamMonotonicMillis() + 250};
  try {
    ULONG peer = 0;
    if (!GetNamedPipeServerProcessId(pipe_, &peer) || peer != binding_.service.pid ||
        !pipePacket(pipe_, true, {packet.begin(), packet.end()}, stopped_, writing_, &deadline)) return false;
    unsigned deferred = 0;
    while (!stopped_ && streamMonotonicMillis() < deadline) {
      bool text = false; std::vector<uint8_t> bytes;
      if (!readPipePacket(pipe_, text, bytes, stopped_, std::chrono::milliseconds(250), &deadline) || !text || bytes.size() > 8192) return false;
      const auto value = JsonParser::parse(std::string(bytes.begin(), bytes.end()));
      if (value.get("type").string() != (releaseOnly ? "signin.worker.release-lease.result-v1" : "signin.worker.lease.result-v1")) {
        if (++deferred > 32 || !defer_ || !defer_(value)) return false;
        continue;
      }
      fields(value, {"type", "serial", "binding", "current"});
      const auto* current = std::get_if<bool>(&value.get("current").value);
      return streamMonotonicMillis() < deadline && integer(value.get("serial")) == serial &&
        sameBinding(binding(value.get("binding")), binding_) && current && *current;
    }
  } catch (...) {}
  return false;
}
}
