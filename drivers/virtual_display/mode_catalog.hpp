#pragma once

// Upgrade only the verified owned runtime XML, preserving the original pin and
// the existing viewport journal/recovery format. Never change signed files.
namespace mode_catalog {
using viewport::Owned;
static std::vector<Mode> plan(std::vector<Mode> extra,
                             const std::vector<Owned> &active) {
  for (const auto &mode : common_modes::all())
    if (!common_modes::original(mode) &&
        std::none_of(extra.begin(), extra.end(), [&](const Mode &item) {
          return common_modes::same(item, mode);
        })) extra.push_back(mode);
  while (extra.size() > 32) {
    const auto retired = std::find_if(extra.begin(), extra.end(), [&](const Mode &mode) {
      return !common_modes::contains(mode) &&
          std::none_of(active.begin(), active.end(), [&](const Owned &item) {
            return mode.width == DWORD(item.display.width) &&
                mode.height == DWORD(item.display.height) &&
                mode.refreshRate == item.display.frequency;
          });
    });
    require(retired != extra.end(), "viewport_mode_limit");
    extra.erase(retired);
  }
  return extra;
}
static unsigned count(const std::vector<Mode> &available) {
  return unsigned(std::count_if(common_modes::all().begin(), common_modes::all().end(),
      [&](const Mode &required) {
        return std::any_of(available.begin(), available.end(), [&](const Mode &mode) {
          return common_modes::same(mode, required);
        });
      }));
}
static bool configured(const std::vector<Mode> &extra) {
  return std::all_of(common_modes::all().begin(), common_modes::all().end(),
      [&](const Mode &mode) {
        return common_modes::original(mode) ||
            std::any_of(extra.begin(), extra.end(), [&](const Mode &item) {
              return common_modes::same(item, mode);
            });
      });
}
static const char *guard(const Status &state, bool recovery,
                        bool console, bool unlocked) {
  if (recovery) return "viewport_recovery_required";
  if (state.foreignDeviceCount || state.deviceCount > 1)
    return "existing_virtual_display_requires_review";
  if (state.vendorPathConflict) return "shared_vendor_path_conflict";
  if (!state.owned || state.deviceCount != 1)
    return "no_exclusive_owned_virtual_display";
  if (state.restartRequired) return "reboot_required";
  if (!console || !unlocked) return "interactive_console_required";
  if (!state.ready) return "owned_virtual_display_not_ready";
  if (state.displays.empty()) return "owned_virtual_output_inactive";
  return "";
}
static bool deferred(const std::string &reason) {
  return reason == "reboot_required" || reason == "interactive_console_required" ||
      reason == "owned_virtual_output_inactive" || reason == "owned_virtual_display_not_ready";
}
static std::string safeReason(const std::string &reason) {
  static const std::set<std::string> values = {
      "administrator_required", "viewport_recovery_required", "viewport_busy", "viewport_acl_failed",
      "existing_virtual_display_requires_review", "shared_vendor_path_conflict",
      "no_exclusive_owned_virtual_display", "reboot_required", "interactive_console_required",
      "owned_virtual_output_inactive", "owned_virtual_display_not_ready", "viewport_mode_limit",
      "viewport_settings_unavailable", "viewport_settings_untrusted", "viewport_settings_modified_externally",
      "viewport_original_settings_modified", "viewport_original_settings_invalid",
      "viewport_package_path_invalid", "viewport_package_path_untrusted",
      "viewport_package_not_administrator_controlled", "viewport_private_file_create_failed",
      "viewport_private_file_write_failed", "viewport_settings_changed",
      "viewport_directory_untrusted", "viewport_path_untrusted", "viewport_settings_replace_failed",
      "viewport_settings_recovery_required", "viewport_journal_cleanup_failed", "viewport_journal_write_failed",
      "viewport_backup_write_failed", "driver_hash_mismatch", "driver_signature_untrusted",
      "driver_file_unavailable", "driver_path_reparse_point", "driver_path_type_invalid",
      "viewport_installed_driver_unavailable", "viewport_installed_driver_untrusted",
      "viewport_installed_driver_mismatch", "owned_source_identity_changed", "owned_device_restart_failed",
      "owned_device_restart_status_unavailable", "owned_virtual_sources_unavailable", "viewport_sources_unavailable",
      "viewport_connector_identity_changed", "viewport_connector_identity_ambiguous",
      "mode_catalog_registration_timeout", "mode_catalog_not_verified", "viewport_topology_changed",
      "viewport_mode_not_applied", "viewport_mode_test_failed", "viewport_mode_apply_failed",
      "viewport_ccd_query_failed", "viewport_ccd_validate_failed", "viewport_ccd_apply_failed",
      "viewport_ccd_source_invalid", "viewport_ccd_mode_invalid", "viewport_ccd_snapshot_limit",
      "viewport_ccd_owned_identity_invalid", "viewport_external_layout_changed", "viewport_required_mode_unavailable",
      "viewport_rollback_incomplete", "mode_catalog_update_required"};
  return values.count(reason) ? reason : "mode_catalog_failed";
}
struct Result {
  bool ready{}, applied{}, changed{}, restarted{}, restartAttempted{}, pending=true,
      isDeferred{}, rollbackAttempted{}, rollbackSucceeded{}, catalogConfigured{};
  unsigned verifiedModes{}, verifiedSources{};
  std::string reason;
};
template<class Access> static Result transaction(Access &access) {
  Result result;
  result.catalogConfigured = access.configured();
  result.verifiedModes = access.verifiedModes();
  result.verifiedSources = access.verifiedSources();
  const auto next = access.nextXml();
  if (result.catalogConfigured && result.verifiedModes == common_modes::all().size()) {
    result.ready = result.applied = true;
    result.pending = false;
    return result;
  }
  access.saveOriginal();
  access.saveJournal();
  try {
    if (next != access.currentXml()) {
      access.replaceXml(next);
      result.changed = true;
    }
    result.restartAttempted = true;
    access.restart();
    result.restarted = true;
    access.waitCatalog();
    access.restoreTopology();
    result.verifiedModes = access.verifiedModes();
    result.verifiedSources = access.verifiedSources();
    require(result.verifiedModes == common_modes::all().size() && result.verifiedSources > 0,
            "mode_catalog_not_verified");
    access.clearJournal();
    result.catalogConfigured = result.ready = result.applied = true;
    result.pending = false;
  } catch (const std::exception &failure) {
    result.reason = safeReason(failure.what());
    result.rollbackAttempted = true;
    result.rollbackSucceeded = access.rollback(result.restarted);
    result.catalogConfigured = access.configuredAfterRollback();
    result.verifiedModes = result.verifiedSources = 0;
    if (!result.rollbackSucceeded) result.reason = "viewport_rollback_incomplete";
  }
  return result;
}
class RuntimeAccess {
  fs::path dir_;
  viewport::Snapshot before_;
  std::string base_, next_;
  unsigned verifiedModes_{}, verifiedSources_{};
  void inspect() {
    const auto mapping = viewport::sources(before_.instance);
    unsigned minimum = unsigned(common_modes::all().size()), sources = 0;
    for (const auto &owned : before_.owned) {
      const auto source = std::find_if(mapping.begin(), mapping.end(), [&](const viewport::Source &item) {
        return item.connector == owned.connector;
      });
      require(source != mapping.end(), "viewport_connector_identity_changed");
      minimum = std::min(minimum, count(supportedModes(source->name)));
      ++sources;
    }
    verifiedModes_ = sources ? minimum : 0;
    verifiedSources_ = sources;
  }
public:
  RuntimeAccess(fs::path directory, const Status &state, const std::string &base,
                const std::string &current)
      : dir_(std::move(directory)),
        base_(base) {
    require(!state.displays.empty(), "owned_virtual_output_inactive");
    before_ = viewport::snapshot(state, current, state.displays.front().name);
    next_ = viewport::generated(base_, plan(viewport::addedModes(base_, current), before_.owned));
    inspect();
  }
  bool configured() const { return mode_catalog::configured(viewport::addedModes(base_, before_.xml)); }
  unsigned verifiedModes() { inspect(); return verifiedModes_; }
  unsigned verifiedSources() const { return verifiedSources_; }
  const std::string &currentXml() const { return before_.xml; }
  const std::string &nextXml() const { return next_; }
  void saveOriginal() {
    if (GetFileAttributesW((dir_ / viewport::backupName).c_str()) == INVALID_FILE_ATTRIBUTES)
      viewport::writeNew(dir_ / viewport::backupName, base_);
  }
  void saveJournal() { viewport::writeNew(dir_ / viewport::journalName, viewport::journal(before_)); }
  void replaceXml(const std::string &next) { viewport::replace(dir_ / L"vdd_settings.xml", next); }
  void restart() { viewport::restart(before_.instance); }
  void waitCatalog() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      require(consoleDesktop() && unlockedDefaultDesktop(), "interactive_console_required");
      const auto state = status();
      if (state.restartRequired) viewport::observedRestartRequired = true;
      require(!state.restartRequired, "reboot_required");
      if (state.owned && state.ready && state.deviceCount == 1 && !state.foreignDeviceCount &&
          !state.vendorPathConflict && !_wcsicmp(state.id.c_str(), before_.instance.c_str())) {
        try {
          viewport::verifyInstalledBinding(before_.instance);
          inspect();
          if (verifiedModes_ == common_modes::all().size()) return;
        } catch (const std::exception &) {}
      }
      Sleep(100);
    }
    throw std::runtime_error("mode_catalog_registration_timeout");
  }
  void restoreTopology() { viewport::apply(before_, before_.owned); }
  void clearJournal() {
    require(DeleteFileW((dir_ / viewport::journalName).c_str()), "viewport_journal_cleanup_failed");
  }
  bool rollback(bool &restarted) { return viewport::rollback(dir_, before_, restarted); }
  bool configuredAfterRollback() const {
    try { return mode_catalog::configured(viewport::addedModes(base_, viewport::readFile(dir_ / L"vdd_settings.xml"))); }
    catch (...) { return false; }
  }
};
static void output(const Result &result, bool readOnly) {
  const auto state = status();
  std::cout << std::boolalpha << "{\"modeCatalogRevision\":" << common_modes::revision
      << ",\"modeCatalogReady\":" << result.ready << ",\"catalogApplied\":" << result.applied
      << ",\"catalogChanged\":" << result.changed << ",\"catalogConfigured\":" << result.catalogConfigured
      << ",\"readOnly\":" << readOnly << ",\"requiredModeCount\":" << common_modes::all().size()
      << ",\"verifiedModeCount\":" << result.verifiedModes << ",\"verifiedSourceCount\":" << result.verifiedSources
      << ",\"deviceRestarted\":" << result.restarted << ",\"deviceRestartAttempted\":" << result.restartAttempted
      << ",\"configurationPending\":" << result.pending << ",\"deferred\":" << result.isDeferred
      << ",\"reason\":" << display_json::quoteUtf8(result.reason)
      << ",\"error\":" << display_json::quoteUtf8(result.isDeferred ||
          (readOnly && result.reason == "mode_catalog_update_required") ? "" : result.reason)
      << ",\"rollbackAttempted\":" << result.rollbackAttempted << ",\"rollbackSucceeded\":" << result.rollbackSucceeded
      << ",\"rebootRequired\":" << (state.restartRequired || viewport::observedRestartRequired)
      << ",\"recoveryRequired\":" << viewportRecoveryPending(state)
      << ",\"state\":" << statusJson(state) << "}\n";
}
static int command(bool readOnly = false) {
  Result result;
  try {
    require(elevated(), "administrator_required");
    const auto initial = status();
    const auto reason = guard(initial, viewportRecoveryPending(initial), consoleDesktop(), unlockedDefaultDesktop());
    if (*reason && !deferred(reason)) throw std::runtime_error(reason);
    const auto dir = viewport::ownedDirectory(initial, true);
    viewport::DirectoryLock directoryLock(dir);
    viewport::verifyPayload(dir);
    viewport::verifyInstalledBinding(initial.id);
    const auto base = viewport::baseXml(dir), current = viewport::readFile(dir / L"vdd_settings.xml");
    const auto extra = viewport::addedModes(base, current);
    result.catalogConfigured = configured(extra);
    require(GetFileAttributesW((dir / viewport::journalName).c_str()) == INVALID_FILE_ATTRIBUTES,
            "viewport_recovery_required");
    if (*reason) {
      result.isDeferred = true; result.reason = reason;
      output(result, readOnly); return 0;
    }
    if (readOnly) {
      RuntimeAccess access(dir, initial, base, current);
      result.verifiedModes = access.verifiedModes(); result.verifiedSources = access.verifiedSources();
      result.ready = result.catalogConfigured && result.verifiedModes == common_modes::all().size();
      result.pending = !result.ready;
      if (!result.ready) result.reason = "mode_catalog_update_required";
    } else {
      PSECURITY_DESCRIPTOR security = nullptr;
      require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &security, nullptr), "viewport_acl_failed");
      SECURITY_ATTRIBUTES attributes{sizeof(attributes), security, FALSE};
      viewport::Handle mutex(CreateMutexW(&attributes, FALSE, L"Global\\Multitap.Display.Viewport"));
      LocalFree(security);
      require(mutex.h && mutex.h != INVALID_HANDLE_VALUE, "viewport_busy");
      const auto acquired = WaitForSingleObject(mutex.h, 0);
      require(acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED, "viewport_busy");
      struct Release { HANDLE h; ~Release() { ReleaseMutex(h); } } release{mutex.h};
      viewport::context(initial.id);
      const auto fresh = status();
      const auto freshReason = guard(fresh, viewportRecoveryPending(fresh), consoleDesktop(), unlockedDefaultDesktop());
      require(!*freshReason, *freshReason ? freshReason : "mode_catalog_failed");
      require(samePath(viewport::ownedDirectory(fresh, true).wstring(), dir.wstring()),
              "owned_source_identity_changed");
      require(viewport::readFile(dir / L"vdd_settings.xml") == current && viewport::baseXml(dir) == base,
              "viewport_settings_changed");
      RuntimeAccess access(dir, fresh, base, current);
      result = transaction(access);
    }
  } catch (const std::exception &failure) { result.reason = safeReason(failure.what()); }
  output(result, readOnly);
  return result.ready || result.isDeferred || (readOnly && result.reason == "mode_catalog_update_required") ? 0 : 1;
}
} // namespace mode_catalog
