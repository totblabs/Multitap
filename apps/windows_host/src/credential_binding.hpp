#pragma once
#include "pending_connection.hpp"
#include "json_object.hpp"
#include <functional>
#include <optional>

// host-machine-v1: a registration belongs to the Windows it was approved on.
//
// A full clone of a registered VM copies the protected registration with the
// disk. Before this check the copy dialled the server with the original's
// token: whichever copy connected first held the channel and the other got
// 409 forever, and the copy never appeared as a new request. The registration
// now carries the SMBIOS UUID it was approved on (Credentials::machineUuid).
// On start the service compares it with the UUID this Windows reports:
//   same                -> use it; the channel names the UUID (agent.hpp) and
//                          the server refuses a mismatch (relay.mjs).
//   none saved (older)   -> record this Windows's UUID and use it (an update
//                          keeps the registration; the server still refuses a
//                          copy made before the update).
//   UUID unreadable     -> use it as before, without naming a UUID (the server
//                          then treats it like a released Host).
//   different           -> do not use it. The file is kept aside (renamed,
//                          never deleted), the public settings forget the
//                          desktop and remember that it was a copy, and Host
//                          settings offers the normal 연결 요청.
namespace host {
enum class CredentialMachine { bound, recorded, unverifiable, copied };
struct CredentialMachineAccess {
  std::function<std::optional<std::string>()> live;  // nullopt or throw: no readable SMBIOS UUID
  std::function<void(const Credentials&)> save;      // re-seal with the recorded UUID
  std::function<void(const Credentials&)> holdAside; // keep the copied registration out of use
};
// `value` is what the channel may use afterwards: its machineUuid is the live
// UUID to name, or empty when none can be named.
inline CredentialMachine bindCredentialMachine(Credentials& value, const CredentialMachineAccess& access) {
  std::optional<std::string> live;
  try { live = access.live(); } catch (...) { live.reset(); }
  if (live) *live = lowerMachineUuid(*live);
  if (!live || !credentialMachineUuid(*live)) { value.machineUuid.clear(); return CredentialMachine::unverifiable; }
  if (value.machineUuid.empty()) {
    value.machineUuid = *live;
    // Not saved now (storage busy, permissions): the next start records it.
    try { access.save(value); } catch (...) {}
    return CredentialMachine::recorded;
  }
  if (lowerMachineUuid(value.machineUuid) == *live) { value.machineUuid = *live; return CredentialMachine::bound; }
  access.holdAside(value);  // a failure throws: the caller uses nothing either way
  return CredentialMachine::copied;
}

// Host settings shows a registration as long as settings.json names a desktop.
// The copy forgets it and keeps a short note (copiedRegistration = the desktop
// it came from) until the next approval rewrites settings.json.
inline void forgetCopiedDesktop(const std::string& desktopId) {
  auto settings = readPublicConfig(); if (std::holds_alternative<std::nullptr_t>(settings.value)) settings.value = Json::Object{};
  auto* fields = std::get_if<Json::Object>(&settings.value); if (!fields) throw std::runtime_error("invalid_host_settings");
  fields->erase("desktopId"); (*fields)["copiedRegistration"].value = desktopId.empty() ? std::string("unknown") : desktopId;
  writePublicConfig(jsonObject(settings));
}
// The registration on disk is still `expected` (the same approval): keep it
// aside. Under the connection state lock, so an approval committed in between
// (pending_connection.hpp commit) is never moved or overwritten.
inline bool sameStoredCredentials(const Credentials& expected) {
  Credentials current; struct Scrub { Credentials& value; ~Scrub() { SecureZeroMemory(value.token.data(), value.token.size()); } } scrub{current};
  return loadCredentials(current) && current.hostId == expected.hostId && current.token == expected.token;
}
inline bool holdCopiedCredentials(const Credentials& expected) {
  auto access = nativeConnectionAccess(); auto lock = connectionStateLock(access);
  if (!sameStoredCredentials(expected)) return false;
  keepCredentialFileAside(L"copied");
  // The original's finished connection request came with the disk. Host
  // settings would read it as this Windows's approval; keep it aside too.
  try { if (const auto request = access.load(); request && lowerMachineUuid(request->uuid) != lowerMachineUuid(machineUuid())) keepPrivateFileAside(connectionRequestPath(), L"copied"); } catch (...) {}
  try { forgetCopiedDesktop(expected.desktopId); } catch (...) {}  // the registration is out of use either way
  return true;
}
inline void recordCredentialMachine(const Credentials& value) {
  auto access = nativeConnectionAccess(); auto lock = connectionStateLock(access);
  if (sameStoredCredentials(value)) saveCredentials(value);
}
inline CredentialMachineAccess nativeCredentialMachineAccess() {
  CredentialMachineAccess access;
  access.live = []() -> std::optional<std::string> { return machineUuid(); };
  access.save = recordCredentialMachine;
  access.holdAside = [](const Credentials& value) { (void)holdCopiedCredentials(value); };
  return access;
}
}
