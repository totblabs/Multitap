// Build/run only this portable target under Root's exclusive QA grant.
// No Windows headers, registry, LSA, logon, account, file, network or input API.
#include "../../apps/windows_host/src/windows_autologon.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
using namespace host::autologon;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
  ++checks; if (!ok) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
template<class Fn> void rejects(Fn fn, const char* code) {
  try { fn(); check(false, code); }
  catch (const Failure& e) { check(std::string(e.what()) == code, code); }
  catch (...) { check(false, "unexpected exception"); }
}
struct Memory final : Access {
  Snapshot current;
  std::optional<Record> record;
  Account account{L"fixture-user", L"FIXTURE-PC", L"S-1-5-21-1-2-3-1001", true};
  bool admin = true, locked = false, login = false, deny = false, replaceSid = false, failAllWrites = false;
  int saves = 0, writes = 0, validations = 0, reads = 0, inspections = 0;
  int failSave = -1, failWriteBefore = -1, failWriteAfter = -1;
  std::vector<std::string> events;
  std::function<void(Memory&)> onRead;
  std::function<Account(Memory&, int)> onInspect;
  bool administrator() override { return admin; }
  bool lock() override { if (locked) return false; locked = true; return true; }
  void unlock() noexcept override { locked = false; }
  Snapshot read() override { ++reads; if (onRead) onRead(*this); return current; }
  std::optional<Record> load() override { return record; }
  void save(const Record& value) override { if (++saves == failSave) throw Failure("storage_unavailable"); record = value; events.push_back("checkpoint"); }
  void remove() override { record.reset(); }
  std::string revision(const Snapshot& snapshot, const std::optional<Record>& value) override {
    // A deterministic fake stamp, not a substitute for the native SHA-256.
    std::wstring bytes = snapshot.secret.text;
    for (const auto& field : snapshot.values) { bytes += wchar_t(field.exists); bytes += wchar_t(field.type); for (const auto b : field.bytes) bytes += wchar_t(b); }
    bytes += wchar_t(snapshot.policyBlocked); bytes += wchar_t(snapshot.secretExists);
    bytes += wchar_t(snapshot.plainPasswordBytes & 0xffffu); bytes += wchar_t(snapshot.plainPasswordBytes >> 16);
    if (value) { for (char c : value->operationId + value->phase) bytes += wchar_t(c); bytes += wchar_t(value->intent); }
    const auto digest = std::hash<std::wstring>{}(bytes); std::ostringstream text; text << std::hex << std::setfill('0') << std::setw(16) << digest;
    auto out = text.str(); while (out.size() < 64) out += out; return out.substr(0, 64);
  }
  Account validateInteractive(const std::wstring&, const Secret&) override {
    ++validations; if (deny) throw Failure("credentials_rejected"); auto out = account;
    if (replaceSid) account.sid += L"-new";
    return out;
  }
  Account inspectAccount(const Account&) override { ++inspections; return onInspect ? onInspect(*this, inspections) : account; }
  Observation observeLogin(const Account&) override { return {login, login ? "2026-10-28T00:00:00Z" : ""}; }
  void beforeWrite() { if (++writes == failWriteBefore || failAllWrites) throw Failure("write_failed"); }
  void afterWrite() { if (writes == failWriteAfter) throw Failure("write_failed"); }
  void writeValue(size_t i, const Value& value) override {
    if (i == 5 && value.exists) throw Failure("invalid_request");
    beforeWrite(); current.values.at(i) = value;
    if (i == 5) current.plainPasswordBytes = 0;
    events.push_back(i == 0 ? (value == textValue(L"1") ? "enable" : "disable") : i == 5 ? "delete-plain" : i == 3 || i == 4 ? "clear-control" : "account"); afterWrite();
  }
  void writeSecret(bool present, const Secret& value) override {
    beforeWrite(); current.secretExists = present; current.secret = value; events.push_back(present ? "secret" : "clear-secret"); afterWrite();
  }
};
Request request(Memory& memory, const std::string& id = "11111111-1111-4111-8111-111111111111") {
  Request r; r.operationId = id; r.expectedRevision = memory.revision(memory.current, memory.record); r.confirmed = true;
  r.username = L"fixture-user"; r.password = Secret{L"synthetic-password"}; r.confirmation = r.password; return r;
}
Request next(Memory& memory) { return request(memory, "22222222-2222-4222-8222-222222222222"); }
void makeExternal(Memory& m) {
  m.current.values[0] = textValue(L"1"); m.current.values[1] = textValue(L"external-user");
  m.current.values[2] = textValue(L"EXTERNAL-PC"); m.current.values[3] = textValue(L"1");
  m.current.values[4] = Value{true,4,{3,0,0,0}};
  m.current.values[5] = Value{true,1,{}}; m.current.plainPasswordBytes = 34;
  m.current.secretExists = true; m.current.secret = Secret{L"synthetic-external-LSA"};
}
void takeoverFixture() {
  { Memory m; makeExternal(m); Controller c(m); const auto before = m.current;
    auto r = request(m); r.takeOwnership = true; r.confirmed = false;
    rejects([&]{ c.apply(r); },"confirmation_required"); check(m.current == before && m.writes == 0 && m.saves == 0 && m.validations == 0,"takeover requires separate explicit confirmation"); }
  { Memory m; makeExternal(m); Controller c(m); auto r=request(m); r.takeOwnership=true; m.current.values[4]=textValue(L"new-count");
    rejects([&]{c.apply(r);},"stale_revision");check(m.writes==0&&m.saves==0&&m.validations==0,"stale external snapshot does not validate or mutate"); }
  { Memory m; makeExternal(m); Controller c(m); m.admin=false; auto r=request(m);r.takeOwnership=true;
    rejects([&]{c.disable(r);},"administrator_required");check(m.writes==0&&m.validations==0,"external disable still requires administrator"); }
  { Memory m; makeExternal(m); const auto before=m.current; m.deny=true; Controller c(m);auto r=request(m);r.takeOwnership=true;
    rejects([&]{c.apply(r);},"credentials_rejected");check(m.current==before&&m.writes==0&&m.saves==0&&m.validations==1,"bad entered account leaves all external settings untouched"); }
  { Memory m; makeExternal(m); const auto before=m.current; Controller c(m);auto r=request(m);r.takeOwnership=true;r.confirmation=Secret{L"other"};
    rejects([&]{c.apply(r);},"invalid_credentials");check(m.current==before&&m.saves==0&&m.validations==0,"password confirmation precedes takeover record and delete"); }
  { Memory m; makeExternal(m);const auto before=m.current; m.replaceSid=true;Controller c(m);auto r=request(m);r.takeOwnership=true;
    rejects([&]{c.apply(r);},"account_changed");check(m.current==before&&m.writes==0&&m.saves==0,"same-name changed SID cannot admit takeover"); }
  { Memory m; makeExternal(m);Controller c(m);auto r=request(m);r.takeOwnership=true;
    const auto status=c.apply(r);check(status.state=="configured"&&status.enabled==true&&!status.loggedIn,"takeover applied is not a Windows login");
    check(m.record&&m.record->schema==2&&baselineSafe(m.record->baseline),"schema2 stores a safe-off baseline, never restores external auto login");
    check(!m.current.values[3].exists&&!m.current.values[4].exists&&!m.current.values[5].exists&&m.current.plainPasswordBytes==0,"takeover clears Force Count and plaintext presence");
    check(m.current.secret==r.password&&m.validations==1,"takeover LSA uses only explicitly entered verified password");
    const auto off=std::find(m.events.begin(),m.events.end(),"disable"), plain=std::find(m.events.begin(),m.events.end(),"delete-plain"), enable=std::find(m.events.begin(),m.events.end(),"enable");
    check(off<plain&&plain<enable,"off readback precedes delete-only plaintext and enable is last");
    const auto writes=m.writes, auth=m.validations;c.apply(r);check(m.writes==writes&&m.validations==auth,"same takeover UUID is read-only replay");
    check(c.disable(next(m)).state=="off"&&baselineSafe(m.current),"later owned disable retains safe-off adoption baseline"); }
  { Memory m; makeExternal(m);m.current.policyBlocked=true;Controller c(m);check(c.status().state=="external"&&c.status().reason=="policy_restricted","external policy keeps explicit disable available");
    auto r=request(m);r.takeOwnership=true;rejects([&]{c.apply(r);},"policy_restricted");check(m.writes==0&&m.validations==0,"takeover never changes or bypasses policy");
    r.password=Secret{};r.confirmation=Secret{};r.username.clear();const auto out=c.disable(r);
    check(out.enabled==false&&out.state=="policy_blocked"&&m.current.policyBlocked&&m.validations==0,"external disable needs no credentials and preserves policy");
    check(m.record&&m.record->schema==2&&m.record->phase=="disabled"&&m.record->account.sid.empty(),"external disabled record does not invent an account SID"); }
  { Memory m;Controller c(m);c.apply(request(m));m.current.values[2]=textValue(L"external-drift");auto r=next(m);r.takeOwnership=true;
    check(c.disable(r).enabled==false&&baselineSafe(m.current),"fresh explicit takeover can disable later foreign drift without guessing old ownership"); }
  { Memory m;makeExternal(m);Controller c(m);auto r=request(m);r.takeOwnership=true;m.failWriteBefore=2;try{c.apply(r);}catch(...){}m.failWriteBefore=-1;
    const auto writes=m.writes;auto later=next(m);later.takeOwnership=true;rejects([&]{c.apply(later);},"repair_required");check(m.writes==writes,"takeover cannot replace an in-flight repair record");
    later.takeOwnership=false;check(c.repair(later).enabled==false&&baselineSafe(m.current),"recorded takeover can repair without reauth or resurrecting plaintext"); }
  { Memory m;makeExternal(m);Controller c(m);auto r=request(m);r.takeOwnership=true;r.password=Secret{};r.confirmation=Secret{};
    check(c.disable(r).enabled==false&&m.validations==0,"external disable does not load or verify a password"); }
  Memory successful;makeExternal(successful);Controller complete(successful);auto initial=request(successful);initial.takeOwnership=true;complete.apply(initial);
  const int writes=successful.writes,saves=successful.saves;
  for(int n=1;n<=writes;++n)for(bool after:{false,true}){
    Memory m;makeExternal(m);Controller c(m);auto r=request(m);r.takeOwnership=true;if(after)m.failWriteAfter=n;else m.failWriteBefore=n;
    try{c.apply(r);}catch(...){}m.failWriteAfter=-1;m.failWriteBefore=-1;
    check(m.record&&m.record->phase=="changing","every takeover write fault retains partial ownership evidence");
    check(c.status().enabled==false,"recoverable write fault attempts exact disable-first before returning");
    auto repair=next(m);check(c.repair(repair).enabled==false&&baselineSafe(m.current)&&!m.current.values[5].exists,"write fault repair never restores foreign plaintext/control/secret");
  }
  for(int n=1;n<=saves;++n){
    Memory m;makeExternal(m);const auto before=m.current;Controller c(m);auto r=request(m);r.takeOwnership=true;m.failSave=n;try{c.apply(r);}catch(...){}m.failSave=-1;
    if(!m.record)check(m.current==before&&m.writes==0,"initial checkpoint fault leaves external state unchanged");
    else{check(m.record->phase=="changing","checkpoint fault never becomes claimed successful takeover");check(c.repair(next(m)).enabled==false&&baselineSafe(m.current),"checkpoint fault repairs only to safe-off baseline");}
  }
  { Memory m;makeExternal(m);Controller c(m);auto r=request(m);r.takeOwnership=true;m.failAllWrites=true;rejects([&]{c.disable(r);},"repair_required");
    check(c.status().enabled==true&&c.status().state=="failed","failed flag write never claims Windows auto login was disabled");
    m.failAllWrites=false;check(c.repair(next(m)).enabled==false&&baselineSafe(m.current),"later explicit repair follows actual native write completion"); }
  { Memory m;makeExternal(m);Controller c(m);auto r=request(m);r.takeOwnership=true;m.onRead=[](Memory& v){if(v.writes==2)v.current.values[2]=textValue(L"foreign-third-image");};
    rejects([&]{c.apply(r);},"configuration_changed");m.onRead={};const auto before=m.current;const auto writesBefore=m.writes;
    rejects([&]{c.repair(next(m));},"repair_required");check(m.current==before&&m.writes==writesBefore,"third image during takeover is preserved, never guessed rollback"); }
  { Memory m;makeExternal(m);Controller c(m);m.current.plainPasswordBytes++;const auto before=c.status().revision;m.current.plainPasswordBytes++;
    check(c.status().revision!=before,"plaintext size-only metadata participates in fresh revision"); }
  { Memory m;makeExternal(m);Controller c(m);auto r=request(m);r.takeOwnership=true;
    rejects([&]{c.repair(r);},"invalid_request");check(m.saves==0&&m.writes==0,"repair cannot be used as an external takeover action"); }
  { Memory m;Controller c(m);m.current.values[0]=Value{true,4,{1,0,0,0}};
    check(!c.status().enabled.has_value(),"unrecognized flag type remains unknown rather than enabled/disabled guess"); }
  { Memory m;makeExternal(m);Controller c(m);m.current.values[5].bytes={42};auto r=request(m);r.takeOwnership=true;
    rejects([&]{c.apply(r);},"state_unavailable");check(m.saves==0&&m.writes==0,"adapter violating presence-only plaintext contract is never persisted"); }
  check(accountLabel(textValue(L"plain-user"))==L"plain-user"&&accountLabel(textValue(L"bad\nname")).empty(),"external label is presentation-only and controls rejected");
}
void fixture() {
  { Memory m; Controller c(m); check(c.status().state == "off" && m.writes == 0 && m.validations == 0, "default off performs no mutation/authentication"); }
  { Memory m; Controller c(m); auto r = request(m); r.confirmed = false; rejects([&] { c.apply(r); }, "confirmation_required"); check(m.writes == 0 && m.validations == 0, "unconfirmed never validates"); }
  { Memory m; Controller c(m); auto r = request(m); r.confirmation = Secret{L"different"}; rejects([&] { c.apply(r); }, "invalid_credentials"); check(m.validations == 0, "mismatch never authenticates"); }
  { Memory m; Controller c(m); auto r = request(m); r.password = Secret{}; r.confirmation = Secret{}; rejects([&] { c.apply(r); }, "invalid_credentials"); }
  { Memory m; m.current.secretExists = true; m.current.secret = Secret{L"foreign"}; Controller c(m); rejects([&] { c.apply(request(m)); }, "foreign_configuration"); check(m.writes == 0 && m.validations == 0, "foreign secret preserved"); }
  { Memory m; m.current.values[5] = textValue(L"foreign-plaintext"); Controller c(m); check(c.status().state == "external", "plaintext presence blocks"); rejects([&] { c.apply(request(m)); }, "foreign_configuration"); }
  for (size_t field : {size_t(3), size_t(4)}) { Memory m; m.current.values[field] = textValue(L"0"); Controller c(m); rejects([&] { c.apply(request(m)); }, "foreign_configuration"); check(m.writes == 0, "force/count settings never adopted"); }
  { Memory m; m.current.policyBlocked = true; Controller c(m); check(c.status().state == "policy_blocked", "policy observed"); rejects([&] { c.apply(request(m)); }, "policy_restricted"); check(m.writes == 0, "policy never changed"); }
  { Memory m; m.deny = true; Controller c(m); rejects([&] { c.apply(request(m)); }, "credentials_rejected"); check(m.validations == 1 && m.writes == 0 && !m.record, "one failed INTERACTIVE validation only"); }
  { Memory m; m.account.usable = false; Controller c(m); rejects([&] { c.apply(request(m)); }, "account_restricted"); check(m.writes == 0, "restricted account no writes"); }
  { Memory m; m.replaceSid = true; Controller c(m); rejects([&] { c.apply(request(m)); }, "account_changed"); check(m.writes == 0, "same-name replacement SID rejected"); }
  { Memory m; Controller c(m);
    m.onInspect = [](Memory& value, int n) {
      auto observed = value.account;
      if (n == 2) observed.sid += L"-replacement";
      if (n == 3) observed.usable = false;
      return observed;
    };
    // Initial pre-write validation is observation 1. At the final enable gate,
    // the old expression combined usable=true from replaced SID observation 2
    // and matching SID from unusable observation 3, admitting neither account.
    rejects([&] { c.apply(request(m)); }, "repair_required");
    check(!(m.current.values[0] == textValue(L"1")), "mixed account observations never enable autologon");
    check(m.record && m.record->phase == "changing", "replaced account preserves exact repair checkpoint");
    check(m.validations == 1, "final account drift does not repeat interactive authentication");
    m.onInspect = {};
    if (m.record && m.record->phase == "changing") {
      check(c.repair(next(m)).state == "off" && !m.current.secretExists, "replaced account cleanup uses ownership without reauthentication");
    }
  }
  { Memory m; Controller c(m); auto r = request(m); auto out = c.apply(r);
    check(out.state == "configured" && !out.loggedIn, "stored is not logged in");
    check(m.events[m.events.size() - 2] == "checkpoint" && m.current.values[0] == textValue(L"1"), "enable checkpoint persisted");
    auto secret = std::find(m.events.begin(), m.events.end(), "secret"), enable = std::find(m.events.begin(), m.events.end(), "enable");
    check(secret < enable, "LSA secret/readback precedes enable");
    const int writes = m.writes; c.apply(r); check(m.validations == 1 && m.writes == writes, "same operation replay is read-only");
    m.login = true; check(c.status().state == "observed", "observed Windows login separate");
  }
  { Memory m; m.current.values[1] = Value{true, 4, {1, 0, 0, 0}}; m.current.values[2] = textValue(L"original-domain"); const auto baseline = m.current; Controller c(m); c.apply(request(m));
    const auto start = m.events.size(); auto out = c.disable(next(m));
    check(out.state == "off" && m.current == baseline && !m.current.secretExists, "disable restores exact safe baseline types/presence");
    const auto first = m.events.begin() + std::ptrdiff_t(start);
    check(std::find(first, m.events.end(), "clear-secret") < std::find(first, m.events.end(), "account"), "disable clears secret before account values");
  }
  { Memory m; Controller c(m); c.apply(request(m)); m.current.values[2] = textValue(L"external"); const auto before = m.current; int writes = m.writes;
    rejects([&] { c.disable(next(m)); }, "configuration_changed"); check(m.current == before && m.writes == writes, "foreign owned drift not overwritten"); }
  { Memory m; Controller c(m); c.apply(request(m)); m.account.sid += L"-replacement"; check(c.status().state == "needs_update", "stored SID lifetime checked"); }
  for (int n = 1; n <= 5; ++n) {
    Memory m; Controller c(m); m.failWriteBefore = n;
    try { c.apply(request(m)); } catch (const Failure&) {}
    if (m.record && m.record->phase == "changing") {
      check(c.status().state == "failed", "write failure not reported off");
      m.failWriteBefore = -1; auto out = c.repair(next(m));
      check(out.state == "off" && baselineSafe(m.current) && !m.current.secretExists, "pre-write fault explicit repair");
    }
  }
  for (int n = 1; n <= 5; ++n) {
    Memory m; Controller c(m); m.failWriteAfter = n;
    try { c.apply(request(m)); } catch (const Failure&) {}
    if (m.record && m.record->phase == "changing") {
      check(m.record->intent, "post-write interrupted intent retained"); m.failWriteAfter = -1;
      check(c.repair(next(m)).state == "off" && !m.current.secretExists, "post-write exact next image reconciled");
    }
  }
  for (int n = 1; n <= 12; ++n) {
    Memory m; Controller c(m); m.failSave = n;
    try { c.apply(request(m)); } catch (...) {}
    m.failSave = -1;
    if (m.record && m.record->phase == "changing") check(c.repair(next(m)).state == "off", "checkpoint interruption repair");
    else check(!m.record ? m.writes == 0 : m.record->phase == "owned", "no mutation before first durable intent");
  }
  { Memory m; Controller c(m); m.failWriteAfter = 3; try { c.apply(request(m)); } catch (...) {}
    m.failWriteAfter = -1; m.current.values[2] = textValue(L"foreign-during-partial"); const auto before = m.current; auto writes = m.writes;
    rejects([&] { c.repair(next(m)); }, "repair_required"); check(m.current == before && m.writes == writes, "partial foreign image preserved, no guessed rollback"); }
  { Memory m; Controller c(m); c.apply(request(m)); auto r = next(m); r.password = Secret{L"replacement-password"}; r.confirmation = r.password; c.apply(r);
    check(m.record->expected.secret == r.password && m.record->next.secret == r.password && !m.record->baseline.secretExists, "successful password change retains no obsolete secret"); }
  { Memory m; m.admin = false; Controller c(m); check(c.status().reason == "administrator_required", "non-admin status gated"); rejects([&] { c.apply(request(m)); }, "administrator_required"); }
  { Memory m; m.locked = true; Controller c(m); check(c.status().reason == "operation_busy" && m.writes == 0, "exclusive lock bound, no wait"); }
}
}
int main() {
  try { fixture(); takeoverFixture(); } catch (const std::exception&) { check(false, "fixture threw"); }
  std::cout << "{\"kind\":\"windows-autologon-memory\",\"checks\":" << checks << ",\"failures\":" << failures << ",\"nativeApi\":false,\"network\":false,\"credentialsRead\":false}\n";
  return failures ? 1 : 0;
}
