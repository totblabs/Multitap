// host-machine-v1 (credential_binding.hpp): a registration is used only on the
// Windows it was approved on. Memory only: no credential file, settings file,
// service, broker or SMBIOS read of this PC is involved.
#include "credential_binding.hpp"
#include <iostream>

using namespace host;
namespace {
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
constexpr const char* original = "f1769b29-ced0-40b6-981b-c0ae743ce185";
constexpr const char* copy = "0b3a6f53-1d2e-4c55-9a77-3e5b1d0c2f90";
Credentials registration(const std::string& machine) { return Credentials{L"https://broker.invalid", "a033c3ac-690a-4e1b-8557-7cd288b24ae0", std::string(43, 'T'), "pve-149", L"", machine}; }

struct Fake {
  std::optional<std::string> live; bool liveThrows = false, saveThrows = false, holdThrows = false;
  int saves = 0, holds = 0; Credentials saved, held;
  CredentialMachineAccess access() {
    CredentialMachineAccess value;
    value.live = [this]() -> std::optional<std::string> { if (liveThrows) throw std::runtime_error("SMBIOS UUID is unavailable"); return live; };
    value.save = [this](const Credentials& c) { saves++; if (saveThrows) throw std::runtime_error("storage busy"); saved = c; };
    value.holdAside = [this](const Credentials& c) { holds++; if (holdThrows) throw std::runtime_error("move failed"); held = c; };
    return value;
  }
};

// Approved here: used, named on the channel, nothing rewritten.
void sameMachine() {
  Fake f; f.live = original; auto c = registration(original);
  require(bindCredentialMachine(c, f.access()) == CredentialMachine::bound, "Own registration was not bound");
  require(c.machineUuid == original && !f.saves && !f.holds, "Own registration was rewritten or kept aside");
  // SMBIOS text case does not matter.
  Fake upper; upper.live = "F1769B29-CED0-40B6-981B-C0AE743CE185"; auto u = registration(original);
  require(bindCredentialMachine(u, upper.access()) == CredentialMachine::bound && u.machineUuid == original, "Upper-case SMBIOS UUID was treated as another machine");
}
// Saved by an older Host (no machine): an update keeps the registration and records this machine.
void updateRecordsMachine() {
  Fake f; f.live = original; auto c = registration("");
  require(bindCredentialMachine(c, f.access()) == CredentialMachine::recorded, "Released registration was not kept");
  require(f.saves == 1 && f.saved.machineUuid == original && f.saved.token == c.token && f.saved.hostId == c.hostId, "The recorded machine was not saved with the same registration");
  require(c.machineUuid == original && !f.holds, "Released registration was not used with this machine");
  // A failed save still keeps the registration (the next start records it).
  Fake busy; busy.live = original; busy.saveThrows = true; auto b = registration("");
  require(bindCredentialMachine(b, busy.access()) == CredentialMachine::recorded && b.machineUuid == original && !busy.holds, "A failed save cost the registration");
}
// A full clone: another SMBIOS UUID. Not used, kept aside, never re-saved.
void copiedMachine() {
  Fake f; f.live = copy; auto c = registration(original);
  require(bindCredentialMachine(c, f.access()) == CredentialMachine::copied, "A copied registration was used");
  require(f.holds == 1 && f.held.hostId == c.hostId && f.held.machineUuid == original && !f.saves, "The copied registration was not kept aside as it was");
  // When keeping it aside fails, the caller gets the error and uses nothing.
  Fake stuck; stuck.live = copy; stuck.holdThrows = true; auto s = registration(original); bool thrown = false;
  try { (void)bindCredentialMachine(s, stuck.access()); } catch (const std::runtime_error&) { thrown = true; }
  require(thrown && !stuck.saves, "A failed hold-aside let the copy continue");
}
// A Windows without a readable SMBIOS UUID: today's behaviour, no UUID named.
void unreadableMachine() {
  for (int mode = 0; mode < 3; mode++) {
    Fake f; if (mode == 0) f.live.reset(); else if (mode == 1) f.liveThrows = true; else f.live = "00000000-0000-0000-0000";
    auto c = registration(original);
    require(bindCredentialMachine(c, f.access()) == CredentialMachine::unverifiable, "A missing SMBIOS UUID blocked the registration");
    require(c.machineUuid.empty() && !f.saves && !f.holds, "A missing SMBIOS UUID named a machine or changed storage");
  }
}
// The sealed text: released files (no machine) still read; the machine round-trips.
void credentialText() {
  Credentials legacy; readCredentialJson(JsonParser::parse("{\"origin\":\"https://broker.invalid\",\"hostId\":\"a033c3ac-690a-4e1b-8557-7cd288b24ae0\",\"token\":\"" + std::string(43, 'T') + "\",\"desktopId\":\"pve-149\"}"), legacy);
  require(legacy.machineUuid.empty() && legacy.desktopId == "pve-149", "A released registration no longer reads");
  auto bound = registration("F1769B29-CED0-40B6-981B-C0AE743CE185"); const auto text = credentialJson(bound);
  require(text.find("\"machineUuid\":\"f1769b29-ced0-40b6-981b-c0ae743ce185\"") != std::string::npos, "The machine is not sealed with the registration");
  Credentials back; readCredentialJson(JsonParser::parse(text), back);
  require(back.machineUuid == original && back.token == bound.token && back.hostId == bound.hostId, "The registration did not round-trip");
  require(credentialJson(registration("")).find("machineUuid") == std::string::npos, "An unknown machine was written");
  bool refused = false; try { (void)credentialJson(registration("not-a-uuid")); } catch (const std::runtime_error&) { refused = true; }
  require(refused, "A malformed machine was sealed");
  refused = false; try { Credentials bad; readCredentialJson(JsonParser::parse("{\"origin\":\"https://broker.invalid\",\"token\":\"" + std::string(43, 'T') + "\",\"machineUuid\":\"zz\"}"), bad); } catch (const std::runtime_error&) { refused = true; }
  require(refused, "A malformed sealed machine was accepted");
  require(credentialMachineUuid(original) && !credentialMachineUuid("F1769B29-CED0-40B6-981B-C0AE743CE185") && !credentialMachineUuid(""), "UUID shape check");
}
// A registration this Windows cannot unseal is its own error, not "none".
void unreadableIsDistinct() {
  bool caught = false; try { throw CredentialUnreadable(); } catch (const std::runtime_error& error) { caught = std::string(error.what()).find("Read protected host credential") == 0; }
  require(caught, "An unreadable registration lost its released message");
  const auto held = heldPrivatePath(credentialPath(), L"copied").filename().wstring();
  const auto request = heldPrivatePath(L"C:\\fixture\\connection-request.dpapi", L"copied");
  require(request.parent_path() == L"C:\\fixture" && request.filename().wstring().rfind(L"connection-request-copied-", 0) == 0, "Kept-aside request left its folder");
  require(held.rfind(L"credentials-copied-", 0) == 0 && held.size() > 25 && held.substr(held.size() - 6) == L".dpapi" && held != L"credentials.dpapi", "Kept-aside name could replace the live registration");
}
}
int main() {
  try {
    sameMachine(); updateRecordsMachine(); copiedMachine(); unreadableMachine(); credentialText(); unreadableIsDistinct();
    std::cout << "{\"kind\":\"host-credential-machine-binding\",\"cases\":6,\"credentialFileWritten\":false,\"settingsWritten\":false,\"installedServiceChanged\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
