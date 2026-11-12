// Actual mfa-v1 DTOs only. Ownership/revision are local controller state,
// never invented fields in the server's status response.
bool _shape(Object? value, Set<String> keys) => value is Map &&
    value.length == keys.length && keys.every(value.containsKey);
Never _invalid() => throw const FormatException('Invalid MFA response.');

class MfaAccountStatus {
  const MfaAccountStatus({required this.enabled, required this.recoveryCodesRemaining});
  final bool enabled;
  final int recoveryCodesRemaining;
  factory MfaAccountStatus.parse(Object? value) {
    if (!_shape(value, {'protocol', 'enabled', 'recoveryCodesRemaining'})) { _invalid(); }
    final map = value as Map, count = map['recoveryCodesRemaining'];
    if (map['protocol'] != 'mfa-v1' || map['enabled'] is! bool ||
        count is! int || count < 0 || count > 10 || (map['enabled'] == false && count != 0)) { _invalid(); }
    return MfaAccountStatus(enabled: map['enabled'] as bool, recoveryCodesRemaining: count);
  }
}

class MfaEnrollment {
  MfaEnrollment._(this._token, this._secret, this._uri, this.expiresAt, this._received)
      : _age = Stopwatch()..start();
  String? _token, _secret;
  Uri? _uri;
  final DateTime expiresAt, _received;
  final Stopwatch _age;
  String get token => _token ?? '';
  String get secret => _secret ?? '';
  Uri? get uri => _uri;
  Duration get remaining {
    if (_token == null) { return Duration.zero; }
    final initial = Duration(milliseconds: expiresAt.difference(_received).inMilliseconds.clamp(0, 120000).toInt());
    final monotonic = initial - _age.elapsed, wall = expiresAt.difference(DateTime.now());
    final left = monotonic < wall ? monotonic : wall;
    return left.isNegative ? Duration.zero : left;
  }
  factory MfaEnrollment.parse(Object? value, {DateTime? receivedAt}) {
    if (!_shape(value, {'protocol', 'setupToken', 'expiresAt', 'secret', 'uri'})) { _invalid(); }
    final map = value as Map;
    final received = receivedAt ?? DateTime.now();
    final expiry = map['expiresAt'] is String ? DateTime.tryParse(map['expiresAt'] as String) : null;
    final uri = map['uri'] is String ? Uri.tryParse(map['uri'] as String) : null;
    if (map['protocol'] != 'mfa-v1' || map['setupToken'] is! String ||
        !RegExp(r'^[A-Za-z0-9_-]{43}$').hasMatch(map['setupToken'] as String) ||
        map['secret'] is! String || !RegExp(r'^[A-Z2-7]{32}$').hasMatch(map['secret'] as String) ||
        expiry == null || !expiry.isAfter(received) ||
        uri == null || uri.scheme != 'otpauth' || uri.host != 'totp' || uri.userInfo.isNotEmpty || uri.hasFragment ||
        uri.queryParametersAll.length != 5 || uri.queryParametersAll.values.any((values) => values.length != 1) ||
        uri.queryParameters['secret'] != map['secret'] || uri.queryParameters['issuer'] != 'Multitap' ||
        uri.queryParameters['algorithm'] != 'SHA1' || uri.queryParameters['digits'] != '6' || uri.queryParameters['period'] != '30') { _invalid(); }
    return MfaEnrollment._(map['setupToken'] as String, map['secret'] as String, uri, expiry, received);
  }
  void forget() { _token = null; _secret = null; _uri = null; _age.stop(); }
  @override
  String toString() => 'Pending MFA enrollment';
}

enum MfaAccountAction { enable, disable, regenerate }

class MfaAccountChange {
  MfaAccountChange._(this.action, this.enabled, List<String> codes) : _codes = codes;
  final MfaAccountAction action;
  final bool enabled;
  bool get requiresLogin => true; // Factory admits ONLY actual requiresLogin:true.
  List<String> _codes;
  List<String> takeCodes() { final codes = _codes; _codes = []; return codes; }
  void forget() => _codes = [];
  factory MfaAccountChange.parse(Object? value, MfaAccountAction action) {
    final withCodes = action != MfaAccountAction.disable;
    if (!_shape(value, {'enabled', 'requiresLogin', if (withCodes) 'recoveryCodes'})) { _invalid(); }
    final map = value as Map;
    if (map['requiresLogin'] != true || map['enabled'] != withCodes) { _invalid(); }
    final codes = <String>[];
    if (withCodes) {
      final list = map['recoveryCodes'];
      if (list is! List || list.length != 10 || list.any((code) => code is! String || !RegExp(r'^[a-f0-9]{32}$').hasMatch(code))) { _invalid(); }
      codes.addAll(list.cast<String>());
      if (codes.toSet().length != 10) { _invalid(); }
    }
    return MfaAccountChange._(action, withCodes, codes);
  }
  @override
  String toString() => 'Acknowledged MFA account change';
}

abstract interface class MfaAccountBrokerApi {
  Uri get origin;
  Future<MfaAccountStatus> readMfaAccount(String token);
  Future<MfaEnrollment> setupMfaAccount(String token, String currentPassword);
  Future<MfaAccountChange> enableMfaAccount(String token, MfaEnrollment enrollment, String code);
  Future<void> reauthenticateMfaAccount(String token, {required String currentPassword, String? code, String? recoveryCode});
  Future<MfaAccountChange> disableMfaAccount(String token, String currentPassword);
  Future<MfaAccountChange> regenerateMfaRecovery(String token, String currentPassword);
}
