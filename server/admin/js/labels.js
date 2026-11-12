// Korean labels for the fixed codes the broker, Host and Windows client
// report. Codes never reach the page as free text: an unknown code is shown
// verbatim in a monospace chip so nothing is invented.
import { t } from './i18n.js';

export const messages = {
  server_access_unavailable: t('labels.messages.server_access_unavailable'),
  server_update_unavailable: t('labels.messages.server_update_unavailable'), server_restart_unavailable: t('labels.messages.server_restart_unavailable'), update_busy: t('labels.messages.update_busy'), update_check_required: t('labels.messages.update_check_required'), update_version_changed: t('labels.messages.update_version_changed'), update_confirmation_required: t('labels.messages.update_confirmation_required'), update_recovery_required: t('labels.messages.update_recovery_required'), update_manual_docker: t('labels.messages.update_manual_docker'),
  invalid_credentials: t('labels.messages.invalid_credentials'),
  password_confirmation_mismatch: t('labels.messages.password_confirmation_mismatch'), proxmox_import_required: t('labels.messages.proxmox_import_required'),
  authentication_required: t('labels.messages.authentication_required'), session_expired: t('labels.messages.session_expired'),
  credentials_change_required: t('labels.messages.credentials_change_required'), username_must_change: t('labels.messages.username_must_change'), setup_not_required: t('labels.messages.setup_not_required'),
  setup_code_invalid: t('labels.messages.setup_code_invalid'), setup_code_required: t('labels.messages.setup_code_required'),
  too_many_setup_attempts: t('labels.messages.too_many_setup_attempts'),
  csrf_required: t('labels.messages.csrf_required'), origin_required: t('labels.messages.origin_required'),
  last_admin_required: t('labels.messages.last_admin_required'), cannot_delete_self: t('labels.messages.cannot_delete_self'),
  primary_admin_required: t('labels.messages.primary_admin_required'),
  primary_admin_protected: t('labels.messages.primary_admin_protected'),
  invalid_clipboard_policy: t('labels.messages.invalid_clipboard_policy'),
  invalid_device_policy: t('labels.messages.invalid_device_policy'),
  sound_card_vm_only: t('labels.messages.sound_card_vm_only'),
  invalid_sound_card_add: t('labels.messages.invalid_sound_card_add'), invalid_assignment_entry: t('labels.messages.invalid_assignment_entry'), invalid_desktop_ids: t('labels.messages.invalid_desktop_ids'),
  user_not_found: t('labels.messages.user_not_found'),
  account_identity_required: t('labels.messages.account_identity_required'),
  invalid_account_identity: t('labels.messages.invalid_account_identity'),
  account_identity_changed: t('labels.messages.account_identity_changed'),
  host_identity_required: t('labels.messages.host_identity_required'),
  invalid_host_identity: t('labels.messages.invalid_host_identity'),
  host_removal_confirmation_required: t('labels.messages.host_removal_confirmation_required'),
  host_registration_changed: t('labels.messages.host_registration_changed'),
  host_registration_busy: t('labels.messages.host_registration_busy'),
  host_activity_unavailable: t('labels.messages.host_activity_unavailable'),
  host_not_registered: t('labels.messages.host_not_registered'),
  assignment_changed: t('labels.messages.assignment_changed'), invalid_permission_entries: t('labels.messages.invalid_permission_entries'),
  invalid_username: t('labels.messages.invalid_username'),
  password_length_10_to_256: t('labels.messages.password_length_10_to_256'), invalid_text: t('labels.messages.invalid_text'),
  already_exists: t('labels.messages.already_exists'), user_or_desktop_unavailable: t('labels.messages.user_or_desktop_unavailable'),
  too_many_attempts: t('labels.messages.too_many_attempts'), login_busy: t('labels.messages.login_busy'),
  proxmox_not_configured: t('labels.messages.proxmox_not_configured'), invalid_proxmox_endpoint: t('labels.messages.invalid_proxmox_endpoint'),
  proxmox_setup_required: t('labels.messages.proxmox_setup_required'),
  invalid_proxmox_token_id: t('labels.messages.invalid_proxmox_token_id'), proxmox_secret_required: t('labels.messages.proxmox_secret_required'),
  invalid_proxmox_ca: t('labels.messages.invalid_proxmox_ca'), invalid_proxmox_fingerprint: t('labels.messages.invalid_proxmox_fingerprint'), proxmox_certificate_error: t('labels.messages.proxmox_certificate_error'),
  proxmox_unreachable: t('labels.messages.proxmox_unreachable'), proxmox_permission_denied: t('labels.messages.proxmox_permission_denied'),
  proxmox_request_failed: t('labels.messages.proxmox_request_failed'), vm_already_imported: t('labels.messages.vm_already_imported'),
  desktop_not_linked: t('labels.messages.desktop_not_linked'), proxmox_vm_not_found: t('labels.messages.proxmox_vm_not_found'),
  proxmox_has_linked_desktops: t('labels.messages.proxmox_has_linked_desktops'),
  proxmox_settings_changed: t('labels.messages.proxmox_settings_changed'),
  proxmox_machine_uuid_missing: t('labels.messages.proxmox_machine_uuid_missing'),
  host_machine_mismatch: t('labels.messages.host_machine_mismatch'),
  host_request_not_found: t('labels.messages.host_request_not_found'),
  host_request_expired: t('labels.messages.host_request_expired'),
  host_request_not_pending: t('labels.messages.host_request_not_pending'),
  host_request_vm_mismatch: t('labels.messages.host_request_vm_mismatch'),
  host_request_duplicate: t('labels.messages.host_request_duplicate'),
  host_request_duplicate_unverifiable: t('labels.messages.host_request_duplicate_unverifiable'),
  host_request_code_collision: t('labels.messages.host_request_code_collision'),
  host_request_pc_code_required: t('labels.messages.host_request_pc_code_required'),
  host_request_pc_not_allowed: t('labels.messages.host_request_pc_not_allowed'),
  host_request_pc_target_changed: t('labels.messages.host_request_pc_target_changed'),
  host_request_pc_name_required: t('labels.messages.host_request_pc_name_required'),
  host_request_code_required: t('labels.messages.host_request_code_required'),
  host_vm_mapping_required: t('labels.messages.host_vm_mapping_required'),
  host_request_match_changed: t('labels.messages.host_request_match_changed'),
  invalid_host_approval: t('labels.messages.invalid_host_approval'),
  desktop_changed: t('labels.messages.desktop_changed'),
  local_pc_unlinked_confirmation_required: t('labels.messages.local_pc_unlinked_confirmation_required'),
  host_request_matches_vm: t('labels.messages.host_request_matches_vm'),
  host_request_conflict: t('labels.messages.host_request_conflict'),
  invalid_local_pc_approval: t('labels.messages.invalid_local_pc_approval'),
  invalid_local_pc_name: t('labels.messages.invalid_local_pc_name'),
  local_pc_already_registered: t('labels.messages.local_pc_already_registered'),
  local_pc_registration_changed: t('labels.messages.local_pc_registration_changed'),
  host_replacement_confirmation_required: t('labels.messages.host_replacement_confirmation_required'),
  host_update_unsupported: t('labels.messages.host_update_unsupported'),
  host_update_in_progress: t('labels.messages.host_update_in_progress'),
  host_update_current: t('labels.messages.host_update_current'), host_update_not_newer: t('labels.messages.host_update_not_newer'),
  host_release_unavailable: t('labels.messages.host_release_unavailable'), host_offline: t('labels.messages.host_offline'),
  host_version_unknown: t('labels.messages.host_version_unknown'), host_update_unavailable: t('labels.messages.host_update_unavailable'),
  host_diagnostics_update_required: t('labels.messages.host_diagnostics_update_required'), diagnostics_rate_limit: t('labels.messages.diagnostics_rate_limit'),
  host_diagnostics_unavailable: t('labels.messages.host_diagnostics_unavailable'), power_in_progress: t('labels.messages.power_in_progress'),
  // 백업·복원 (server/src/backup.mjs, backup-format.mjs, update/restore-engine.mjs).
  backup_unavailable: t('labels.messages.backup_unavailable'), backup_busy: t('labels.messages.backup_busy'),
  backup_rate_limited: t('labels.messages.backup_rate_limited'), backup_disk_space: t('labels.messages.backup_disk_space'),
  backup_passphrase_too_short: t('labels.messages.backup_passphrase_too_short'), backup_passphrase_invalid: t('labels.messages.backup_passphrase_invalid'), backup_passphrase_mismatch: t('labels.messages.backup_passphrase_mismatch'),
  backup_too_large: t('labels.messages.backup_too_large'), backup_download_expired: t('labels.messages.backup_download_expired'),
  backup_file_invalid: t('labels.messages.backup_file_invalid'), backup_unreadable: t('labels.messages.backup_unreadable'),
  backup_format_newer: t('labels.messages.backup_format_newer'), backup_from_newer_server: t('labels.messages.backup_from_newer_server'), backup_schema_newer: t('labels.messages.backup_schema_newer'),
  backup_database_invalid: t('labels.messages.backup_database_invalid'), backup_database_unsupported: t('labels.messages.backup_database_unsupported'),
  backup_no_administrator: t('labels.messages.backup_no_administrator'), backup_helper_failed: t('labels.messages.backup_helper_failed'), backup_helper_timeout: t('labels.messages.backup_helper_timeout'),
  backup_upload_not_found: t('labels.messages.backup_upload_not_found'), backup_upload_offset: t('labels.messages.backup_upload_offset'), backup_upload_state: t('labels.messages.backup_upload_state'), backup_upload_incomplete: t('labels.messages.backup_upload_incomplete'),
  backup_inspection_required: t('labels.messages.backup_inspection_required'),
  restore_unavailable: t('labels.messages.restore_unavailable'), restore_busy: t('labels.messages.restore_busy'),
  restore_recovery_required: t('labels.messages.restore_recovery_required'), restore_other_recovery_required: t('labels.messages.restore_other_recovery_required'),
  restore_confirmation_required: t('labels.messages.restore_confirmation_required'), restore_result_unknown: t('labels.messages.restore_result_unknown'),
  restore_operation_reused: t('labels.messages.restore_operation_reused'), restore_command_invalid: t('labels.messages.restore_command_invalid'), restore_fence_invalid: t('labels.messages.restore_fence_invalid'),
  current_password_required: t('labels.messages.current_password_required'), current_password_incorrect: t('labels.messages.current_password_incorrect'), mfa_code_required: t('labels.messages.mfa_code_required'),
  mfa_invalid_code: t('labels.messages.mfa_invalid_code'), invalid_mfa_fields: t('labels.messages.invalid_mfa_fields'), mfa_requires_https: t('labels.messages.mfa_requires_https'),
  proxmox_settings_unreadable: t('labels.messages.proxmox_settings_unreadable'), proxmox_validation_unavailable: t('labels.messages.proxmox_validation_unavailable'),
};

