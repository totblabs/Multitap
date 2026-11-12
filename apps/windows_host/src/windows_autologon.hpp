#pragma once
// Pure transaction boundary. No Windows headers, logon, file or registry calls.
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace host::autologon {
inline void erase(std::wstring& value) noexcept {
  volatile wchar_t* at = value.empty() ? nullptr : &value[0];
  for (size_t i = 0; i < value.size(); ++i) at[i] = 0;
  value.clear();
}
struct Secret {
  std::wstring text;
  Secret() = default;
  explicit Secret(std::wstring value) : text(std::move(value)) {}
  Secret(const Secret&) = default;
  Secret& operator=(const Secret& other) { if (this != &other) { erase(text); text = other.text; } return *this; }
  ~Secret() { erase(text); }
  bool operator==(const Secret& other) const { return text == other.text; }
};
struct Failure : std::runtime_error { explicit Failure(const char* code) : std::runtime_error(code) {} };
struct Value {
  bool exists = false;
  uint32_t type = 0;
  std::vector<uint8_t> bytes;
  bool operator==(const Value& other) const { return exists == other.exists && type == other.type && bytes == other.bytes; }
};
// Only these fixed Winlogon values may be touched; no generic registry RPC.
inline constexpr std::array<const wchar_t*, 6> names{
  L"AutoAdminLogon", L"DefaultUserName", L"DefaultDomainName",
  L"ForceAutoLogon", L"AutoLogonCount", L"DefaultPassword"};
struct Snapshot {
  std::array<Value, 6> values;
  bool secretExists = false;
  Secret secret;
  bool policyBlocked = false;
  // Metadata only. DefaultPassword contents are never read into values[5].
  uint32_t plainPasswordBytes = 0;
  bool operator==(const Snapshot& other) const {
    return values == other.values && plainPasswordBytes == other.plainPasswordBytes && secretExists == other.secretExists &&
      secret == other.secret && policyBlocked == other.policyBlocked;
  }
};
inline bool sameSettings(const Snapshot& a, const Snapshot& b) {
  return a.values == b.values && a.plainPasswordBytes == b.plainPasswordBytes && a.secretExists == b.secretExists && a.secret == b.secret;
}
inline Value textValue(const std::wstring& text) {
  Value out{true, 1, {}}; // REG_SZ, UTF-16LE (also portable on non-Windows QA).
  for (const auto ch : text) { out.bytes.push_back(uint8_t(ch)); out.bytes.push_back(uint8_t(uint32_t(ch) >> 8)); }
  out.bytes.push_back(0); out.bytes.push_back(0); return out;
}
inline bool disabled(const Value& value) {
  return !value.exists || value == textValue(L"0");
}
inline bool baselineSafe(const Snapshot& snapshot) {
  return disabled(snapshot.values[0]) && !snapshot.secretExists &&
    !snapshot.values[3].exists && !snapshot.values[4].exists && !snapshot.values[5].exists && snapshot.plainPasswordBytes == 0;
}
inline Snapshot takeoverBaseline(Snapshot value) {
  value.values[0] = textValue(L"0");
  value.values[3] = {}; value.values[4] = {}; value.values[5] = {};
  value.plainPasswordBytes = 0; value.secretExists = false; value.secret = Secret{};
  return value;
}
inline std::wstring accountLabel(const Value& value) {
  if (!value.exists || value.type != 1 || value.bytes.size() < 2 || value.bytes.size() > 514 || value.bytes.size() % 2) return {};
  std::wstring out;
  for (size_t i = 0; i < value.bytes.size(); i += 2) {
    const uint16_t ch = uint16_t(value.bytes[i]) | uint16_t(uint16_t(value.bytes[i+1]) << 8);
    if (i + 2 == value.bytes.size()) return ch == 0 ? out : std::wstring();
    if (ch < 32 || ch == 127 || (ch >= 0xdc00 && ch <= 0xdfff)) return {};
    out += wchar_t(ch);
    if (ch >= 0xd800 && ch <= 0xdbff) {
      if (i + 4 >= value.bytes.size()) return {};
      const uint16_t low = uint16_t(value.bytes[i+2]) | uint16_t(uint16_t(value.bytes[i+3]) << 8);
      if (low < 0xdc00 || low > 0xdfff) return {};
      out += wchar_t(low); i += 2;
    }
  }
  return {};
}
struct Account {
  std::wstring name, domain, sid;
  bool usable = false;
};
struct Observation { bool loggedIn = false; std::string observedAt; };
struct Record {
  uint32_t schema = 1;
  std::string operationId, action, phase;
  Account account;
  Snapshot baseline, expected, next;
  bool intent = false;
};
struct Status {
  bool supported = true, administrator = false;
  std::string state = "unknown", reason = "state_unavailable", revision;
  std::wstring account;
  bool loggedIn = false;
  std::string observedAt, operationId, operationState;
  bool supportsTakeOwnership = true;
  std::optional<bool> enabled;
};
struct Request {
  std::string operationId, expectedRevision;
  bool confirmed = false;
  std::wstring username;
  Secret password, confirmation;
  bool takeOwnership = false;
};
inline bool hex64(const std::string& value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
inline bool operationUuid(const std::string& value) {
  if (value.size() != 36) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
    else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
  }
  return true;
}
struct Access {
  virtual ~Access() = default;
  virtual bool administrator() = 0;
  virtual bool lock() = 0;
  virtual void unlock() noexcept = 0;
  virtual Snapshot read() = 0;
  virtual std::optional<Record> load() = 0;
  // save must durably replace one DPAPI/SY+BA-only record before returning.
  virtual void save(const Record&) = 0;
  virtual void remove() = 0;
  virtual std::string revision(const Snapshot&, const std::optional<Record>&) = 0;
  virtual Account validateInteractive(const std::wstring&, const Secret&) = 0;
  virtual Account inspectAccount(const Account&) = 0;
  virtual Observation observeLogin(const Account&) = 0;
  virtual void writeValue(size_t, const Value&) = 0;
  virtual void writeSecret(bool, const Secret&) = 0;
};
struct Guard {
  Access& access;
  explicit Guard(Access& value) : access(value) { if (!access.lock()) throw Failure("operation_busy"); }
  ~Guard() { access.unlock(); }
};
class Controller {
 public:
  explicit Controller(Access& access) : access_(access) {}
  Status status(const std::string& operation = {}) {
    Status out; out.administrator = access_.administrator();
    if (!out.administrator) { out.reason = "administrator_required"; return out; }
    try { Guard guard(access_); return statusLocked(operation); }
    catch (const Failure& e) { out.reason = e.what(); }
    catch (...) { out.reason = "storage_unavailable"; }
    return out;
  }
  Status apply(const Request& request) { return change(request, "apply"); }
  Status disable(const Request& request) { return change(request, "disable"); }
  Status repair(const Request& request) { return change(request, "repair"); }
 private:
  Access& access_;
  static void checkRecord(const Record& r) {
    if ((r.schema != 1 && r.schema != 2) || !operationUuid(r.operationId) ||
        (r.action != "apply" && r.action != "disable") ||
        (r.phase != "changing" && r.phase != "owned" && r.phase != "disabled")) throw Failure("recovery_invalid");
    if ((r.account.sid.empty() && (r.schema == 1 || r.action == "apply" || r.phase == "owned")) || !baselineSafe(r.baseline)) throw Failure("recovery_invalid");
    if (r.schema == 1) {
      if (r.expected.values[5].exists || r.next.values[5].exists) throw Failure("recovery_invalid");
      for (size_t i : {size_t(3), size_t(4)}) {
        if (!(r.expected.values[i] == r.baseline.values[i]) || !(r.next.values[i] == r.baseline.values[i])) throw Failure("recovery_invalid");
      }
    }
    for (const auto* image : {&r.baseline, &r.expected, &r.next}) {
      if ((!image->secretExists && !image->secret.text.empty()) || image->secret.text.size() > (r.schema == 2 ? 2048u : 256u)) throw Failure("recovery_invalid");
      if (!image->values[5].bytes.empty() || (!image->values[5].exists && image->plainPasswordBytes != 0)) throw Failure("recovery_invalid");
      for (const auto& value : image->values) if (value.bytes.size() > 4096) throw Failure("recovery_invalid");
    }
    if (r.phase == "owned" && (r.action != "apply" || r.intent || !r.expected.secretExists || r.expected.secret.text.empty() || r.expected.secret.text.size() > 256 || !(r.expected.values[0] == textValue(L"1")) || r.expected.values[3].exists || r.expected.values[4].exists || r.expected.values[5].exists)) throw Failure("recovery_invalid");
    if (r.phase == "disabled" && (r.intent || !baselineSafe(r.expected))) throw Failure("recovery_invalid");
  }
  Status statusLocked(const std::string& operation) {
    Status out; out.administrator = true;
    const auto current = access_.read(); const auto record = access_.load();
    if (disabled(current.values[0])) out.enabled = false;
    else if (current.values[0] == textValue(L"1")) out.enabled = true;
    if (record) checkRecord(*record);
    out.revision = access_.revision(current, record);
    if (!hex64(out.revision)) throw Failure("storage_unavailable");
    if (record && (operation.empty() || record->operationId == operation)) {
      out.operationId = record->operationId;
      out.operationState = record->phase == "owned" ? "configured" : record->phase == "disabled" ? "disabled" : "partial";
    } else if (!operation.empty()) { out.operationId = operation; out.operationState = "unknown"; }
    if (record && (record->phase == "changing" || record->intent)) {
      out.state = "failed"; out.reason = "repair_required"; return out;
    }
    if (record && record->phase == "owned") {
      out.account = record->account.name;
      if (!sameSettings(current, record->expected)) { out.state = "external"; out.reason = current.policyBlocked ? "policy_restricted" : "configuration_changed"; out.account = accountLabel(current.values[1]); return out; }
      if (current.policyBlocked) { out.state = "policy_blocked"; out.reason = "policy_restricted"; return out; }
      const auto actual = access_.inspectAccount(record->account);
      if (!actual.usable || actual.sid != record->account.sid) { out.state = "needs_update"; out.reason = "account_changed"; return out; }
      out.state = "configured"; out.reason = "none";
      const auto login = access_.observeLogin(record->account);
      out.loggedIn = login.loggedIn; out.observedAt = login.observedAt;
      if (login.loggedIn) out.state = "observed";
      return out;
    }
    if (!baselineSafe(current)) { out.state = "external"; out.reason = current.policyBlocked ? "policy_restricted" : "foreign_configuration"; out.account = accountLabel(current.values[1]); return out; }
    out.state = current.policyBlocked ? "policy_blocked" : "off";
    out.reason = current.policyBlocked ? "policy_restricted" : "none";
    return out;
  }
  void step(Record& record, Snapshot target) {
    // Reconcile a completed native write whose durable checkpoint was interrupted.
    if (record.intent) {
      const auto actual = access_.read();
      if (actual == record.next) record.expected = record.next;
      else if (!(actual == record.expected)) throw Failure("configuration_changed");
      record.intent = false; access_.save(record);
    }
    for (size_t i = 0; i < names.size(); ++i) {
      if (record.expected.values[i] == target.values[i]) continue;
      auto next = record.expected; next.values[i] = target.values[i];
      if (i == 5) next.plainPasswordBytes = target.plainPasswordBytes;
      mutate(record, std::move(next), [&] { access_.writeValue(i, target.values[i]); });
    }
    if (record.expected.secretExists != target.secretExists || !(record.expected.secret == target.secret)) {
      auto next = record.expected; next.secretExists = target.secretExists; next.secret = target.secret;
      mutate(record, std::move(next), [&] { access_.writeSecret(target.secretExists, target.secret); });
    }
  }
  void disableAfterTakeoverFailure(Record& record) noexcept {
    if (record.schema != 2) return;
    try {
      const auto actual = access_.read();
      if (!sameSettings(actual, record.expected) && !(record.intent && sameSettings(actual, record.next))) return;
      // Resolve only our exact intent; never overwrite a third/foreign image.
      record.phase = "changing";
      record.expected = actual; record.next = actual; record.intent = false;
      access_.save(record);
      auto off = record.expected; off.values[0] = textValue(L"0"); step(record, off);
    } catch (...) { /* Keep repair evidence; status reports actual flag or unknown. */ }
  }
  void mutate(Record& record, Snapshot next, const std::function<void()>& nativeWrite) {
    if (!(access_.read() == record.expected)) throw Failure("configuration_changed");
    record.next = std::move(next); record.intent = true; access_.save(record);
    // OS registry and LSA are not one atomic transaction. Recheck after the
    // durable intent and after each native write; never "roll back" foreign drift.
    if (!(access_.read() == record.expected)) throw Failure("configuration_changed");
    nativeWrite();
    if (!(access_.read() == record.next)) throw Failure("configuration_changed");
    record.expected = record.next; record.intent = false; access_.save(record);
  }
  Status change(const Request& request, const std::string& action) {
    if (!access_.administrator()) throw Failure("administrator_required");
    if (!request.confirmed) throw Failure("confirmation_required");
    if (action == "repair" && request.takeOwnership) throw Failure("invalid_request");
    if (!operationUuid(request.operationId) || !hex64(request.expectedRevision)) throw Failure("invalid_request");
    Guard guard(access_);
    auto current = access_.read(); auto prior = access_.load();
    if (prior) checkRecord(*prior);
    // An own replay is read-only, never a second credential attempt or mutation.
    if (prior && prior->operationId == request.operationId) return statusLocked(request.operationId);
    if (access_.revision(current, prior) != request.expectedRevision) throw Failure("stale_revision");
    if (action == "repair") {
      if (!prior || prior->phase != "changing") throw Failure("repair_unavailable");
      auto record = *prior;
      try {
        // Only the exact before/after images of our single recorded intent qualify.
        if (record.intent) { if (sameSettings(current, record.next)) record.expected = current; else if (sameSettings(current, record.expected)) record.expected = current; else throw Failure("configuration_changed"); record.intent = false; access_.save(record); }
        else if (!sameSettings(current, record.expected)) throw Failure("configuration_changed");
        record.expected.policyBlocked = current.policyBlocked;
        record.operationId = request.operationId; record.action = "disable"; access_.save(record);
        auto safe = record.expected; safe.values[0] = textValue(L"0"); step(record, safe);
        safe = record.expected; safe.secretExists = false; safe.secret = Secret{}; step(record, safe);
        auto clean = record.baseline; clean.policyBlocked = record.expected.policyBlocked;
        // Restore only our original safe baseline, never an external autologon.
        step(record, clean); record.phase = "disabled"; record.next = record.expected; access_.save(record);
        return statusLocked(request.operationId);
      } catch (...) { throw Failure("repair_required"); }
    }
    if (prior && (prior->phase == "changing" || prior->intent)) throw Failure("repair_required");
    const bool owned = prior && prior->phase == "owned" && sameSettings(current, prior->expected);
    const bool external = prior && prior->phase == "owned" ? !owned : !baselineSafe(current);
    if (external && !request.takeOwnership) throw Failure(prior && prior->phase == "owned" ? "configuration_changed" : "foreign_configuration");
    if (action == "disable" && !owned && !(external && request.takeOwnership)) throw Failure("not_owned");
    if (external && !current.values[5].bytes.empty()) throw Failure("state_unavailable");
    Account account;
    if (action == "apply") {
      if (current.policyBlocked) throw Failure("policy_restricted");
      if (request.username.empty() || request.username.size() > 256 || request.password.text.empty() || request.password.text.size() > 256 || request.password.text.find(L'\0') != std::wstring::npos || request.password.text != request.confirmation.text) throw Failure("invalid_credentials");
      account = access_.validateInteractive(request.username, request.password);
      if (!account.usable || account.sid.empty()) throw Failure("account_restricted");
      // Validation is exactly one explicit INTERACTIVE attempt. Never timer retry.
      const auto now = access_.inspectAccount(account);
      if (!now.usable || now.sid != account.sid) throw Failure("account_changed");
      if (!(access_.read() == current)) throw Failure("configuration_changed");
    }
    Record record; record.operationId = request.operationId; record.action = action;
    record.schema = external ? 2u : (owned ? prior->schema : 1u);
    record.phase = "changing"; record.account = action == "apply" ? account : (owned ? prior->account : Account{});
    record.baseline = external ? takeoverBaseline(current) : (owned ? prior->baseline : current);
    record.expected = current; record.next = current; access_.save(record);
    try {
      auto target = current; target.values[0] = textValue(L"0"); step(record, target);
      if (external) {
        // Explicitly adopted fixed settings are removed only AFTER flag0 readback.
        // Plain password presence/type/size are recorded, never its contents.
        target = takeoverBaseline(record.expected); step(record, target);
      }
      if (action == "apply") {
        target = record.expected; target.values[1] = textValue(account.name); target.values[2] = textValue(account.domain);
        target.secretExists = true; target.secret = request.password; step(record, target);
        // Admission is one coherent account observation. Never combine the
        // usable bit of one account lifetime with the SID of another query.
        const auto beforeEnable = access_.inspectAccount(account);
        if (!beforeEnable.usable || beforeEnable.sid != account.sid) throw Failure("account_changed");
        target = record.expected; target.values[0] = textValue(L"1"); step(record, target); // ENABLE LAST.
        record.phase = "owned";
        // No obsolete password is kept after a successful account/password change.
        record.next = record.expected;
      } else {
        target = record.expected; target.secretExists = false; target.secret = Secret{}; step(record, target);
        target = record.baseline; target.policyBlocked = record.expected.policyBlocked; step(record, target);
        record.phase = "disabled"; record.next = record.expected;
      }
      access_.save(record);
      return statusLocked(request.operationId);
    } catch (const Failure& e) {
      disableAfterTakeoverFailure(record);
      // Keep the exact pending checkpoint for explicit repair; never call it OFF.
      if (std::string(e.what()) == "configuration_changed") throw;
      throw Failure("repair_required");
    } catch (...) { disableAfterTakeoverFailure(record); throw Failure("repair_required"); }
  }
};
} // namespace host::autologon
