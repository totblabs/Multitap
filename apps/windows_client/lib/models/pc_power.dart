import '../l10n/l10n.dart';

/// Power of an approved physical PC (pc-power-v1, broker pc-power.mjs):
/// 종료 and 다시 시작 through the PC's
/// Host, 켜기 by Wake-on-LAN from the server. No 정지 for a PC.
const pcPowerProtocol = 'pc-power-v1';

class PcPowerState {
  const PcPowerState({
    this.supported = false,
    this.actions = const {},
    this.wake = false,
  });

  /// A broker before pc-power-v1, or a VM: no PC power at all.
  static const absent = PcPowerState();

  /// The broker offers PC power for this desktop.
  final bool supported;

  /// What may be asked now: 'shutdown' and 'reboot' while the Host is
  /// online and announced them (none from a Host before pc-power-v1),
  /// 'start' while the PC is offline and the server knows its wake address.
  final Set<String> actions;

  /// The server knows a wake address of this PC (never the address itself).
  final bool wake;

  /// Lenient like the other optional list fields: anything unexpected is
  /// [absent], so the PC simply shows no power actions.
  factory PcPowerState.fromJson(Object? data) {
    if (data is! Map ||
        data['protocol'] != pcPowerProtocol ||
        data['actions'] is! List ||
        data['wake'] is! bool) {
      return absent;
    }
    return PcPowerState(
      supported: true,
      actions: {
        for (final action in data['actions'] as List)
          if (const {'start', 'shutdown', 'reboot'}.contains(action))
            action as String,
      },
      wake: data['wake'] as bool,
    );
  }
}

/// The fixed results a PC power task may end with besides OK (the PC
/// Host's refusals and the server's wake results); anything else is the
/// generic power failure.
const pcPowerTaskCodes = {
  'invalid_request',
  'shutdown_in_progress',
  'privilege_unavailable',
  'other_users_signed_in',
  'machine_locked',
  'shutdown_failed',
  'wake_timeout',
  'wake_send_failed',
};

/// Why 켜기 did not bring the PC up, as the one short line of the help
/// dialog; null for any other code (the generic power message applies).
String? pcWakeFailure(String? code) => switch (code) {
  'pc_wake_unavailable' => l10n.serverHasntReceivedLocalPcs,
  'wake_timeout' => l10n.wakeSignalWasSentBut,
  'wake_send_failed' => l10n.serverCouldntSendWakeSignal,
  _ => null,
};

/// What Wake-on-LAN needs, behind the help dialog's 자세히.
List<String> get pcWakeRequirements => [
  l10n.localPcMustConnectedBy,
  l10n.wakeLanMustBiosUefi,
  l10n.deviceManagerNetworkAdapterPower,
  l10n.localPcMustSameNetwork,
  l10n.installLatestHostLocalPc,
];