export const updateActive = new Set(['checking', 'downloading', 'verifying', 'backing_up', 'applying', 'restarting', 'health_check', 'interrupted']);
export const updateLabels = { idle: t('labels.updateLabels.idle'), checking: t('labels.updateLabels.checking'), available: t('labels.updateLabels.available'), current: t('labels.updateLabels.current'), downloading: t('labels.updateLabels.downloading'), verifying: t('labels.updateLabels.verifying'), backing_up: t('labels.updateLabels.backing_up'), applying: t('labels.updateLabels.applying'), restarting: t('labels.updateLabels.restarting'), health_check: t('labels.updateLabels.health_check'), succeeded: t('labels.updateLabels.succeeded'), rolled_back: t('labels.updateLabels.rolled_back'), failed: t('labels.updateLabels.failed'), interrupted: t('labels.updateLabels.interrupted'), recovery_required: t('labels.updateLabels.recovery_required'), unavailable: t('labels.updateLabels.unavailable'), not_configured: t('labels.updateLabels.not_configured') };
export const deploymentLabels = { docker: 'Docker Compose', systemd: 'Linux systemd', external: t('labels.deploymentLabels.external'), unknown: t('labels.deploymentLabels.unknown') };

export const hostUpdateActive = new Set(['waiting', 'dispatched', 'accepted', 'downloading', 'verifying', 'installing']);
export const hostUpdateLabels = { waiting: t('labels.hostUpdateLabels.waiting'), dispatched: t('labels.hostUpdateLabels.dispatched'), accepted: t('labels.hostUpdateLabels.accepted'), downloading: t('labels.hostUpdateLabels.downloading'), verifying: t('labels.hostUpdateLabels.verifying'), installing: t('labels.hostUpdateLabels.installing'), completed: t('labels.hostUpdateLabels.completed'), failed: t('labels.hostUpdateLabels.failed'), rejected: t('labels.hostUpdateLabels.rejected'), unknown: t('labels.hostUpdateLabels.unknown'), expired: t('labels.hostUpdateLabels.expired') };
// Fixed Host/broker codes (server/src/host-update.mjs hostUpdateCodes).
export const hostUpdateCodeText = { session_active: t('labels.hostUpdateCodeText.session_active'), busy: t('labels.hostUpdateCodeText.busy'), not_newer: t('labels.hostUpdateCodeText.not_newer'), same_version: t('labels.hostUpdateCodeText.same_version'), invalid_request: t('labels.hostUpdateCodeText.invalid_request'),
  download_failed: t('labels.hostUpdateCodeText.download_failed'), download_size_mismatch: t('labels.hostUpdateCodeText.download_size_mismatch'), hash_mismatch: t('labels.hostUpdateCodeText.hash_mismatch'), not_executable: t('labels.hostUpdateCodeText.not_executable'), stage_failed: t('labels.hostUpdateCodeText.stage_failed'), launch_failed: t('labels.hostUpdateCodeText.launch_failed'),
  install_failed: t('labels.hostUpdateCodeText.install_failed'), install_result_missing: t('labels.hostUpdateCodeText.install_result_missing'),
  certificate_trust_required: t('labels.hostUpdateCodeText.certificate_trust_required'), driver_install_required: t('labels.hostUpdateCodeText.driver_install_required'), existing_install_required: t('labels.hostUpdateCodeText.existing_install_required'),
  // Legacy code for any installer attention (display, WinFsp or shortcut).
  // An unattended update reports this when the Host has no virtual display driver:
  // opening Host settings on that machine finishes the install with one consent.
  display_attention: t('labels.hostUpdateCodeText.display_attention'), restart_required: t('labels.hostUpdateCodeText.restart_required'), result_unreported: t('labels.hostUpdateCodeText.result_unreported'),
  host_no_response: t('labels.hostUpdateCodeText.host_no_response'), host_no_progress: t('labels.hostUpdateCodeText.host_no_progress'), host_not_returned: t('labels.hostUpdateCodeText.host_not_returned'), host_disconnected: t('labels.hostUpdateCodeText.host_disconnected'), wait_expired: t('labels.hostUpdateCodeText.wait_expired'),
  signature_invalid: t('labels.hostUpdateCodeText.signature_invalid') };
// One short stage per Host update, the words of the client's bulk window
// (bulk_host_updates.dart stage): 다시 연결 is installing while the Host's
// service restarts to finish. Null without an update.
export function hostUpdateStage(view) {
  const operation = view?.operation, status = operation?.status;
  if (!operation) return null;
  if (['waiting', 'dispatched', 'accepted'].includes(status)) return { label: t('common.107'), tone: 'blue' };
  if (status === 'downloading') return { label: t('common.9'), tone: 'blue' };
  if (status === 'installing' && view.connected === false) return { label: t('common.106'), tone: 'blue' };
  if (status === 'verifying' || status === 'installing') return { label: t('common.108'), tone: 'blue' };
  if (status === 'completed') return { label: t('common.109'), tone: operation.rebootRequired || operation.code === 'display_attention' ? 'amber' : 'green' };
  if (status === 'unknown') return { label: t('common.17'), tone: 'amber' };
  return { label: t('common.110'), tone: 'red' };
}
export const hostUpdateDescription = t('labels.hostUpdateDescription');

// A text of a fixed table for a code that came from outside (a server, Host or
// client value, a page address): only the table's own entries, so a code such
// as "constructor" or "__proto__" is never read from Object.prototype.
export const own = (table, key) => (typeof key === 'string' || typeof key === 'number') && Object.hasOwn(table, key) ? table[key] : undefined;

export const osNames = { windows: 'Windows', linux: 'Linux', other: t('labels.osNames.other'), unknown: t('labels.osNames.unknown') };
export const powerNames = { running: t('labels.powerNames.running'), stopped: t('labels.powerNames.stopped'), unknown: t('labels.powerNames.unknown') };

