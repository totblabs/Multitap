// Root-only explicit memory fixture. Uses the ACTUAL production binary codec.
// No NativeAccess construction, DPAPI, LSA, registry, account, file, service,
// process, network, GUI, login or input operation. UTF conversion and in-memory
// SecureZeroMemory are the only reached Windows helpers.
#include "windows_autologon_native.hpp"
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace host::autologon;
namespace {
int checks = 0, failures = 0;
void check(bool pass, const char* label) {
  ++checks;
  if (!pass) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
template<class Fn> void invalid(Fn fn, const char* label) {
  try { fn(); check(false, label); }
  catch (const Failure& error) { check(std::string(error.what()) == "recovery_invalid", label); }
  catch (...) { check(false, "unexpected codec exception"); }
}
bool same(const Record& a, const Record& b) {
  return a.schema == b.schema && a.operationId == b.operationId && a.action == b.action &&
    a.phase == b.phase && a.account.name == b.account.name && a.account.domain == b.account.domain &&
    a.account.sid == b.account.sid && a.account.usable == b.account.usable &&
    a.baseline == b.baseline && a.expected == b.expected && a.next == b.next && a.intent == b.intent;
}
Record decode(const std::string& bytes) { Decoder decoder{bytes}; return decoder.record(); }
void encode(Bytes& out, const Record& value) { encodeRecord(out, value); }
std::string hex(const std::string& bytes) {
  // The source-controlled synthetic oracle is deliberately small. This is a
  // representation of production output, NOT a second encoder/expected oracle.
  if (bytes.size() > 2048) throw Failure("fixture_oracle_limit");
  constexpr char digits[] = "0123456789abcdef";
  std::string out; out.reserve(bytes.size() * 2);
  for (const auto value : bytes) { const auto byte = uint8_t(value); out += digits[byte >> 4]; out += digits[byte & 15]; }
  return out;
}
Record schema1Owned() {
  Record r; r.schema = 1;
  r.operationId = "11111111-1111-4111-8111-111111111111";
  r.action = "apply"; r.phase = "owned";
  r.account = {L"fixture-\uD55C\uAE00", L"FIXTURE-PC", L"S-1-5-21-1-2-3-1001", true};
  r.baseline.values[0] = textValue(L"0");
  r.baseline.values[1] = textValue(L"before-\uD55C");
  r.baseline.values[2] = Value{true, 4, {3, 0, 0, 0}};
  r.expected = r.baseline;
  r.expected.values[0] = textValue(L"1");
  r.expected.values[1] = textValue(r.account.name);
  r.expected.values[2] = textValue(r.account.domain);
  r.expected.secretExists = true;
  r.expected.secret = Secret{L"synthetic-\uD55C\uAE00-password"};
  r.next = r.expected;
  return r;
}
void oracle(const char* name, const Record& value) {
  Bytes encoded; encode(encoded, value);
  check(same(decode(encoded.data), value), "schema1 production roundtrip");
  std::cout << "{\"kind\":\"schema1-production-codec-oracle\",\"case\":\"" << name <<
    "\",\"bytes\":" << encoded.data.size() << ",\"hex\":\"" << hex(encoded.data) <<
    "\",\"synthetic\":true,\"productionEncoder\":true,\"privateStateRead\":false}\n";
}
// THIS function and its input use only APIs/fields present in BOTH the released
// schema1 header and new schema2 header. Root compiles the SAME source twice;
// compare its three returned byte/hex rows exactly, not a handwritten encoding.
void commonSchema1Oracle() {
  const auto owned = schema1Owned(); oracle("owned-unicode", owned);
  auto disabled = owned; disabled.action = "disable"; disabled.phase = "disabled";
  disabled.expected = disabled.baseline; disabled.next = disabled.expected;
  oracle("disabled-types-presence", disabled);
  auto pending = owned; pending.phase = "changing"; pending.intent = true;
  pending.expected = pending.baseline; pending.next = owned.expected;
  oracle("changing-intent", pending);
}

#ifndef MULTITAP_AUTOLOGON_CODEC_SCHEMA1_ONLY
void schema2Roundtrips() {
  auto pending = schema1Owned(); pending.schema = 2; pending.phase = "changing"; pending.intent = true;
  pending.expected = pending.baseline;
  pending.expected.values[0] = textValue(L"1");
  pending.expected.values[3] = textValue(L"1");
  pending.expected.values[4] = Value{true, 4, {2, 0, 0, 0}};
  pending.expected.values[5] = Value{true, 1, {}}; // Presence/type only; NO plaintext.
  pending.expected.plainPasswordBytes = 84;
  pending.expected.secretExists = true;
  pending.expected.secret = Secret{L"synthetic-previous-LSA"};
  pending.next = pending.expected; pending.next.values[0] = textValue(L"0");
  Bytes encoded; encode(encoded, pending); const auto actual = decode(encoded.data);
  check(same(actual, pending), "schema2 apply intent real codec roundtrip");
  check(actual.expected.plainPasswordBytes == 84 && actual.next.plainPasswordBytes == 84 &&
    actual.baseline.plainPasswordBytes == 0 && actual.expected.values[5].bytes.empty(), "schema2 carries length metadata without plaintext payload");
  auto owned = schema1Owned(); owned.schema = 2;
  Bytes completed; encode(completed, owned);
  check(same(decode(completed.data), owned), "schema2 completed apply real codec roundtrip");
  auto disabled = owned; disabled.action = "disable"; disabled.phase = "disabled"; disabled.account = {};
  disabled.expected = disabled.baseline; disabled.next = disabled.expected;
  Bytes off; encode(off, disabled);
  const auto result = decode(off.data);
  check(same(result, disabled) && result.account.sid.empty(), "schema2 external disable roundtrip without fabricated account SID");
  pending.expected.plainPasswordBytes = UINT32_MAX; pending.next.plainPasswordBytes = UINT32_MAX;
  Bytes maximum; encode(maximum, pending);
  check(decode(maximum.data).expected.plainPasswordBytes == UINT32_MAX, "opaque DWORD length does not allocate a password buffer");
  for (size_t length = 0; length < encoded.data.size(); ++length)
    invalid([&] { (void)decode(encoded.data.substr(0, length)); }, "schema2 rejects every truncated prefix including metadata suffix");
  invalid([&] { (void)decode(encoded.data + '\0'); }, "schema2 rejects trailing byte");
  if (encoded.data.size() < 8) throw Failure("fixture_header_unavailable");
  auto unknown = encoded.data; unknown[0] = char(99); unknown[1] = unknown[2] = unknown[3] = 0;
  invalid([&] { (void)decode(unknown); }, "schema2 codec rejects unknown schema");
  auto longText = encoded.data;
  // Corrupt the actual first text length; no independent layout/codec oracle.
  for (size_t i = 4; i < 8; ++i)
    *reinterpret_cast<unsigned char*>(&longText[i]) = static_cast<unsigned char>(UINT32_MAX & 0xffu);
  invalid([&] { (void)decode(longText); }, "schema2 rejects oversized encoded string length");
  auto malformed = pending; malformed.baseline.values[0] = Value{false, 1, {}};
  Bytes badPresence; encode(badPresence, malformed);
  invalid([&] { (void)decode(badPresence.data); }, "codec rejects absent value with nonzero type");
  malformed = pending; malformed.baseline.secretExists = false; malformed.baseline.secret = Secret{L"stray-synthetic"};
  Bytes badSecret; encode(badSecret, malformed);
  invalid([&] { (void)decode(badSecret.data); }, "codec rejects secret payload marked absent");
  malformed = pending; malformed.expected.values[5].bytes = {42};
  invalid([&] { Bytes out; encode(out, malformed); }, "production encoder refuses plaintext registry payload");
  auto legacy = schema1Owned(); Bytes old; encode(old, legacy);
  for (size_t length = 0; length < old.data.size(); ++length)
    invalid([&] { (void)decode(old.data.substr(0, length)); }, "schema1 codec still rejects every truncated prefix");
  invalid([&] { (void)decode(old.data + '\0'); }, "schema1 codec still rejects trailing byte");
  if (old.data.size() < 4) throw Failure("fixture_intent_unavailable");
  auto badBool = old.data; badBool[badBool.size() - 4] = 2;
  invalid([&] { (void)decode(badBool); }, "schema1 codec still rejects nonboolean intent");
}
#endif
} // namespace
int main() {
  try {
    commonSchema1Oracle();
#ifndef MULTITAP_AUTOLOGON_CODEC_SCHEMA1_ONLY
    schema2Roundtrips();
#endif
  } catch (...) { check(false, "unexpected fixture exception"); }
  std::cout << "{\"kind\":\"windows-autologon-native-codec-memory\",\"checks\":" << checks <<
    ",\"failures\":" << failures << ",\"productionCodec\":true,\"registry\":false,\"lsa\":false,\"dpapi\":false," <<
    "\"account\":false,\"files\":false,\"services\":false,\"network\":false}\n";
  return failures ? 1 : 0;
}
