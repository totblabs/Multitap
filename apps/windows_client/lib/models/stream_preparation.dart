import '../l10n/l10n.dart';

/// Fixed diagnostic codes only. Arbitrary host errors never become UI text.
enum StreamPreparation {
  restart,
  console,
  login,
  identity,
  session,
  resume,
  locked,
  // The Host reports this only while the user signed in at the desktop's
  // console is not a Windows administrator and its session worker cannot
  // prepare the virtual display for that reason: an older Host, or a current
  // one on the older display manager. Otherwise a current Host streams a
  // standard user's session.
  administrator,
  recovery,
  display,
  verification,
  configuration,
  maintenance,
  checking;

  const StreamPreparation();
  String get badge => switch (this) {
    StreamPreparation.restart => l10n.restartNeeded,
    StreamPreparation.console => l10n.consoleNeeded,
    StreamPreparation.login => l10n.signNeeded,
    StreamPreparation.identity => l10n.checkWindowsAccount,
    StreamPreparation.session => l10n.checkWindowsScreen,
    StreamPreparation.resume => l10n.resumeWindows,
    StreamPreparation.locked => l10n.windowsLocked,
    StreamPreparation.administrator => l10n.adminAccountNeeded,
    StreamPreparation.recovery => l10n.repairNeeded,
    StreamPreparation.display => l10n.notReady,
    StreamPreparation.verification => l10n.checkDriver,
    StreamPreparation.configuration => l10n.checkSettings,
    StreamPreparation.maintenance => l10n.settingUpScreen,
    StreamPreparation.checking => l10n.checkStatus,
  };

  String get detail => switch (this) {
    StreamPreparation.restart => l10n.restartWindowsGetVirtualDisplay,
    StreamPreparation.console => l10n.virtualDisplayCantPreparedRdp,
    StreamPreparation.login => l10n.signWindowsConnect,
    StreamPreparation.identity => l10n.windowsAccountMustCheckedHost,
    StreamPreparation.session => l10n.couldntCheckWindowsScreenIf,
    StreamPreparation.resume => l10n.youCanBringWindowsScreen,
    StreamPreparation.locked => l10n.windowsLockedUnlockConnect,
    StreamPreparation.administrator => l10n.windowsAccountSignedNowIsnt,
    StreamPreparation.recovery => l10n.virtualDisplayNeedsRepairTell,
    StreamPreparation.display => l10n.virtualDisplayIsntReadyIf2,
    StreamPreparation.verification => l10n.couldntCheckVirtualDisplayDriver,
    StreamPreparation.configuration => l10n.virtualDisplaySettingsNeedChecking,
    StreamPreparation.maintenance => l10n.hostSettingUpVirtualDisplay,
    StreamPreparation.checking => l10n.couldntCheckVirtualDisplayStatus,
  };

  static StreamPreparation? fromCode(Object? code) => switch (code) {
    'reboot_required' => restart,
    'interactive_console_required' => console,
    'no_interactive_session' || 'windows_session_unknown' => session,
    'windows_login_required' => login,
    // An absent/unresolved saved account does not prove nobody is logged in.
    'windows_identity_required' => identity,
    'rdp_session_active' || 'rdp_session_disconnected' => resume,
    'windows_session_locked' => locked,
    'windows_identity_mismatch' ||
    'windows_session_ambiguous' ||
    'console_in_use' => configuration,
    'administrator_required' => administrator,
    'viewport_recovery_required' ||
    'viewport_rollback_incomplete' ||
    'native_display_snapshot_invalid' ||
    'native_display_topology_mismatch' ||
    'native_topology_recovery_required' => recovery,
    'owned_virtual_output_inactive' ||
    'owned_virtual_display_not_ready' ||
    'native_display_not_ready' ||
    'native_topology_not_ready' ||
    'native_display_change_pending' ||
    'viewport_sources_unavailable' ||
    'monitor_not_available' => display,
    'native_display_provider_unavailable' ||
    'native_display_configuration_untrusted' ||
    'driver_hash_mismatch' ||
    'driver_signature_untrusted' ||
    'driver_catalog_membership_invalid' ||
    'viewport_package_path_invalid' ||
    'viewport_package_path_untrusted' ||
    'viewport_package_not_administrator_controlled' ||
    'viewport_installed_driver_unavailable' ||
    'viewport_installed_driver_untrusted' ||
    'viewport_installed_driver_mismatch' => verification,
    'existing_virtual_display_requires_review' ||
    'shared_vendor_path_conflict' ||
    'viewport_settings_unavailable' ||
    'viewport_settings_untrusted' ||
    'viewport_settings_read_failed' ||
    'viewport_original_settings_invalid' ||
    'viewport_original_settings_modified' ||
    'viewport_settings_modified_externally' ||
    'mode_catalog_update_required' => configuration,
    'host_maintenance' || 'viewport_busy' => maintenance,
    'viewport_probe_pending' ||
    'viewport_probe_failed' ||
    'viewport_probe_timeout' ||
    'viewport_support_unavailable' ||
    'viewport_connector_identity_unavailable' ||
    'viewport_connector_identity_ambiguous' ||
    'viewport_ccd_query_failed' ||
    'viewport_ccd_source_invalid' ||
    'viewport_ccd_snapshot_limit' ||
    'viewport_topology_changed' ||
    'viewport_ccd_validate_failed' ||
    'mode_catalog_failed' ||
    'mode_catalog_timeout' ||
    'mode_catalog_cancelled' ||
    'mode_catalog_registration_timeout' ||
    'mode_catalog_not_verified' ||
    'owned_device_restart_failed' ||
    'owned_device_restart_status_unavailable' => checking,
    _ => null,
  };
}