// Host diagnostics (server/src/host-diagnostics.mjs vocabularies).
const displayReasons = { '': t('common.57'), runtime_recovery_required: t('labels.displayReasons.runtime_recovery_required'), native_stream_profile_recovery_required: t('labels.displayReasons.native_stream_profile_recovery_required'), transport_open_failed: t('labels.displayReasons.transport_open_failed'), controller_start_failed: t('labels.displayReasons.controller_start_failed'), heartbeat_failed: t('labels.displayReasons.heartbeat_failed'), internal_failure: t('labels.displayReasons.internal_failure'), native_display_not_ready: t('labels.displayReasons.native_display_not_ready'), interactive_console_required: t('labels.displayReasons.interactive_console_required'), reboot_required: t('labels.displayReasons.reboot_required'), native_topology_recovery_required: t('labels.displayReasons.native_topology_recovery_required'), no_interactive_session: t('labels.displayReasons.no_interactive_session'), administrator_required: t('labels.displayReasons.administrator_required'), viewport_probe_pending: t('labels.displayReasons.viewport_probe_pending'), viewport_probe_failed: t('labels.displayReasons.viewport_probe_failed'), viewport_probe_timeout: t('labels.displayReasons.viewport_probe_timeout'), viewport_recovery_required: t('labels.displayReasons.viewport_recovery_required'), owned_virtual_display_not_ready: t('labels.displayReasons.owned_virtual_display_not_ready'), native_display_provider_unavailable: t('labels.displayReasons.native_display_provider_unavailable'), native_display_snapshot_invalid: t('labels.displayReasons.native_display_snapshot_invalid'), native_display_configuration_untrusted: t('labels.displayReasons.native_display_configuration_untrusted'), native_display_topology_mismatch: t('labels.displayReasons.native_display_topology_mismatch'), native_display_change_pending: t('labels.displayReasons.native_display_change_pending'), native_topology_not_ready: t('labels.displayReasons.native_topology_not_ready') };
export const displayReason = code => own(displayReasons, code ?? '') ?? code;
const activationReasons = { '': t('labels.1'), interactive_console_required: t('labels.activationReasons.interactive_console_required'), native_display_snapshot_invalid: t('labels.activationReasons.native_display_snapshot_invalid'), native_display_configuration_untrusted: t('labels.activationReasons.native_display_configuration_untrusted'), native_display_topology_mismatch: t('labels.activationReasons.native_display_topology_mismatch'), native_topology_query_failed: t('labels.activationReasons.native_topology_query_failed'), native_topology_foreign_changed: t('labels.activationReasons.native_topology_foreign_changed'), native_topology_target_unavailable: t('labels.activationReasons.native_topology_target_unavailable'), native_topology_source_changed: t('labels.activationReasons.native_topology_source_changed'), native_topology_transform_mismatch: t('labels.activationReasons.native_topology_transform_mismatch'), native_topology_refresh_mismatch: t('labels.activationReasons.native_topology_refresh_mismatch'), native_topology_scanline_mismatch: t('labels.activationReasons.native_topology_scanline_mismatch'), native_topology_size_mismatch: t('labels.activationReasons.native_topology_size_mismatch'), native_topology_format_mismatch: t('labels.activationReasons.native_topology_format_mismatch'), native_topology_placement_mismatch: t('labels.activationReasons.native_topology_placement_mismatch'), native_topology_plan_invalid: t('labels.activationReasons.native_topology_plan_invalid'), native_topology_mode_invalid: t('labels.activationReasons.native_topology_mode_invalid'), native_topology_baseline_required: t('labels.activationReasons.native_topology_baseline_required'), native_topology_validate_failed: t('labels.activationReasons.native_topology_validate_failed'), native_topology_changed: t('labels.activationReasons.native_topology_changed'), native_topology_apply_failed: t('labels.activationReasons.native_topology_apply_failed'), native_topology_not_applied: t('labels.activationReasons.native_topology_not_applied'), native_topology_failed: t('labels.activationReasons.native_topology_failed'), native_activation_observation_pending: t('labels.activationReasons.native_activation_observation_pending'), native_activation_lease_changed: t('labels.activationReasons.native_activation_lease_changed'), native_activation_worker_unavailable: t('labels.activationReasons.native_activation_worker_unavailable') };
export const activationReason = code => own(activationReasons, code ?? '') ?? t('labels.2');
export const nativePhases = { ready: t('labels.nativePhases.ready'), unknown: t('labels.nativePhases.unknown'), empty: t('labels.nativePhases.empty'), starting: t('labels.nativePhases.starting'), stable: t('labels.nativePhases.stable'), changing: t('labels.nativePhases.changing'), stopped: t('labels.nativePhases.stopped'), recovery: t('labels.nativePhases.recovery') };
// Fixed host codes only (see server/src/host-diagnostics.mjs recoveryCodes).
export const recoveryLabels = { none: t('labels.recoveryLabels.none'), started: t('labels.recoveryLabels.started'), profile_missing: t('labels.recoveryLabels.profile_missing'), session_active: t('labels.recoveryLabels.session_active'), reservation_held: t('labels.recoveryLabels.reservation_held'), recovery_running: t('labels.recoveryLabels.recovery_running'), console_session_required: t('labels.recoveryLabels.console_session_required'), logon_unverified: t('labels.recoveryLabels.logon_unverified'), sdk_not_quiescent: t('labels.recoveryLabels.sdk_not_quiescent'), original_service_alive: t('labels.recoveryLabels.original_service_alive'), original_worker_alive: t('labels.recoveryLabels.original_worker_alive'), retry_budget_exhausted: t('labels.recoveryLabels.retry_budget_exhausted'), profile_not_restored: t('labels.recoveryLabels.profile_not_restored'), retirement_prepared: t('labels.recoveryLabels.retirement_prepared'), launch_profile_mismatch: t('labels.recoveryLabels.launch_profile_mismatch'), launch_worker_alive: t('labels.recoveryLabels.launch_worker_alive'), records_unreadable: t('labels.recoveryLabels.records_unreadable'), ok: t('labels.recoveryLabels.ok'), failed: t('labels.recoveryLabels.failed'), succeeded: t('labels.recoveryLabels.succeeded'), expired_restore: t('labels.recoveryLabels.expired_restore'), retirement: t('labels.recoveryLabels.retirement'), expired_retirement: t('labels.recoveryLabels.expired_retirement'), in_service: t('labels.recoveryLabels.in_service') };
export const consoleReasons = { console_ready: t('labels.consoleReasons.console_ready'), native_provider_required: t('labels.consoleReasons.native_provider_required'), reboot_required: t('labels.consoleReasons.reboot_required'), virtual_device_not_ready: t('labels.consoleReasons.virtual_device_not_ready'), windows_session_unknown: t('labels.consoleReasons.windows_session_unknown'), windows_session_locked: t('labels.consoleReasons.windows_session_locked'), windows_identity_required: t('labels.consoleReasons.windows_identity_required'), windows_identity_mismatch: t('labels.consoleReasons.windows_identity_mismatch'), windows_login_required: t('labels.consoleReasons.windows_login_required'), windows_session_ambiguous: t('labels.consoleReasons.windows_session_ambiguous'), rdp_session_active: t('labels.consoleReasons.rdp_session_active'), rdp_session_disconnected: t('labels.consoleReasons.rdp_session_disconnected'), console_unavailable: t('labels.consoleReasons.console_unavailable'), console_in_use: t('labels.consoleReasons.console_in_use'), operation_cancelled: t('labels.consoleReasons.operation_cancelled'), console_busy: t('labels.consoleReasons.console_busy'), console_timeout: t('labels.consoleReasons.console_timeout'), console_transfer_failed: t('labels.consoleReasons.console_transfer_failed'), console_transfer_pending: t('labels.consoleReasons.console_transfer_pending'), console_binding_changed: t('labels.consoleReasons.console_binding_changed'), worker_failed: t('labels.consoleReasons.worker_failed'), administrator_required: t('labels.consoleReasons.administrator_required') };

