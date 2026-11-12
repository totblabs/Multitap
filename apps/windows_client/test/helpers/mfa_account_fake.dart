import 'package:multitap_client/data/mfa_account.dart';
import 'package:multitap_client/data/mfa_account_controller.dart';

final mfaCodes = List.generate(10, (index) => index.toRadixString(16).padLeft(32, '0'));
MfaAccountChange mfaChange([MfaAccountAction action = MfaAccountAction.enable]) => MfaAccountChange.parse({
  'enabled': action != MfaAccountAction.disable, 'requiresLogin': true,
  if (action != MfaAccountAction.disable) 'recoveryCodes': mfaCodes,
}, action);
MfaEnrollment mfaEnrollment() {
  const secret = 'JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP';
  return MfaEnrollment.parse({'protocol': 'mfa-v1', 'setupToken': 'a' * 43,
    'expiresAt': DateTime.now().add(const Duration(minutes: 2)).toUtc().toIso8601String(),
    'secret': secret, 'uri': 'otpauth://totp/Multitap:synthetic?secret=$secret&issuer=Multitap&algorithm=SHA1&digits=6&period=30'});
}
class MfaTestApi implements MfaAccountBrokerApi {
  @override
  final origin = Uri.parse('https://synthetic.invalid');
  final calls = <String>[];
  MfaAccountStatus status = const MfaAccountStatus(enabled: false, recoveryCodesRemaining: 0);
  Future<MfaAccountStatus> Function()? read;
  Future<MfaEnrollment> Function()? setup;
  Future<void> Function()? reauth;
  Future<MfaAccountChange> Function(MfaAccountAction action)? change;
  @override
  Future<MfaAccountStatus> readMfaAccount(String token) async { calls.add('status'); return read == null ? status : await read!(); }
  @override
  Future<MfaEnrollment> setupMfaAccount(String token, String currentPassword) async { calls.add('setup'); return setup == null ? mfaEnrollment() : await setup!(); }
  @override
  Future<void> reauthenticateMfaAccount(String token, {required String currentPassword, String? code, String? recoveryCode}) async {
    calls.add('reauth'); await reauth?.call();
  }
  Future<MfaAccountChange> _change(MfaAccountAction action) async {
    calls.add(action.name); return change == null ? mfaChange(action) : await change!(action);
  }
  @override
  Future<MfaAccountChange> enableMfaAccount(String token, MfaEnrollment enrollment, String code) => _change(MfaAccountAction.enable);
  @override
  Future<MfaAccountChange> disableMfaAccount(String token, String currentPassword) => _change(MfaAccountAction.disable);
  @override
  Future<MfaAccountChange> regenerateMfaRecovery(String token, String currentPassword) => _change(MfaAccountAction.regenerate);
}
class MfaTestOwner {
  final api = MfaTestApi(), inbox = MfaCompletionInbox();
  bool presentation = true, authenticated = true;
  int authRejections = 0;
  late final owner = MfaCompletionOwner(origin: api.origin, accountId: '11111111-1111-4111-8111-111111111111',
      username: 'synthetic', generation: 1, current: () => presentation);
  late final controller = MfaAccountController(api: api, owner: owner, token: 'synthetic-only',
      currentAuthentication: () => authenticated, completed: inbox, onAuthenticationRejected: (_) => authRejections++);
  Future<void> enrollment() async {
    await controller.refresh(); controller.choose(MfaAccountAction.enable);
    await controller.submitProof('synthetic-password');
  }
  void dispose() { controller.dispose(); inbox.dispose(); }
}
