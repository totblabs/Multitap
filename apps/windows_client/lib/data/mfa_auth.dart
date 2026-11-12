import 'client_session_policy.dart';
import '../l10n/l10n.dart';

export 'mfa_recovery.dart' show normalizeMfaRecoveryCode;

/// A negotiated, short-lived login challenge. Never persisted or logged.
class MfaChallenge {
  MfaChallenge._(
    this.origin,
    String token,
    this.expiresAt,
    this.methods,
    this.pcLoginIntent,
  ) : _token = token,
      _received = DateTime.now(),
      _age = Stopwatch()..start();

  final Uri origin;
  String? _token;
  String get token => _token ?? '';
  final DateTime expiresAt;
  final Set<String> methods;

  /// Original local request context; the server must bind its own challenge.
  final PcLoginIntent? pcLoginIntent;
  final DateTime _received;
  final Stopwatch _age;

  factory MfaChallenge.fromJson(
    Object? value, {
    required Uri origin,
    PcLoginIntent? pcLoginIntent,
  }) {
    if (origin.scheme != 'https' || value is! Map) {
      throw const FormatException('Invalid MFA challenge');
    }
    final token = value['challengeToken'], expiry = value['expiresAt'];
    final methods = value['methods'];
    final expiresAt = expiry is String ? DateTime.tryParse(expiry) : null;
    if (value['protocol'] != 'mfa-v1' ||
        token is! String ||
        !RegExp(r'^[A-Za-z0-9_-]{43}$').hasMatch(token) ||
        expiresAt == null ||
        methods is! List ||
        methods.isEmpty ||
        methods.length > 8 ||
        methods.any((v) => v is! String)) {
      throw const FormatException('Invalid MFA challenge');
    }
    final supported = methods
        .whereType<String>()
        .where((v) => v == 'totp' || v == 'recovery')
        .toSet();
    if (supported.isEmpty) {
      throw const FormatException('Unsupported MFA method');
    }
    return MfaChallenge._(
      origin,
      token,
      expiresAt.toUtc(),
      Set.unmodifiable(supported),
      pcLoginIntent,
    );
  }

  /// The server is authoritative. Local wall-clock rollback cannot extend120s.
  Duration get remaining {
    if (_token == null) return Duration.zero;
    final absolute = expiresAt.difference(DateTime.now());
    final initial = expiresAt
        .difference(_received)
        .inMilliseconds
        .clamp(0, 120000);
    final bounded = Duration(milliseconds: initial) - _age.elapsed;
    final value = absolute < bounded ? absolute : bounded;
    return value.isNegative ? Duration.zero : value;
  }

  void forget() {
    _token = null;
    _age.stop();
  }

  @override
  String toString() => 'MFA login challenge';
}

class MfaRequiredException implements Exception {
  const MfaRequiredException(this.challenge);
  final MfaChallenge challenge;
  @override
  String toString() => l10n.twoStepVerificationRequired;
}