// Windows client diagnostics (server/src/client-diagnostics.mjs).
export const clientEvents = { appStarted: t('labels.clientEvents.appStarted'), signIn: t('labels.clientEvents.signIn'), inventory: t('labels.clientEvents.inventory'), brokerRequest: t('labels.clientEvents.brokerRequest'), displayConfigure: t('labels.clientEvents.displayConfigure'), streamStart: t('labels.clientEvents.streamStart'), streamReady: t('labels.clientEvents.streamReady'), streamEnd: t('labels.clientEvents.streamEnd'), viewportRequest: t('labels.clientEvents.viewportRequest'), viewportResult: t('labels.clientEvents.viewportResult'), updateCheck: t('labels.clientEvents.updateCheck'), updateResult: t('labels.clientEvents.updateResult'), updateDownload: t('labels.clientEvents.updateDownload'), streamStats: t('labels.clientEvents.streamStats'), clipboard: t('labels.clientEvents.clipboard'), usb: t('labels.clientEvents.usb') };
export const clientCodes = { unknown_failure: t('labels.clientCodes.unknown_failure'), network_error: t('labels.clientCodes.network_error'), invalid_response: t('labels.clientCodes.invalid_response'), server_restarting: t('labels.clientCodes.server_restarting'), request_failed: t('labels.clientCodes.request_failed'), authentication_required: t('labels.clientCodes.authentication_required'), session_expired: t('labels.clientCodes.session_expired'), desktop_not_found: t('labels.clientCodes.desktop_not_found'), host_unavailable: t('labels.clientCodes.host_unavailable'), host_disconnected: t('labels.clientCodes.host_disconnected'), host_offline: t('labels.clientCodes.host_offline'), host_maintenance: t('labels.clientCodes.host_maintenance'), session_cleanup_pending: t('labels.clientCodes.session_cleanup_pending'), capabilities_pending: t('labels.clientCodes.capabilities_pending'), virtual_display_required: t('labels.clientCodes.virtual_display_required'), no_interactive_session: t('labels.clientCodes.no_interactive_session'), streaming_unavailable: t('labels.clientCodes.streaming_unavailable'), display_control_unavailable: t('labels.clientCodes.display_control_unavailable'), display_not_supported: t('labels.clientCodes.display_not_supported'), display_busy: t('labels.clientCodes.display_busy'), display_in_progress: t('labels.clientCodes.display_in_progress'), display_operation_failed: t('labels.clientCodes.display_operation_failed'), display_operation_unknown: t('labels.clientCodes.display_operation_unknown'), display_operation_timeout: t('labels.clientCodes.display_operation_timeout'), display_operation_not_found: t('labels.clientCodes.display_operation_not_found'), display_configuration_failed: t('labels.clientCodes.display_configuration_failed'), display_count_mismatch: t('labels.clientCodes.display_count_mismatch'), display_timeout: t('labels.clientCodes.display_timeout'), worker_failed: t('labels.clientCodes.worker_failed'), operation_cancelled: t('labels.clientCodes.operation_cancelled'), invalid_display_request: t('labels.clientCodes.invalid_display_request'), invalid_display_count: t('labels.clientCodes.invalid_display_count'), too_many_display_attempts: t('labels.clientCodes.too_many_display_attempts'), power_in_progress: t('labels.clientCodes.power_in_progress'), rdp_not_supported: t('labels.clientCodes.rdp_not_supported'), desktop_busy: t('labels.clientCodes.desktop_busy'), session_busy: t('labels.clientCodes.session_busy'), controller_in_use: t('labels.clientCodes.controller_in_use'), taken_over: t('labels.clientCodes.taken_over'), other_user: t('labels.clientCodes.other_user'), initial_viewport_unavailable: t('labels.clientCodes.initial_viewport_unavailable'), invalid_initial_viewport: t('labels.clientCodes.invalid_initial_viewport'), viewport_encoder_limits_unavailable: t('labels.clientCodes.viewport_encoder_limits_unavailable'), invalid_viewport_encoder_limits: t('labels.clientCodes.invalid_viewport_encoder_limits'), track_streaming_unavailable: t('labels.clientCodes.track_streaming_unavailable'), local_display_layout_unavailable: t('labels.clientCodes.local_display_layout_unavailable'), local_display_layout_unsupported: t('labels.clientCodes.local_display_layout_unsupported'), console_preparation_failed: t('labels.clientCodes.console_preparation_failed'), console_preparation_timeout: t('labels.clientCodes.console_preparation_timeout'), native_display_not_ready: t('labels.clientCodes.native_display_not_ready'), native_topology_recovery_required: t('labels.clientCodes.native_topology_recovery_required'), initial_viewport_timeout: t('labels.clientCodes.initial_viewport_timeout'), initial_viewport_mismatch: t('labels.clientCodes.initial_viewport_mismatch'), initial_frame_timeout: t('labels.clientCodes.initial_frame_timeout'), video_timeout: t('labels.clientCodes.video_timeout'), output_video_timeout: t('labels.clientCodes.output_video_timeout'), decoder_failed: t('labels.clientCodes.decoder_failed'), output_decoder_stalled: t('labels.clientCodes.output_decoder_stalled'), decoder_stalled: t('labels.clientCodes.decoder_stalled'), media_status_failed: t('labels.clientCodes.media_status_failed'), resize_timeout: t('labels.clientCodes.resize_timeout'), resize_frame_timeout: t('labels.clientCodes.resize_frame_timeout'), capture_failed: t('labels.clientCodes.capture_failed'), encoder_failed: t('labels.clientCodes.encoder_failed'), host_revoked: t('labels.clientCodes.host_revoked'), server_stopping: t('labels.clientCodes.server_stopping'), connection_overloaded: t('labels.clientCodes.connection_overloaded'), display_changed: t('labels.clientCodes.display_changed'), session_ended: t('labels.clientCodes.session_ended'), authorization_revoked: t('labels.clientCodes.authorization_revoked'), relay_closed: t('labels.clientCodes.relay_closed'), host_timeout: t('labels.clientCodes.host_timeout'), loopback_error: t('labels.clientCodes.loopback_error'), client_backpressure: t('labels.clientCodes.client_backpressure'), viewport_frame_timeout: t('labels.clientCodes.viewport_frame_timeout'), viewport_apply_timeout: t('labels.clientCodes.viewport_apply_timeout'), signature_invalid: t('labels.clientCodes.signature_invalid') };
// The relay's detail of a Host connection that ended (server/src/relay.mjs
// hostErrorDetail and host-closing-v1), as clients record it.
Object.assign(clientCodes, { host_frame_invalid: t('labels.3'), host_message_too_large: t('labels.4'), host_text_invalid: t('labels.5'), host_message_fragmented: t('labels.6'), host_transport_error: t('labels.7'), host_service_stopping: t('labels.8'), host_protocol_rejected: t('labels.9'), host_closing: t('labels.10') });
Object.assign(clientCodes, { usb_capture_failed: t('labels.11'), usb_capture_timeout: t('labels.12'), usb_request_stuck: t('labels.13'), usb_device_not_returned: t('labels.14'), usb_device_error: t('labels.15'), usb_device_busy: t('labels.16'), usb_device_removed: t('labels.17'), usb_vm_detached: t('labels.18'), usb_user_stopped: t('common.125'), usb_policy_changed: t('labels.19'), usb_session_closed: t('labels.20'), usb_attach_failed: t('labels.21'), usb_class_not_granted: t('labels.22'), usb_class_denied: t('labels.23'), usb_hid_boot_denied: t('labels.24'), usb_isochronous_denied: t('labels.25'), usb_descriptor_invalid: t('labels.26'), usb_limit_reached: t('labels.27'), usb_vhci_unavailable: t('labels.28'), usb_transport_closed: t('labels.29'), usbdk_missing: t('labels.30'), usbdk_install_failed: t('labels.31'), usbdk_signature_invalid: t('labels.32') });
// Fixed hostAction codes (apps/windows_host/src/service.hpp recordHostEvent).
export const hostActions = { 200: t('labels.hostActions.200'), 201: t('labels.hostActions.201'), 202: t('labels.hostActions.202'), 203: t('labels.hostActions.203'), 210: t('labels.hostActions.210'), 211: t('labels.hostActions.211'), 212: t('labels.hostActions.212'), 214: t('labels.hostActions.214'), 215: t('labels.hostActions.215'), 216: t('labels.hostActions.216'), 217: t('labels.hostActions.217'), 218: t('labels.hostActions.218'), 219: t('labels.hostActions.219'), 220: t('labels.hostActions.220'), 230: t('labels.hostActions.230'), 240: t('labels.hostActions.240'), 241: t('labels.hostActions.241'), 242: t('labels.hostActions.242'), 247: t('labels.hostActions.247'), 250: 'USB', 253: t('labels.hostActions.253'), 262: t('labels.hostActions.262'), 263: t('labels.hostActions.263'), 264: t('labels.hostActions.264'), 270: t('labels.hostActions.270'), 271: t('labels.hostActions.271'), 273: t('labels.hostActions.273'), 274: t('labels.hostActions.274'), 275: t('labels.hostActions.275'), 276: t('labels.hostActions.276'), 280: t('labels.hostActions.280'), 281: t('labels.hostActions.281'), 282: t('labels.hostActions.282'), 285: t('labels.hostActions.285'), 291: t('labels.hostActions.291') };
// hostAction 274 (apps/windows_host/src/boot_timeline.hpp): one service start's way to 연결 준비됨. uptimeMs = Windows uptime
// when the service started; each stage = ms after that start, in this order; a stage not reached is absent. ok false (stage 2):
// not ready ten minutes after the start.
export const hostBootStages = [['configMs', t('labels.33')], ['brokerMs', t('labels.34')], ['virtualDisplayMs', t('common.122')], ['logonMs', t('labels.35')], ['sessionWorkerMs', t('labels.36')], ['readyMs', t('labels.37')]];
// hostAction 270 stage (apps/windows_host/src/udp_probe.hpp) and hostAction 271 stage and reason (udp_media_path.hpp).
export const hostUdpProbeText = { 1: t('labels.hostUdpProbeText.1'), 2: t('labels.hostUdpProbeText.2'), 3: t('labels.hostUdpProbeText.3'), 4: t('labels.hostUdpProbeText.4') };
// hostAction 280 stage (apps/windows_host/src/thumbnail_capture.hpp ThumbnailState), recorded on a change only; kib: picture size.
export const hostThumbnailText = { 0: t('labels.hostThumbnailText.0'), 1: t('labels.hostThumbnailText.1'), 2: t('labels.hostThumbnailText.2'), 3: t('labels.hostThumbnailText.3'), 4: t('labels.hostThumbnailText.4'), 5: t('labels.hostThumbnailText.5') };
export const hostUdpPathText = { 1: t('labels.hostUdpPathText.1'), 2: t('labels.hostUdpPathText.2'), 3: t('labels.hostUdpPathText.3') };
export const hostUdpPathReasonText = { 1: t('labels.hostUdpPathReasonText.1'), 2: t('labels.hostUdpPathReasonText.2'), 3: t('labels.hostUdpPathReasonText.3'), 4: t('labels.hostUdpPathReasonText.4'), 5: t('labels.hostUdpPathReasonText.5'), 6: t('labels.hostUdpPathReasonText.6'), 7: t('labels.hostUdpPathReasonText.7'), 8: t('labels.hostUdpPathReasonText.8') };
// hostAction 253 (apps/windows_host/src/clipboard_diagnostic.hpp): one clipboard transfer on the desktop. stage: what happened; format; error: the
// fixed reason that went to or came from the peer (1-based index into hostClipboardReasons, 99 other); cause: the internal condition
// (1-based index into hostClipboardCauses, 255 not in the table); items and kib: count and size. test/host-clipboard-diagnostics.test.mjs
// compares these tables with the header.
export const hostClipboardStageText = { 1: t('labels.hostClipboardStageText.1'), 2: t('labels.hostClipboardStageText.2'), 3: t('labels.hostClipboardStageText.3'), 4: t('labels.hostClipboardStageText.4'), 5: t('labels.hostClipboardStageText.5'), 6: t('labels.hostClipboardStageText.6'), 7: t('labels.hostClipboardStageText.7'), 8: t('labels.hostClipboardStageText.8'), 9: t('labels.hostClipboardStageText.9'),
  // Observation of this desktop's own copies (no transfer): see clipboard_diagnostic.hpp ClipboardNote / ClipboardFormats.
  10: t('labels.hostClipboardStageText.10'), 11: t('labels.hostClipboardStageText.11'), 12: t('labels.hostClipboardStageText.12'), 13: t('labels.hostClipboardStageText.13'), 14: t('labels.hostClipboardStageText.14') };
// hostAction 253 stage 10-14: error indexes this table (1-based); items is a bit mask of what was on the clipboard.
export const hostClipboardNoteText = [t('labels.38'), t('labels.39'), t('labels.40'), t('labels.41'), t('labels.42'), t('labels.43'), t('labels.44'), t('labels.45'), t('labels.46')];
export const hostClipboardFormatBits = [[1, t('labels.47')], [2, t('labels.48')], [4, t('labels.49')], [8, t('labels.50')], [16, t('labels.51')], [32, 'HTML']];
export const hostClipboardFormatText = { 1: t('labels.hostClipboardFormatText.1'), 2: t('labels.hostClipboardFormatText.2'), 3: t('labels.hostClipboardFormatText.3'), 4: t('labels.hostClipboardFormatText.4'), 5: t('labels.hostClipboardFormatText.5'), 6: t('labels.hostClipboardFormatText.6') };
export const hostClipboardReasons = ["clipboard_empty","clipboard_busy","unsupported_clipboard_format","clipboard_read_failed","clipboard_write_failed","clipboard_file_unavailable","clipboard_transfer_failed","no_interactive_session","clipboard_cancelled","clipboard_policy_denied","clipboard_changed","clipboard_limits_exceeded","clipboard_disk_full","clipboard_file_locked","clipboard_selected_file_changed","clipboard_offer_expired","clipboard_path_too_long","clipboard_text_too_large","clipboard_entry_count_limit","clipboard_depth_limit","clipboard_image_too_large"];
export const hostClipboardCauses = ["Invalid clipboard transfer","Invalid clipboard chunk size","Noncanonical clipboard base64","Unsupported clipboard packet","Clipboard worker stopped","Clipboard window class failed","clipboard_allocation_failed","clipboard_busy","clipboard_cancelled","clipboard_changed","clipboard_chunk_limit","clipboard_chunk_out_of_order_or_bounds","clipboard_data_bounds","clipboard_depth_limit","clipboard_disk_full","clipboard_duplicate_file_name","clipboard_empty","clipboard_empty_failed","clipboard_entry_count_limit","clipboard_expiry_busy","clipboard_expiry_invalid","clipboard_file_count_limit","clipboard_file_list_invalid","clipboard_file_locked","clipboard_file_name_or_size_limit","clipboard_file_not_complete","clipboard_file_not_materialized","clipboard_file_path_invalid","clipboard_file_unavailable","clipboard_format_not_allowed","clipboard_hash_create_failed","clipboard_hash_finish_failed","clipboard_hash_mismatch","clipboard_hash_property_failed","clipboard_hash_provider_failed","clipboard_hash_update_failed","clipboard_html_invalid","clipboard_image_invalid","clipboard_image_too_large","clipboard_invalid_unicode","clipboard_lock_failed","clipboard_offer_expired","clipboard_owner_not_this_process","clipboard_owner_unavailable","clipboard_path_too_long","clipboard_policy_denied","clipboard_private_acl_failed","clipboard_regular_files_only","clipboard_rtf_invalid","clipboard_selected_file_changed","clipboard_selected_file_unavailable","clipboard_text_not_terminated","clipboard_text_too_large","clipboard_text_too_large_or_invalid","clipboard_timeout","clipboard_total_size_limit","clipboard_transfer_directory_failed","clipboard_transfer_directory_lock_failed","clipboard_transfer_directory_untrusted","clipboard_transfer_failed","clipboard_transfer_file_create_failed","clipboard_transfer_flush_failed","clipboard_transfer_hash_mismatch","clipboard_transfer_id_failed","clipboard_transfer_not_active","clipboard_transfer_not_complete","clipboard_transfer_parent_invalid","clipboard_transfer_parent_not_absolute","clipboard_transfer_parent_untrusted","clipboard_transfer_write_failed","clipboard_transport_closed","clipboard_unexpected_ack","clipboard_user_identity_failed","clipboard_user_token_failed","clipboard_window_full","clipboard_write_failed","unsupported_clipboard_format","clipboard_desktop_unavailable"];

