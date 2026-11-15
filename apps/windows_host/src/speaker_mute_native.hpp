#pragma once
// speaker-mute-v1 on Windows (speaker_mute.hpp has the rules): the default
// render endpoint's mute through IAudioEndpointVolume, the record file, who
// holds it, and the lock the workers of one Windows session share.
// Everything here runs in the session worker, as the signed-in user: the
// endpoints are that user's, and the stream's capture that must run before a
// mute is in the same process. The service (session 0) never touches a mute.
// The audio objects live in speaker_mute_native.cpp: endpointvolume.h and
// ntsecapi.h cannot be in one translation unit (both name `Network`).
#include "speaker_mute.hpp"
#include "machine.hpp"

namespace host {
// Per user, beside the display-scale record: the worker writes it as the signed-in user.
inline std::filesystem::path speakerMuteRecordPath() { return knownFolder(FOLDERID_LocalAppData) / L"Multitap" / L"Host" / L"speaker-mute.state"; }

inline uint64_t speakerMuteFileTime(const FILETIME& time) { return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
inline SpeakerMuteOwner speakerMuteSelf() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return {GetCurrentProcessId(), 1};
  return {GetCurrentProcessId(), std::max<uint64_t>(1, speakerMuteFileTime(created))};
}
// Whether the worker that wrote the record still runs. A process that cannot be looked at counts as
// running: its mute is never taken away on a guess.
inline bool speakerMuteOwnerAlive(const SpeakerMuteOwner& owner) {
  if (owner.none()) return false;
  WinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner.pid));
  if (!process.value) return GetLastError() != ERROR_INVALID_PARAMETER;  // no process has that id
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process.value, &created, &exited, &kernel, &user)) return true;
  if (speakerMuteFileTime(created) != owner.started) return false;  // the id belongs to a later process
  DWORD code = 0;
  return !GetExitCodeProcess(process.value, &code) || code == STILL_ACTIVE;  // ended, its handle still held by someone
}

// The lock every worker of this Windows session takes around one keeper call. Returns its hold, or nothing
// when it could not be taken in time.
inline std::shared_ptr<void> speakerMuteLock() {
  const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\MultitapHost.SpeakerMute");
  if (!mutex) return {};
  const DWORD waited = WaitForSingleObject(mutex, 5000);
  if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) { CloseHandle(mutex); return {}; }
  return std::shared_ptr<void>(mutex, [](void* handle) { ReleaseMutex(handle); CloseHandle(handle); });
}

// The endpoints and the record of the calling process's user. The caller's thread has COM (multithreaded).
// Only SetMute is ever called on an endpoint: no volume level.
SpeakerMuteAccess nativeSpeakerMuteAccess();
// A worker's start (its thread has COM): what an earlier worker left muted gets its sound back. Returns the
// stage 49 values to journal. Without a record no audio object is made at all.
std::vector<uint32_t> speakerMuteRecover() noexcept;
}  // namespace host
