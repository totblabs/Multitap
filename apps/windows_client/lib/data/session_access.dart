const sessionAccessProtocol = 'session-access-v1';

/// Accepted server policy. No locally invented deadline or device identity.
class SessionAccess {
  const SessionAccess({
    required this.idleTimeout,
    required this.warning,
    required this.idleDeadline,
    required this.serverTime,
    this.idleEnabled = true,
    this.responseElapsed = Duration.zero,
    this.age,
  });
  final Duration idleTimeout, warning;
  final DateTime? idleDeadline;
  final DateTime serverTime;
  final bool idleEnabled;
  final Duration responseElapsed;
  final Duration Function()? age;
  Duration get elapsed => responseElapsed + (age?.call() ?? Duration.zero);
  Duration? get remaining {
    if (!idleEnabled) return null;
    final value = idleDeadline!.difference(serverTime) - elapsed;
    return value < Duration.zero ? Duration.zero : value;
  }

  static SessionAccess parse(
    Object? value, {
    Duration responseElapsed = Duration.zero,
  }) {
    if (value is! Map ||
        value.length != 7 ||
        value.keys.any(
          (key) => !const {
            'protocol',
            'serverTime',
            'idleDeadline',
            'warningAt',
            'idleTimeoutMs',
            'warningMs',
            'idleEnabled',
          }.contains(key),
        ) ||
        value['protocol'] != sessionAccessProtocol ||
        value['idleTimeoutMs'] is! int ||
        value['warningMs'] is! int ||
        value['idleEnabled'] is! bool ||
        value['serverTime'] is! String ||
        (value['idleEnabled'] == true
            ? (value['idleDeadline'] is! String ||
                  value['warningAt'] is! String)
            : (value['idleDeadline'] != null || value['warningAt'] != null))) {
      throw const FormatException();
    }
    final timeout = value['idleTimeoutMs'] as int;
    final warning = value['warningMs'] as int;
    final enabled = value['idleEnabled'] as bool;
    final raw = value['idleDeadline'] as String?,
        warningRaw = value['warningAt'] as String?;
    final deadline = raw == null ? null : DateTime.tryParse(raw);
    final serverRaw = value['serverTime'] as String;
    final server = DateTime.tryParse(serverRaw);
    final warningAt = warningRaw == null ? null : DateTime.tryParse(warningRaw);
    if (timeout < 1000 ||
        timeout > 86400000 ||
        warning < 0 ||
        warning >= timeout ||
        server == null ||
        serverRaw.length > 40 ||
        !serverRaw.endsWith('Z') ||
        (enabled &&
            (deadline == null ||
                warningAt == null ||
                raw!.length > 40 ||
                warningRaw!.length > 40 ||
                !raw.endsWith('Z') ||
                !warningRaw.endsWith('Z') ||
                deadline.difference(server) <= Duration.zero ||
                deadline.difference(server) > Duration(milliseconds: timeout) ||
                warningAt !=
                    deadline.subtract(Duration(milliseconds: warning)))) ||
        responseElapsed < Duration.zero ||
        responseElapsed > const Duration(days: 1)) {
      throw const FormatException();
    }
    final watch = Stopwatch()..start();
    return SessionAccess(
      idleTimeout: Duration(milliseconds: timeout),
      warning: Duration(milliseconds: warning),
      idleDeadline: deadline?.toUtc(),
      idleEnabled: enabled,
      serverTime: server.toUtc(),
      responseElapsed: responseElapsed,
      age: () => watch.elapsed,
    );
  }

  Map<String, Object?> toJson() => {
    'protocol': sessionAccessProtocol,
    'idleTimeoutMs': idleTimeout.inMilliseconds,
    'warningMs': warning.inMilliseconds,
    'idleEnabled': idleEnabled,
    'idleDeadline': idleDeadline?.toUtc().toIso8601String(),
    'serverTime': serverTime.toUtc().toIso8601String(),
    'warningAt': idleDeadline?.subtract(warning).toUtc().toIso8601String(),
  };
}

/// Authenticated display context, never authority to replace a session.
class LoginContextDesktop {
  const LoginContextDesktop._(this.id, this.name);
  final String id, name;

  static LoginContextDesktop parse(Object? value) {
    if (value is! Map || !_loginContextKeys(value, const {'id', 'name'})) {
      throw const FormatException();
    }
    final id = value['id'];
    if (id is! String ||
        !RegExp(r'^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$').hasMatch(id)) {
      throw const FormatException();
    }
    return LoginContextDesktop._(id, _loginContextText(value['name'], 128));
  }
}

class LoginContextSession {
  const LoginContextSession._(this.issuer, this.createdAt, this.desktops);