// What the timeline shows for the causes that are sentences in the header (the rest are codes).
export const hostClipboardCauseText = { 'Invalid clipboard transfer': t('labels.52'), 'Invalid clipboard chunk size': t('labels.53'), 'Noncanonical clipboard base64': t('labels.54'), 'Unsupported clipboard packet': t('labels.55'), 'Clipboard worker stopped': t('labels.56'), 'Clipboard window class failed': t('labels.57') };
// hostAction 253 `win32` (apps/windows_host/src/clipboard_diagnostic.hpp, native/windows_clipboard writeFailureDetail): bits 0-15 the Win32 error (low 16 bits of an
// HRESULT) of the clipboard call that failed, bits 16-17 who held the clipboard when it could not be opened (1 another program, 2 this worker itself, 3 another thread of
// this worker), bit 20 = the write was tried again after the worker released what it itself held.
export const hostClipboardHolderText = ['', t('labels.58'), t('labels.59'), t('labels.60')];
export function hostClipboardWin32Text(value) {
  if (!Number.isFinite(value) || value <= 0) return null;
  const parts = [t('common.126', { value: value & 0xffff })];
  const holder = hostClipboardHolderText[(value >> 16) & 3];
  if (holder) parts.push(holder);
  if (value & (1 << 20)) parts.push(t('labels.61'));
  return parts.join(' · ');
}// hostAction 240 stage (apps/windows_host/src/agent.hpp mediaDropped).
export const hostMediaDropText = { 1: t('labels.hostMediaDropText.1'), 2: t('labels.hostMediaDropText.2') };
// hostAction 242 stage (apps/windows_host/src/print_redirection.hpp PrintStage);
// Hosts 0.2.73-0.2.76 sent the same record as 240 with an `ok` field.
export const hostPrintText = { 1: t('labels.hostPrintText.1'), 2: t('labels.hostPrintText.2'), 3: t('labels.hostPrintText.3'), 4: t('labels.hostPrintText.4'), 5: t('labels.hostPrintText.5'), 6: t('labels.hostPrintText.6'), 7: t('labels.hostPrintText.7'), 8: t('labels.hostPrintText.8'), 9: t('labels.hostPrintText.9'), 11: t('labels.hostPrintText.11'), 13: t('labels.hostPrintText.13'), 14: t('labels.hostPrintText.14'), 15: t('labels.hostPrintText.15'), 16: t('labels.hostPrintText.16'), 17: t('labels.hostPrintText.17'), 18: t('labels.hostPrintText.18'), 19: t('labels.hostPrintText.19'), 20: t('labels.hostPrintText.20'), 21: t('labels.hostPrintText.21'), 22: t('labels.hostPrintText.22'), 23: t('labels.hostPrintText.23'), 24: t('labels.hostPrintText.24'), 25: t('labels.hostPrintText.25'), 26: t('labels.hostPrintText.26'), 27: t('labels.hostPrintText.27'), 28: t('labels.hostPrintText.28'), 29: t('labels.hostPrintText.29'), 30: t('labels.hostPrintText.30'), 31: t('labels.hostPrintText.31') };
// hostAction 242 stage 30 (the desktop's default printer could not be set): `error` is the
// reason (print_default.hpp DefaultFailure) and `cause` the Windows error behind it.
export const hostPrintDefaultReasonText = { 1: t('labels.hostPrintDefaultReasonText.1'), 2: t('labels.hostPrintDefaultReasonText.2'), 3: t('labels.hostPrintDefaultReasonText.3'), 4: t('labels.hostPrintDefaultReasonText.4'), 5: t('labels.hostPrintDefaultReasonText.5'), 6: t('labels.hostPrintDefaultReasonText.6'), 7: t('labels.hostPrintDefaultReasonText.7'), 8: t('labels.hostPrintDefaultReasonText.8') };
// hostAction 247 (drive_redirection.hpp record): `stage`, and `error` = drive_protocol.hpp diagnosticCode;
// `cause` is the Windows error behind a failed mapping.
export const hostDriveText = { 1: t('labels.hostDriveText.1'), 2: t('labels.hostDriveText.2'), 3: t('labels.hostDriveText.3'), 4: t('labels.hostDriveText.4'), 5: t('labels.hostDriveText.5'), 6: t('labels.hostDriveText.6'), 7: t('labels.hostDriveText.7'), 8: t('labels.hostDriveText.8'), 9: t('labels.hostDriveText.9'), 10: t('labels.hostDriveText.10'), 11: t('labels.hostDriveText.11'), 12: t('labels.hostDriveText.12'), 13: t('labels.hostDriveText.13'), 14: t('labels.hostDriveText.14'), 15: t('labels.hostDriveText.15'), 16: t('labels.hostDriveText.16') };
export const hostDriveErrorText = { 1: t('labels.hostDriveErrorText.1'), 2: t('labels.hostDriveErrorText.2'), 3: t('labels.hostDriveErrorText.3'), 4: t('labels.hostDriveErrorText.4'), 5: t('labels.hostDriveErrorText.5'), 6: t('labels.hostDriveErrorText.6'), 7: t('labels.hostDriveErrorText.7'), 8: t('labels.hostDriveErrorText.8'), 9: t('labels.hostDriveErrorText.9'), 10: t('labels.hostDriveErrorText.10'), 11: t('labels.hostDriveErrorText.11'), 12: t('labels.hostDriveErrorText.12') };
// hostAction 241 stage = index (apps/windows_host/src/agent.hpp hostChannelCloseCodes); error = Win32 error.
export const hostChannelCloseCodes = ['', 'transport_closed', 'send_failed', 'heartbeat_timeout', 'protocol_rejected', 'send_queue_stalled', 'service_stopping'];
export const hostChannelCloseText = { transport_closed: t('labels.hostChannelCloseText.transport_closed'), send_failed: t('labels.hostChannelCloseText.send_failed'), heartbeat_timeout: t('labels.hostChannelCloseText.heartbeat_timeout'), protocol_rejected: t('labels.hostChannelCloseText.protocol_rejected'), send_queue_stalled: t('labels.hostChannelCloseText.send_queue_stalled'), service_stopping: t('labels.hostChannelCloseText.service_stopping') };
// hostAction 230 (apps/windows_host/src/host_update.hpp recordHostUpdateEvent):
// stage = state number (1-based), error = code index.
export const hostUpdateEventStates = ['accepted', 'downloading', 'verifying', 'installing', 'completed', 'failed', 'deferred', 'rejected'];
export const hostUpdateEventCodes = ['', 'invalid_request', 'busy', 'not_newer', 'same_version', 'session_active', 'download_failed', 'download_size_mismatch', 'hash_mismatch', 'not_executable',
  'stage_failed', 'launch_failed', 'install_failed', 'install_result_missing', 'certificate_trust_required', 'driver_install_required', 'existing_install_required', 'display_attention', 'restart_required', 'result_unreported',
  'signature_invalid'];
export const hostUpdateStateText = { accepted: t('labels.hostUpdateStateText.accepted'), downloading: t('labels.hostUpdateStateText.downloading'), verifying: t('labels.hostUpdateStateText.verifying'), installing: t('labels.hostUpdateStateText.installing'), completed: t('labels.hostUpdateStateText.completed'), failed: t('labels.hostUpdateStateText.failed'), deferred: t('labels.hostUpdateStateText.deferred'), rejected: t('labels.hostUpdateStateText.rejected') };
// hostAction 250 (apps/windows_host/src/usb_core.hpp UsbStage): streaming USB redirection on the desktop. `stage` below;
// `error` is a reason (hostUsbReasons, 1-based) for stages 10-16 and 20-23, the errno for 24, seconds for 26 and
// Windows' problem code for 28; `expected` is a device count, or the class of the request for 17 and 24-26 (usbRequestClassText),
// or seconds since the attach for 28. Stage 27 (device summary): items = requests, kib, cause = failed, expected = cancelled.
// test/usb-labels.test.mjs compares these tables with the header.
export const hostUsbText = { 1: t('labels.hostUsbText.1'), 2: t('labels.hostUsbText.2'), 3: t('labels.hostUsbText.3'), 4: t('labels.hostUsbText.4'), 5: t('labels.hostUsbText.5'), 6: t('labels.hostUsbText.6'), 7: t('labels.hostUsbText.7'), 8: t('labels.hostUsbText.8'), 10: t('labels.hostUsbText.10'), 11: t('labels.hostUsbText.11'), 12: t('labels.hostUsbText.12'), 13: t('labels.hostUsbText.13'), 14: t('labels.hostUsbText.14'), 15: t('labels.hostUsbText.15'), 16: t('labels.hostUsbText.16'), 17: t('labels.hostUsbText.17'), 18: t('labels.hostUsbText.18'), 20: t('labels.hostUsbText.20'), 21: t('labels.hostUsbText.21'), 22: t('labels.hostUsbText.22'), 23: t('labels.hostUsbText.23'),
  24: t('labels.hostUsbText.24'), 25: t('labels.hostUsbText.25'), 26: t('labels.hostUsbText.26'), 27: t('labels.hostUsbText.27'), 28: t('labels.hostUsbText.28'),
  29: t('labels.hostUsbText.29'), 30: t('labels.hostUsbText.30'), 31: t('labels.hostUsbText.31'), 32: t('labels.hostUsbText.32') };
