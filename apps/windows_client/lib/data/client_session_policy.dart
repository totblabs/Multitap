import '../l10n/l10n.dart';

/// Additive login protocol for cooperating Windows peers, not device identity.
const pcSessionPolicyHeader = 'X-Multitap-Session-Policy';
const pcLoginIntentHeader = 'X-Multitap-Login-Intent';
const windowsPcSessionPolicy = 'windows-pc-v1';

enum PcLoginIntent {
  interactive('interactive-v1'),
  confirmable('confirmable-v1'),
  recovery('recovery-v1');

  const PcLoginIntent(this.wire);
  final String wire;
}

String? pcLoginTerminalCode(String code, int status) =>
    ((code == 'pc_login_replaced' || code == 'session_idle_timeout') &&
            status == 401) ||
        ((code == 'pc_login_requires_interaction' ||
                code == 'session_reauthentication_required') &&
            status == 409)
    ? code
    : null;

String pcLoginTerminalMessage(String code) => switch (code) {
  'pc_login_replaced' => l10n.youSignedSomewhereElseSo,
  'session_idle_timeout' => l10n.youWereSignedOutAfter,
  'pc_login_requires_interaction' => l10n.useAnotherDeviceSignAgain,
  'session_reauthentication_required' => l10n.youWereSignedOutEnter,
  _ => l10n.signHasExpiredSignAgain,
};
