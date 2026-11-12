import '../l10n/l10n.dart';

/// Explicit, single-use takeover context from an opted-in current broker.
/// It identifies the observed controller set; it never authorizes ordinary
/// connect or invents the old device's identity. The broker owns expiry/CAS.
const streamTakeoverProtocol = 'takeover-context-v1';
const streamTakeoverHeader = 'X-Multitap-Takeover-Context';
String get streamTakeoverUnavailableMessage =>
    l10n.desktopConnectedSomewhereElseInformation;

/// What the "다른 곳에서 연결 중이에요" card says about each connection its
/// button would end: the account and, where the broker attests it, when that
/// connection began (local time). The card is the only question before the
/// takeover, so these lines are what the person agrees to (2026-11-09: a
/// desktop another account uses is taken over only after a confirmation).
/// A start time the broker does not attest is left out, never invented.
List<String> streamTakeoverSessionLines(StreamTakeoverOffer offer) {
  String two(int value) => value.toString().padLeft(2, '0');
  return [
    for (final session in offer.sessions)
      if (session.createdAt?.toLocal() case final at?)
        // The date by locale (docs/i18n.md): "11월 6일" / "Nov 6".
        l10n.connectedAccountSinceDate(
          session.username,
          formatMonthDay(at),
          '${two(at.hour)}:${two(at.minute)}',
        )
      else
        l10n.connectedAccount(session.username),
  ];
}

bool _safeText(Object? value, int max) {
  if (value is! String ||
      value.trim().isEmpty ||
      value.length > max * 2 ||
      value.runes.length > max) {
    return false;
  }
  final units = value.codeUnits;
  for (var i = 0; i < units.length; i++) {
    final unit = units[i];
    if (unit < 0x20 ||
        (unit >= 0x7f && unit <= 0x9f) ||
        unit == 0x061c ||
        unit == 0x200e ||
        unit == 0x200f ||
        unit == 0x2028 ||
        unit == 0x2029 ||
        (unit >= 0x202a && unit <= 0x202e) ||
        (unit >= 0x2066 && unit <= 0x2069)) {
      return false;
    }
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++i >= units.length || units[i] < 0xdc00 || units[i] > 0xdfff) {
        return false;
      }
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      return false;
    }
  }
  return true;
}

bool _keys(Map value, Set<String> expected) =>
    value.length == expected.length && value.keys.every(expected.contains);

class StreamTakeoverSession {
  const StreamTakeoverSession._(this.username, this.createdAt);
  final String username;

  /// Null means the broker cannot attest this legacy stream's start time.
  final DateTime? createdAt;

  /// The current protocol has no observed remote-device inventory.
  String? get deviceLabel => null;
}

class StreamTakeoverOffer {
  StreamTakeoverOffer._({
    required this.context,
    required this.expiresInSeconds,
    required this.desktopId,
    required this.desktopName,
    required List<StreamTakeoverSession> sessions,
    required this.receivedAt,
  }) : sessions = List.unmodifiable(sessions);

  final String context;
  final int expiresInSeconds;
  final String desktopId, desktopName;
  final List<StreamTakeoverSession> sessions;
  final DateTime receivedAt;

  bool currentAt(DateTime now) =>
      !now.isBefore(receivedAt) &&
      now.difference(receivedAt) < Duration(seconds: expiresInSeconds);

  factory StreamTakeoverOffer.parse(
    Object? value, {
    String? expectedDesktopId,
    DateTime? receivedAt,
  }) {
    Never invalid() => throw const FormatException('invalid_takeover_context');
    if (value is! Map ||
        !_keys(value, {
          'protocol',
          'context',
          'expiresInSeconds',
          'desktop',
          'sessions',
        }) ||
        value['protocol'] != streamTakeoverProtocol) {
      invalid();
    }
    final context = value['context'];
    final ttl = value['expiresInSeconds'];
    final desktop = value['desktop'];
    final rawSessions = value['sessions'];
    if (context is! String ||
        !RegExp(r'^[A-Za-z0-9_-]{43}$').hasMatch(context) ||
        ttl is! int ||
        ttl < 1 ||
        ttl > 120 ||
        desktop is! Map ||
        !_keys(desktop, {'id', 'name'}) ||
        desktop['id'] is! String ||
        !RegExp(r'^[A-Za-z0-9_-]{1,80}$').hasMatch(desktop['id'] as String) ||
        !_safeText(desktop['name'], 128) ||
        (expectedDesktopId != null && desktop['id'] != expectedDesktopId) ||
        rawSessions is! List ||
        rawSessions.isEmpty ||
        rawSessions.length > 5) {
      invalid();
    }
    final sessions = <StreamTakeoverSession>[];
    for (final session in rawSessions) {
      if (session is! Map ||
          !_keys(session, {'username', 'createdAt', 'deviceLabel'}) ||
          !_safeText(session['username'], 64) ||
          session['deviceLabel'] != null) {
        invalid();
      }
      final rawCreated = session['createdAt'];
      DateTime? created;
      if (rawCreated != null) {
        if (rawCreated is! String ||
            !RegExp(r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$')
                .hasMatch(rawCreated)) {
          invalid();
        }
        created = DateTime.tryParse(rawCreated);
        if (created == null || !created.isUtc) invalid();
        // DateTime.parse normalizes invalid calendar dates; reject those too.
        if (created.toIso8601String() != rawCreated) invalid();
      }
      sessions.add(
        StreamTakeoverSession._(session['username'] as String, created),
      );
    }
    return StreamTakeoverOffer._(
      context: context,
      expiresInSeconds: ttl,
      desktopId: desktop['id'] as String,
      desktopName: desktop['name'] as String,
      sessions: sessions,
      receivedAt: (receivedAt ?? DateTime.now()).toUtc(),
    );
  }
}