// The desktop's USB controller (Host diagnostics devices.usbController; hostAction 250 stage 8 sends its index as `expected`).
export const usbControllerCodes = ['none', 'started', 'stopped', 'unmarked', 'duplicates', 'foreign', 'multiple', 'unknown'];
export const usbControllerText = { none: t('labels.usbControllerText.none'), started: t('labels.usbControllerText.started'), stopped: t('labels.usbControllerText.stopped'), unmarked: t('labels.usbControllerText.unmarked'), duplicates: t('labels.usbControllerText.duplicates'), foreign: t('labels.usbControllerText.foreign'), multiple: t('labels.usbControllerText.multiple'), unknown: t('labels.usbControllerText.unknown') };
// hostAction 273: virtual display adapter inventory and cleanup. Stage 0: expected = in use, stale = previous
// versions, missingOwned = duplicates, fallback = unproven, items = other products, ok = nothing of ours left. Stages 1 and 5:
// items = removed, missing = waiting for a restart, error = failed.
export const hostDisplayInventoryText = { 0: t('labels.hostDisplayInventoryText.0'), 1: t('labels.hostDisplayInventoryText.1'), 5: t('labels.hostDisplayInventoryText.5') };
// hostAction 215 (apps/windows_host/src/qemu_console_route.hpp): the Host stops its own virtual outputs so that the Console
// (the Proxmox console) shows Windows when no worker can change the display there. stage 1: parked (ok) or refused
// (error = QemuConsoleParkRefusal), cause = why no worker could act (qemuConsoleParkCause), changed = the outputs were
// stopped or the stop was tried. stage 2: the park ended and the outputs come back.
export const hostConsoleParkText = { 1: t('labels.hostConsoleParkText.1'), 2: t('labels.hostConsoleParkText.2') };
export const hostConsoleParkRefusalText = { 1: t('labels.hostConsoleParkRefusalText.1'), 2: t('labels.hostConsoleParkRefusalText.2') };
export const hostConsoleParkCauseText = { 1: t('labels.hostConsoleParkCauseText.1'), 2: t('labels.hostConsoleParkCauseText.2'), 3: t('labels.hostConsoleParkCauseText.3'), 4: t('labels.hostConsoleParkCauseText.4') };
// hostAction 200 stage 246 (docs/system-input.md "Records"): where the stream's input went, as counts and levels only.
// error = kind | value << 8. The value is a count, except: 6 and 16 a Windows error, 10 worker level | window level << 4,
// 18 and 19 worker level | reason << 4. test/host-action-labels.test.mjs compares the kinds with that document.
export const hostInputKindText = { 1: t('labels.hostInputKindText.1'), 2: t('labels.hostInputKindText.2'), 3: t('labels.hostInputKindText.3'), 4: t('labels.hostInputKindText.4'),
  5: t('labels.hostInputKindText.5'), 6: t('labels.hostInputKindText.6'), 7: t('labels.hostInputKindText.7'), 8: t('labels.hostInputKindText.8'), 9: t('labels.hostInputKindText.9'),
  10: t('labels.hostInputKindText.10'), 11: t('labels.hostInputKindText.11'), 12: t('labels.hostInputKindText.12'), 13: t('labels.hostInputKindText.13'), 14: t('labels.hostInputKindText.14'),
  15: t('labels.hostInputKindText.15'), 16: t('labels.hostInputKindText.16'), 17: t('labels.hostInputKindText.17'), 18: t('labels.hostInputKindText.18'), 19: t('labels.hostInputKindText.19'),
  20: t('labels.hostInputKindText.20'), 21: t('labels.hostInputKindText.21'), 22: t('labels.hostInputKindText.22'), 23: t('labels.hostInputKindText.23'), 24: t('labels.hostInputKindText.24'),
  25: t('labels.hostInputKindText.25') };
export const hostInputLevelText = { 2: t('labels.hostInputLevelText.2'), 3: t('labels.hostInputLevelText.3'), 4: t('labels.hostInputLevelText.4'), 15: t('labels.hostInputLevelText.15') };
export const hostInputInjectorReasonText = { 1: t('labels.hostInputInjectorReasonText.1'), 2: t('labels.hostInputInjectorReasonText.2') };
// hostAction 276 (apps/windows_host/src/settings_rpc.hpp): Host settings' finish of an install, one record per step.
// stage 1: the virtual display driver; error = host_prerequisites.hpp errorNumber. stage 2: WinFsp; error =
// settings_rpc.hpp winfspErrorNumber. Both tables are append only (99: a code not listed);
// test/host-action-labels.test.mjs compares them with the headers.
export const hostFinishStepText = { 1: t('labels.hostFinishStepText.1'), 2: 'WinFsp' };
export const hostFinishDisplayCodes = ['', 'confirmation_required', 'certificate_trust_required', 'certificate_store_failed', 'native_display_catalog_untrusted', 'native_display_package_missing', 'native_display_package_mismatch',
  'native_display_package_unprotected', 'native_display_publisher_mismatch', 'native_display_certificate_expired', 'native_selection_invalid', 'native_selected_device_unverified', 'pnp_inf_invalid', 'pnp_unavailable', 'pnp_create_failed',
  'pnp_driver_unavailable', 'pnp_register_failed', 'pnp_install_failed', 'pnp_instance_invalid', 'native_display_binding_mismatch', 'native_selection_write_failed', 'native_display_install_busy'];
export const hostFinishWinfspCodes = ['', 'confirmation_required', 'winfsp_already_installed', 'winfsp_package_mismatch', 'winfsp_busy', 'winfsp_blocked', 'winfsp_install_failed', 'winfsp_unavailable', 'winfsp_dll_missing', 'winfsp_load_failed',
  'winfsp_driver_missing', 'winfsp_path_invalid'];
export const hostFinishCodeText = { confirmation_required: t('labels.hostFinishCodeText.confirmation_required'), certificate_trust_required: t('labels.hostFinishCodeText.certificate_trust_required'), certificate_store_failed: t('labels.hostFinishCodeText.certificate_store_failed'),
  native_display_catalog_untrusted: t('labels.hostFinishCodeText.native_display_catalog_untrusted'), native_display_package_missing: t('labels.hostFinishCodeText.native_display_package_missing'), native_display_package_mismatch: t('labels.hostFinishCodeText.native_display_package_mismatch'),
  native_display_package_unprotected: t('labels.hostFinishCodeText.native_display_package_unprotected'), native_display_publisher_mismatch: t('labels.hostFinishCodeText.native_display_publisher_mismatch'), native_display_certificate_expired: t('labels.hostFinishCodeText.native_display_certificate_expired'),
  native_selection_invalid: t('labels.hostFinishCodeText.native_selection_invalid'), native_selected_device_unverified: t('labels.hostFinishCodeText.native_selected_device_unverified'), pnp_inf_invalid: t('labels.hostFinishCodeText.pnp_inf_invalid'), pnp_unavailable: t('labels.hostFinishCodeText.pnp_unavailable'),
  pnp_create_failed: t('labels.hostFinishCodeText.pnp_create_failed'), pnp_driver_unavailable: t('labels.hostFinishCodeText.pnp_driver_unavailable'), pnp_register_failed: t('labels.hostFinishCodeText.pnp_register_failed'), pnp_install_failed: t('labels.hostFinishCodeText.pnp_install_failed'),
  pnp_instance_invalid: t('labels.hostFinishCodeText.pnp_instance_invalid'), native_display_binding_mismatch: t('labels.hostFinishCodeText.native_display_binding_mismatch'), native_selection_write_failed: t('labels.hostFinishCodeText.native_selection_write_failed'), native_display_install_busy: t('labels.hostFinishCodeText.native_display_install_busy'),
  winfsp_already_installed: t('labels.hostFinishCodeText.winfsp_already_installed'), winfsp_package_mismatch: t('labels.hostFinishCodeText.winfsp_package_mismatch'), winfsp_busy: t('labels.hostFinishCodeText.winfsp_busy'), winfsp_blocked: t('labels.hostFinishCodeText.winfsp_blocked'), winfsp_install_failed: t('labels.hostFinishCodeText.winfsp_install_failed'),
  winfsp_unavailable: t('labels.hostFinishCodeText.winfsp_unavailable'), winfsp_dll_missing: t('labels.hostFinishCodeText.winfsp_dll_missing'), winfsp_load_failed: t('labels.hostFinishCodeText.winfsp_load_failed'), winfsp_driver_missing: t('labels.hostFinishCodeText.winfsp_driver_missing'), winfsp_path_invalid: t('labels.hostFinishCodeText.winfsp_path_invalid') };
// hostAction 285 (apps/windows_host/src/host_removal.hpp): what removing the Host cleaned up besides its files. error = why
// nothing was cleaned (reasonNumber); items = device nodes removed, missing = nodes that were already gone, pending = a node
// is still there; dropped = driver packages removed, expected = packages kept; changed = certificate entries were removed,
// cause = certificateNumber; owned = the selection and display records were deleted. Both tables are append only.
export const hostRemovalReasons = ['', 'arguments', 'not_removal', 'upgrade', 'product', 'installer_running', 'service_running', 'display_restore_pending'];
export const hostRemovalReasonText = { arguments: t('labels.hostRemovalReasonText.arguments'), not_removal: t('labels.hostRemovalReasonText.not_removal'), upgrade: t('labels.hostRemovalReasonText.upgrade'), product: t('labels.hostRemovalReasonText.product'),
  installer_running: t('labels.hostRemovalReasonText.installer_running'), service_running: t('labels.hostRemovalReasonText.service_running'), display_restore_pending: t('labels.hostRemovalReasonText.display_restore_pending') };
export const hostRemovalCertificates = ['removed', 'none', 'needed', 'nodes', 'unknown', 'failed'];
export const hostRemovalCertificateText = { removed: t('labels.hostRemovalCertificateText.removed'), none: t('labels.hostRemovalCertificateText.none'), needed: t('labels.hostRemovalCertificateText.needed'),
  nodes: t('labels.hostRemovalCertificateText.nodes'), unknown: t('labels.hostRemovalCertificateText.unknown'), failed: t('labels.hostRemovalCertificateText.failed') };
