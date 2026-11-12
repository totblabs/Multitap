#pragma once
// Which sentence the removal window shows for a failure code
// (setup_uninstall_ui.hpp), and nothing else.
//
// The sentence follows what the person can do about it. "Another Multitap
// install or removal is running" (uninstall_installation_in_progress,
// setup_uninstall.hpp InstallationGate) only has to be waited for; a program
// or file still in use has to be closed; codes about this setup's own file
// (uninstall_running_image_*, uninstall_running_setup_required) and about its
// records get the general sentence, never "the program is in use".
#include <string>
#include "setup_text.hpp"

namespace multitap::installation {
enum class RemovalFailure { installation, partial, inUse, otherInstaller };

inline RemovalFailure removalFailure(const std::string& code) {
  const auto has = [&](const char* part) { return code.find(part) != std::string::npos; };
  if (has("partial_removal")) return RemovalFailure::partial;
  // Another install, update or removal of the same product holds the gate.
  if (has("installation_in_progress") || has("installer_gate_unavailable")) return RemovalFailure::otherInstaller;
  // A program or file of the product is still in use.
  if (has("busy") || has("still_running") || has("in_use")) return RemovalFailure::inUse;
  return RemovalFailure::installation;
}
// Two lines at most (the window has room for two), in [language]
// (setup_text.hpp; the person's Windows display language by default).
inline const wchar_t* removalFailureText(RemovalFailure failure, ui::Language language) {
  using Id = setup_text::Id;
  switch (failure) {
    case RemovalFailure::partial: return setup_text::text(Id::RemovalPartial, language);
    case RemovalFailure::inUse: return setup_text::text(Id::RemovalInUse, language);
    case RemovalFailure::otherInstaller: return setup_text::text(Id::RemovalOtherInstaller, language);
    default: return setup_text::text(Id::RemovalInstallation, language);
  }
}
inline const wchar_t* removalFailureText(RemovalFailure failure) { return removalFailureText(failure, ui::processLanguage()); }
}  // namespace multitap::installation