  /// A server protocol label; it does not identify the OS or physical device.
  final String issuer;
  final DateTime createdAt;
  final List<LoginContextDesktop> desktops;

  static LoginContextSession parse(Object? value) {
    if (value is! Map ||
        !_loginContextKeys(value, const {'issuer', 'createdAt', 'desktops'})) {
      throw const FormatException();
    }
    final issuer = value['issuer'],
        raw = value['createdAt'],
        desktops = value['desktops'];
    if (issuer is! String ||
        !RegExp(r'^[A-Za-z0-9_-]{1,64}$').hasMatch(issuer) ||
        raw is! String ||
        raw.length > 40 ||
        !raw.endsWith('Z') ||
        desktops is! List ||
        desktops.length > 5) {
      throw const FormatException();
    }
    final createdAt = DateTime.tryParse(raw);
    // Canonical UTC also rejects DateTime.parse's overflowing-day normalization.
    if (createdAt == null ||
        !createdAt.isUtc ||
        createdAt.toIso8601String() != raw) {
      throw const FormatException();
    }
    return LoginContextSession._(
      issuer,
      createdAt,
      List.unmodifiable(desktops.map(LoginContextDesktop.parse)),
    );
  }
}

class ExistingLoginContext {
  const ExistingLoginContext._(this.username, this.sessionCount, this.sessions);
  final String username;
  final int sessionCount;
  final List<LoginContextSession> sessions;

  static ExistingLoginContext parse(Object? value) {
    if (value is! Map ||
        !_loginContextKeys(value, const {
          'username',
          'sessionCount',
          'sessions',
        })) {
      throw const FormatException();
    }
    final count = value['sessionCount'], sessions = value['sessions'];
    if (count is! int ||
        count < 1 ||
        count > 5 ||
        sessions is! List ||
        sessions.length != count) {
      throw const FormatException();
    }
    final parsed = sessions
        .map(LoginContextSession.parse)
        .toList(growable: false);
    if (parsed.fold<int>(
          0,
          (total, session) => total + session.desktops.length,
        ) >
        5) {
      throw const FormatException();
    }
    return ExistingLoginContext._(
      _loginContextText(value['username'], 64),
      count,
      List.unmodifiable(parsed),
    );
  }
}

bool _loginContextKeys(Map value, Set<String> expected) =>
    value.length == expected.length && value.keys.every(expected.contains);

String _loginContextText(Object? value, int maxLength) {
  if (value is! String ||
      value.trim().isEmpty ||
      value.runes.length > maxLength ||
      RegExp(
        r'[\x00-\x1f\x7f-\x9f\u061c\u200e\u200f\u2028\u2029\u202a-\u202e\u2066-\u2069]',
      ).hasMatch(value)) {
    throw const FormatException();
  }
  final units = value.codeUnits;
  for (var index = 0; index < units.length; index++) {
    final unit = units[index];
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++index >= units.length ||
          units[index] < 0xdc00 ||
          units[index] > 0xdfff) {
        throw const FormatException();
      }
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      throw const FormatException();
    }
  }
  return value;
}

/// In-memory, origin-bound and one-use. Never saved or put into diagnostics.
class LoginConfirmation {
  LoginConfirmation._(
    this.origin,
    this._token,
    this.expiresAt,
    this.existingLogin,
  );
  final Uri origin;
  final ExistingLoginContext? existingLogin;
  String? _token;
  final DateTime expiresAt;
  bool get forgotten => _token == null;
  void forget() => _token = null;

  static LoginConfirmation parse(Object? value, Uri origin, DateTime now) {
    if (value is! Map ||
        (value.length != 3 && value.length != 4) ||
        value.keys.any(
          (key) => !const {
            'protocol',
            'challengeToken',
            'expiresInMs',
            'existingLogin',
          }.contains(key),
        ) ||
        value['protocol'] != sessionAccessProtocol ||
        value['challengeToken'] is! String ||
        value['expiresInMs'] is! int) {
      throw const FormatException();
    }
    final token = value['challengeToken'] as String;
    final lifetime = value['expiresInMs'] as int;
    if (!RegExp(r'^[A-Za-z0-9_-]{16,4096}$').hasMatch(token) ||
        lifetime < 1 ||
        lifetime > 120000) {
      throw const FormatException();
    }
    return LoginConfirmation._(
      origin,
      token,
      now.add(Duration(milliseconds: lifetime)),
      value.containsKey('existingLogin')
          ? ExistingLoginContext.parse(value['existingLogin'])
          : null,
    );
  }

  String consume(Uri expectedOrigin, DateTime now) {
    final token = _token;
    forget();
    if (origin != expectedOrigin || token == null || !expiresAt.isAfter(now)) {
      throw const FormatException();
    }
    return token;
  }
}