export const hostUsbReasons = ['usb_class_denied', 'usb_hid_boot_denied', 'usb_isochronous_denied', 'usb_descriptor_invalid', 'usb_class_not_granted', 'usb_vhci_unavailable', 'usb_limit_reached', 'usb_attach_failed', 'usb_busy', 'usb_user_stopped', 'usb_device_removed', 'usb_vm_detached', 'usb_policy_changed', 'usb_device_error', 'usb_session_closed', 'usb_driver_missing', 'usb_driver_install_failed', 'usb_vhci_unprotected', 'usb_setup_failed', 'usb_restart_required'];
const usbRequestNames = { '80,6': 'GET_DESCRIPTOR', '00,9': 'SET_CONFIGURATION', '01,11': 'SET_INTERFACE', '02,1': 'CLEAR_FEATURE(HALT)', '00,1': 'CLEAR_FEATURE', '00,3': 'SET_FEATURE', '80,0': 'GET_STATUS', '81,0': 'GET_STATUS', '82,0': 'GET_STATUS', '80,8': 'GET_CONFIGURATION', '81,10': 'GET_INTERFACE', 'a1,254': 'GET_MAX_LUN', '21,255': t('labels.62') };
const usbErrnoText = { 5: t('labels.usbErrnoText.5'), 19: t('labels.usbErrnoText.19'), 32: t('labels.usbErrnoText.32'), 62: t('labels.usbErrnoText.62'), 71: t('labels.usbErrnoText.71'), 75: t('labels.usbErrnoText.75'), 104: t('labels.usbErrnoText.104'), 108: t('labels.usbErrnoText.108'), 110: t('labels.usbErrnoText.110') };
// A request class from the diagnostics: bmRequestType<<8 | bRequest for a control transfer, 0x10000 | type<<8 | endpoint address otherwise.
export function usbRequestClassText(cls) {
  if (!Number.isInteger(cls) || cls < 0) return '';
  if (cls >= 0x10000) { const endpoint = cls & 255, type = (cls >> 8) & 255; return t('labels.65', { value: type === 3 ? t('labels.63') : t('labels.64'), value2: endpoint & 0x80 ? 'IN' : 'OUT', value3: endpoint & 15 }); }
  const bm = (cls >> 8) & 255, request = cls & 255, name = own(usbRequestNames, `${bm.toString(16).padStart(2, '0')},${request}`);
  return t('labels.66', { toString: bm.toString(16).toUpperCase().padStart(2, '0'), toString2: request.toString(16).toUpperCase().padStart(2, '0'), value: name ? ` (${name})` : '' });
}
export const usbErrnoLabel = errno => own(usbErrnoText, errno) ?? `errno ${errno}`;
// Client `usb` events (apps/windows_client/lib/data/usb_redirection.dart): stage.
export const clientUsbText = { 1: t('labels.clientUsbText.1'), 2: t('labels.clientUsbText.2'), 3: t('labels.clientUsbText.3'), 4: t('labels.clientUsbText.4'), 5: t('labels.clientUsbText.5'), 6: t('labels.clientUsbText.6'), 7: t('labels.clientUsbText.7'), 8: t('labels.clientUsbText.8'), 9: t('labels.clientUsbText.9'), 10: t('labels.clientUsbText.10'), 11: t('labels.clientUsbText.11'), 12: t('labels.clientUsbText.12'), 13: t('labels.clientUsbText.13'), 14: t('labels.clientUsbText.14'), 15: t('labels.clientUsbText.15') };
Object.assign(clientCodes, { usb_prepare_timeout: t('labels.67'), usb_return_unknown: t('labels.68') });
export const clientUsbBack = { 0: t('labels.clientUsbBack.0'), 1: t('labels.clientUsbBack.1'), 2: t('labels.clientUsbBack.2') };
export const hostEvents = { installerStart: t('labels.hostEvents.installerStart'), installerStage: t('labels.hostEvents.installerStage'), installerResult: t('labels.hostEvents.installerResult'), displayObserved: t('labels.hostEvents.displayObserved'), driverPreserved: t('labels.hostEvents.driverPreserved'), driverInstall: t('labels.hostEvents.driverInstall'), displayConfigure: t('labels.hostEvents.displayConfigure'), modeCatalog: t('labels.hostEvents.modeCatalog'), serviceState: t('labels.hostEvents.serviceState'), brokerState: t('labels.hostEvents.brokerState'), hostAction: t('labels.hostEvents.hostAction') };
// Field names of client/Host log values, shown as "이름 값".
export const valueNames = { ok: t('labels.valueNames.ok'), elapsedMs: t('labels.valueNames.elapsedMs'), monitors: t('labels.valueNames.monitors'), width: t('labels.valueNames.width'), height: t('labels.valueNames.height'), action: t('labels.valueNames.action'), stage: t('labels.valueNames.stage'), error: t('labels.valueNames.error'), active: t('labels.valueNames.active'), pending: t('labels.valueNames.pending'), recovery: t('labels.valueNames.recovery'), remote: t('labels.valueNames.remote'), running: t('labels.valueNames.running'), online: t('labels.valueNames.online'), changed: t('labels.valueNames.changed'), available: t('labels.valueNames.available'), auto: t('labels.valueNames.auto'), installed: t('labels.valueNames.installed'), started: t('labels.valueNames.started'), owned: t('labels.valueNames.owned'), reboot: t('labels.valueNames.reboot'), registered: t('labels.valueNames.registered'), restartAttempted: t('labels.valueNames.restartAttempted'), expected: t('labels.valueNames.expected'), missing: t('labels.valueNames.missing'), missingOwned: t('labels.valueNames.missingOwned'), fallback: t('labels.valueNames.fallback'), consoleExpected: t('labels.valueNames.consoleExpected'), consoleActive: t('labels.valueNames.consoleActive'), stale: t('labels.valueNames.stale'), remapped: t('labels.valueNames.remapped'), dropped: t('labels.valueNames.dropped'), format: t('labels.valueNames.format'), items: t('labels.valueNames.items'), kib: t('labels.valueNames.kib'), cause: t('labels.valueNames.cause'), usbDevices: t('labels.valueNames.usbDevices'), usbUrbs: t('labels.valueNames.usbUrbs'), usbKib: t('labels.valueNames.usbKib'), usbStalls: t('labels.valueNames.usbStalls'), usbFailed: t('labels.valueNames.usbFailed'), usbTimeouts: t('labels.valueNames.usbTimeouts'), usbSlow: t('labels.valueNames.usbSlow'), usbClass: t('labels.valueNames.usbClass'), usbReq: t('labels.valueNames.usbReq'), usbBack: t('labels.valueNames.usbBack') };
// Client update downloads: versions as logVersion numbers, the
// requests made and what was on disk when it ended; the fixed failure classes.
Object.assign(valueNames, { current: t('labels.69'), latest: t('labels.70'), attempts: t('labels.71'), receivedKib: t('labels.72') });
Object.assign(valueNames, { uptimeMs: t('labels.73') }, Object.fromEntries(hostBootStages.map(([key, label]) => [key, `${label}(ms)`])));
Object.assign(clientCodes, { download_network: t('labels.74'), download_timeout: t('labels.75'), download_server_error: t('labels.76'), download_not_found: t('labels.77'), download_http_status: t('labels.78'), download_disk_full: t('labels.79'), download_file_error: t('labels.80'), download_size_mismatch: t('labels.81'), download_failed: t('labels.82'), hash_mismatch: t('labels.83'), package_version_mismatch: t('labels.84'), installer_start_failed: t('labels.85'), invalid_catalog: t('labels.86') });

