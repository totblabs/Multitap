import 'host_devices.dart';
import '../l10n/l10n.dart';

String hostDiagnosticsText(Map<String, dynamic> state) {
  Map<String, dynamic> section(String key) =>
      Map<String, dynamic>.from(state[key] as Map? ?? {});
  String value(Object? raw) => raw == null
      ? l10n.noInfo
      : raw
            .toString()
            .replaceAll(RegExp(r'[\r\n\t]'), ' ')
            .substring(0, raw.toString().length.clamp(0, 240));
  String status(Object? raw, String yes, String no) => raw == true
      ? yes
      : raw == false
      ? no
      : l10n.noInfo;
  final service = section('service'),
      broker = section('broker'),
      display = section('display'),
      account = section('windowsAccount');
  const brokerStates = {
    'waiting_registration',
    'connecting',
    'awaiting_ack',
    'online',
    'reconnecting',
    'stale',
    'stopped',
    'unknown',
  };
  const brokerReasons = {
    '',
    'unregistered',
    'credentials_unavailable',
    'network_unavailable',
    'tls_failed',
    'authorization_failed',
    'server_unavailable',
    'protocol_rejected',
    'transport_closed',
    'send_failed',
    'heartbeat_timeout',
    'clock_changed',
    'status_unavailable',
    'service_stopped',
    'service_unknown',
  };
  final ackAge = broker['heartbeatAgeSeconds'];
  final validAge = ackAge is num && ackAge.isFinite && ackAge >= 0;
  final verifiedOnline =
      state['registered'] == true &&
      service['running'] == true &&
      broker['state'] == 'online' &&
      broker['online'] == true &&
      validAge &&
      ackAge <= 30;
  // Explicit allowlist: enrollment codes, tokens and arbitrary status fields never enter the clipboard.
  return [
    l10n.multitapHostDiagnostics,
    l10n.hostVersion(value(state['version'])),
    l10n.desktop2(value(state['desktopId'])),
    l10n.server2(value(state['serverOrigin'])),
    l10n.serverRegistration(status(state['registered'], l10n.registered, l10n.notRegistered)),
    l10n.serviceInstall(status(service['installed'], l10n.installed, l10n.notInstalled)),
    l10n.serviceState(status(service['running'], l10n.running, l10n.stopped)),
    if (service['queryError'] is num && service['queryError'] != 0)
      l10n.serviceQueryError(value(service['queryError'])),
    l10n.serverConnection2(verifiedOnline ? l10n.serverResponded : l10n.needsCheck),
    l10n.serverConnectionStage(brokerStates.contains(broker['state']) ? broker['state'] : 'unknown'),
    l10n.sinceLastServerResponse(validAge ? l10n.s(ackAge.toStringAsFixed(1)) : l10n.noInfo),
    if (brokerReasons.contains(broker['failureReason']) &&
        broker['failureReason'] != '')
      l10n.serverConnectionFailureReason(broker['failureReason']),
    l10n.windowsSign(status(state['interactiveSession'], l10n.yes, l10n.needsCheck)),
    l10n.virtualDisplayDriver(status(display['installed'], l10n.installed, l10n.notInstalled)),
    l10n.virtualDisplayDevice(status(display['deviceReady'], l10n.started, l10n.needsCheck)),
    l10n.virtualDisplay(status(display['ready'], l10n.ready, l10n.needsCheck)),
    for (final m in display['monitors'] as List? ?? [])
      if (m is Map)
        l10n.virtualMonitor(value(m['id']), value(m['width']), value(m['height'])),
    l10n.displayRecovery(status(display['recoveryRequired'], l10n.needed, l10n.notNeeded)),
    l10n.windowsRestart(status(display['rebootRequired'], l10n.needed, l10n.notRequested2)),
    // Whether an account is stored, never its name or password.
    l10n.windowsAccount(status(account['configured'], l10n.saved, l10n.none)),
    // devices-v1: the fixed row texts, never the reply's own strings.
    for (final row in hostDeviceRows(state['devices']))
      '${row.title}: ${[row.level.label, if (row.status.isNotEmpty) row.status].join(' · ')}',
  ].join('\n');
}