// Audit events (server/src/*.mjs store.audit calls).
export const eventNames = { user_created: t('labels.eventNames.user_created'), user_updated: t('labels.eventNames.user_updated'), user_deleted: t('labels.eventNames.user_deleted'), password_changed: t('labels.eventNames.password_changed'), user_enabled: t('labels.eventNames.user_enabled'), user_disabled: t('labels.eventNames.user_disabled'), desktop_created: t('labels.eventNames.desktop_created'), desktop_updated: t('labels.eventNames.desktop_updated'), desktop_deleted: t('labels.eventNames.desktop_deleted'), desktop_enabled: t('labels.eventNames.desktop_enabled'), desktop_disabled: t('labels.eventNames.desktop_disabled'), desktop_assigned: t('labels.eventNames.desktop_assigned'), assignment_revoked: t('labels.eventNames.assignment_revoked'), assignments_replaced: t('labels.eventNames.assignments_replaced'), integration_updated: t('labels.eventNames.integration_updated'), login_succeeded: t('labels.eventNames.login_succeeded'), login_failed: t('labels.eventNames.login_failed'), logout: t('labels.eventNames.logout'), admin_logout: t('labels.eventNames.admin_logout'), power_submitted: t('labels.eventNames.power_submitted'), power_failed: t('labels.eventNames.power_failed'), power_superseded: t('labels.eventNames.power_superseded'), clipboard_policy_changed: t('labels.eventNames.clipboard_policy_changed'), device_policy_changed: t('labels.eventNames.device_policy_changed'), rdp_redirection_policy_changed: t('labels.eventNames.device_policy_changed'), session_taken_over: t('labels.eventNames.session_taken_over'), session_cancelled: t('labels.eventNames.session_cancelled'),
  // server/src/pc-power.mjs (local PC power).
  pc_power_accepted: t('labels.eventNames.pc_power_accepted'), pc_power_refused: t('labels.eventNames.pc_power_refused'), pc_wake_sent: t('labels.eventNames.pc_wake_sent'), pc_wake_online: t('labels.eventNames.pc_wake_online'), pc_wake_timeout: t('labels.eventNames.pc_wake_timeout'),
  host_update_requested: t('labels.eventNames.host_update_requested'), host_update_completed: t('labels.eventNames.host_update_completed'), host_update_failed: t('labels.eventNames.host_update_failed'), host_update_rejected: t('labels.eventNames.host_update_rejected'), host_update_unknown: t('labels.eventNames.host_update_unknown'), host_update_expired: t('labels.eventNames.host_update_expired'),
  host_request_created: t('labels.eventNames.host_request_created'), host_request_cancelled: t('labels.eventNames.host_request_cancelled'), host_request_approved: t('labels.eventNames.host_request_approved'), host_request_rejected: t('labels.eventNames.host_request_rejected'), host_enrolled: t('labels.eventNames.host_enrolled'), host_enrollment_created: t('labels.eventNames.host_enrollment_created'),
  host_registration_removed: t('labels.eventNames.host_registration_removed'), registered_hosts_refreshed: t('labels.eventNames.registered_hosts_refreshed'),
  // server/src/store.mjs: assignments go with the Host they belonged to (another machine under the same VM number, the one-time cleanup;
  // a removal carries its count in host_registration_removed).
  desktop_assignments_removed: t('labels.eventNames.desktop_assignments_removed'), hostless_assignments_removed: t('labels.eventNames.hostless_assignments_removed'),
  // server/src/store.mjs relocateRegisteredVm: the desktop followed its VM to another node or VM ID (migration, HA, restore).
  desktop_vm_relocated: t('labels.eventNames.desktop_vm_relocated'),
  display_requested: t('labels.eventNames.display_requested'), display_completed: t('labels.eventNames.display_completed'), display_result_detail: t('labels.eventNames.display_result_detail'), console_resume_requested: t('labels.eventNames.console_resume_requested'), console_resume_completed: t('labels.eventNames.console_resume_completed'),
  server_update_checked: t('labels.eventNames.server_update_checked'), server_update_requested: t('labels.eventNames.server_update_requested'), server_restart_requested: t('labels.eventNames.server_restart_requested'), initial_setup_requested: t('labels.eventNames.initial_setup_requested'), initial_setup_completed: t('labels.eventNames.initial_setup_completed'), proxmox_synchronized: t('labels.eventNames.proxmox_synchronized'),
  // server/src/relay.mjs dropHost: a Host connection that ended sessions or ended for a known cause.
  host_channel_closed: t('labels.eventNames.host_channel_closed'),
  // server/src/relay.mjs hostMachineChanged: a copy of a registered desktop (full clone) refused (host-machine-v1).
  host_machine_changed: t('labels.eventNames.host_machine_changed'),
  // server/src/session-resume.mjs: a lost connection that came back, or did not in time.
  session_resumed: t('labels.eventNames.session_resumed'), session_resume_expired: t('labels.eventNames.session_resume_expired'),
  // The remaining audit events: every store.audit(...) name in server/src has a name in this table
  // (one without a name shows its raw code in 활동 기록).
  client_idle_logout: t('labels.eventNames.client_idle_logout'), client_login_replaced: t('labels.eventNames.client_login_replaced'),
  console_granted: t('labels.eventNames.console_granted'), console_ready: t('labels.eventNames.console_ready'), console_ended: t('labels.eventNames.console_ended'), console_logout: t('labels.eventNames.console_logout'), console_taken_over: t('labels.eventNames.console_taken_over'),
  desktop_permissions_replaced: t('labels.eventNames.desktop_permissions_replaced'), sound_card_add_permission_changed: t('labels.eventNames.sound_card_add_permission_changed'),
  host_identity_changed: t('labels.eventNames.host_identity_changed'), local_pc_approved: t('labels.eventNames.local_pc_approved'), power_refused: t('labels.eventNames.power_refused'),
  mfa_factor_changed: t('labels.eventNames.mfa_factor_changed'), mfa_root_reset: t('labels.eventNames.mfa_root_reset'),
  // server/src/mfa.mjs: failed codes (grouped, userId:count per minute) and blocks (userId:minutes, or userId:day for the daily cap).
  mfa_failed: t('labels.eventNames.mfa_failed'), mfa_locked: t('labels.eventNames.mfa_locked'),
  server_access_saved: t('labels.eventNames.server_access_saved'), server_access_tested: t('labels.eventNames.server_access_tested'),
  // server/src/http.mjs and admin-http.mjs: a save ends the connections (with their count); a waiting save was withdrawn.
  server_access_connections_ended: t('labels.eventNames.server_access_connections_ended'), server_access_cancelled: t('labels.eventNames.server_access_cancelled'),
  vm_audio_added: t('labels.eventNames.vm_audio_added'), vm_audio_unknown: t('labels.eventNames.vm_audio_unknown'),
  // server/src/certificate-expiry.mjs: the presented TLS certificate reached 14 or 3 days before expiry.
  tls_certificate_expiring: t('labels.eventNames.tls_certificate_expiring'),
  // server/src/backup.mjs and admin-http.mjs (백업·복원). backup_restore_connections_ended reaches audit() through the fence.
  backup_exported: t('labels.eventNames.backup_exported'), backup_downloaded: t('labels.eventNames.backup_downloaded'), backup_reauthentication_failed: t('labels.eventNames.backup_reauthentication_failed'), backup_inspected: t('labels.eventNames.backup_inspected'),
  backup_restore_requested: t('labels.eventNames.backup_restore_requested'), backup_restore_connections_ended: t('labels.eventNames.backup_restore_connections_ended'), backup_restore_finished: t('labels.eventNames.backup_restore_finished'), backup_restore_failed: t('labels.eventNames.backup_restore_failed'), backup_restored: t('labels.eventNames.backup_restored'),
  backup_restore_access_requested: t('labels.eventNames.backup_restore_access_requested'), backup_restore_access_same: t('labels.eventNames.backup_restore_access_same'), backup_restore_access_failed: t('labels.eventNames.backup_restore_access_failed'),
  // server/src/relay.mjs auditConsoleProof: desktopId:code (server/src/stream-console-profile.mjs).
  stream_console_unverified: t('labels.eventNames.stream_console_unverified'), stream_console_identity_refused: t('labels.eventNames.stream_console_identity_refused') };
export const eventLabel = value => own(eventNames, value) || value;
export const auditFailures = new Set(['mfa_failed', 'mfa_locked', 'backup_reauthentication_failed', 'backup_restore_failed', 'backup_restore_access_failed', 'login_failed', 'power_failed', 'pc_power_refused', 'pc_wake_timeout', 'host_update_failed', 'host_update_rejected', 'host_update_unknown', 'host_update_expired', 'host_request_rejected', 'host_channel_closed', 'host_machine_changed', 'session_resume_expired', 'stream_console_identity_refused']);
// Why a stream went on without the Proxmox display check, or was refused (the fixed codes above).
export const consoleProofCodeText = Object.fromEntries(['console_vga_other', 'console_gpu_primary', 'console_qemu_args', 'console_vga_default', 'console_config_other',
  'proxmox_not_configured', 'proxmox_unreachable', 'proxmox_timeout', 'proxmox_permission_denied', 'proxmox_certificate_error', 'proxmox_request_failed',
  'proxmox_invalid_response', 'proxmox_vm_not_found', 'proxmox_vm_not_running', 'proxmox_target_invalid', 'proxmox_settings_changed', 'proxmox_read_failed',
  'machine_uuid_unknown', 'host_machine_mismatch', 'desktop_changed', 'desktop_unavailable'].map(code => [code, t('labels.consoleProof.' + code)]));

// Proxmox tags and pool of a VM (read-only here: they are managed in Proxmox).
export const vmTags = desktop => Array.isArray(desktop?.pve_tags) ? desktop.pve_tags.filter(tag => typeof tag === 'string') : [];
export const vmPool = desktop => typeof desktop?.pve_pool === 'string' && desktop.pve_pool ? desktop.pve_pool : '';
export const tagsHint = t('labels.tagsHint');
// What the VM search matches: ID, name, node, OS, Proxmox tags and pool.
export const vmSearchText = desktop => [desktop.pve_vmid, desktop.name, desktop.pve_node ?? '', desktop.os ?? '', ...vmTags(desktop), vmPool(desktop)].join(' ').toLowerCase();

// Plain-language cause of a failed connection and who has to act. `who`:
// 'admin' (the operator has something to do; `fix` says what), 'user' (the
// user's side, nothing to do here) or 'wait' (it passes by itself). A code
// that is not listed has no advice: the record then says the cause is not
// explained yet and never suggests a fix.
const advice = (who, what, fix = null) => Object.freeze({ who, what, fix });
const pcSilent = advice('admin', t('labels.87'), t('labels.88'));
const pcDisplay = advice('admin', t('labels.89'), t('labels.90'));
const pcInUse = advice('user', t('labels.91'));
const signedOut = advice('user', t('labels.92'));
export const failureAdvice = Object.freeze({
  host_offline: pcSilent, host_unavailable: pcSilent, host_disconnected: pcSilent, host_timeout: pcSilent,
  no_interactive_session: advice('admin', t('labels.93'), t('labels.94')),
  virtual_display_required: pcDisplay, native_display_not_ready: pcDisplay, native_topology_recovery_required: pcDisplay, display_configuration_failed: pcDisplay,
  host_revoked: advice('admin', t('labels.95'), t('labels.96')),
  host_maintenance: advice('wait', t('labels.97')),
  server_restarting: advice('wait', t('labels.98')), server_stopping: advice('wait', t('labels.98')),
  session_cleanup_pending: advice('wait', t('labels.99')),
  network_error: advice('user', t('labels.100')),
  authentication_required: signedOut, session_expired: signedOut,
  desktop_busy: pcInUse, session_busy: pcInUse, controller_in_use: pcInUse, other_user: pcInUse,
  taken_over: advice('user', t('labels.101')),
  operation_cancelled: advice('user', t('labels.102')),
  authorization_revoked: advice('wait', t('labels.103')),
  login_failed: advice('user', t('labels.104')),
});

// ---- 공지 보내기 (js/notices.js, server/src/notices.mjs). Kept in its own block: the tables above are edited
// elsewhere. The refusals the server answers a send or a withdrawal with, and the two administration records.
Object.assign(messages, {
  invalid_notice_title: t('labels.109'),
  invalid_notice_body: t('labels.110'),
  invalid_notice_text: t('labels.111'),
  invalid_notice_target: t('labels.112'),
  invalid_notice_schedule: t('labels.113'),
  invalid_notice_kind: t('labels.114'),
  notice_no_recipients: t('labels.115'),
  too_many_notices: t('labels.116'),
  notice_not_found: t('labels.117'),
  notice_already_ended: t('labels.118'),
  notices_unavailable: t('labels.119'),
});
messages.desktop_not_found ??= t('labels.120');
Object.assign(eventNames, { notice_sent: t('labels.121'), notice_withdrawn: t('labels.122') });
