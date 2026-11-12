import '../l10n/l10n.dart';

/// Whether a remote connection holds the VM now (broker `activeSession`,
/// 2026-10-26): this account's own, another account's, or, from a broker
/// that reports only "a connection" in its Host update view, [someone].
/// Display only; the broker decides every connection.
enum DesktopUse {
  idle,
  mine,
  other,
  someone;

  bool get active => this != idle;

  /// The tile's line about it, or null when nobody is connected.
  String? get label => switch (this) {
    idle => null,
    mine => l10n.connectedAccount2,
    other => l10n.anotherUserUsing,
    someone => l10n.connectionAlreadyOpen,
  };

  /// The same in a few words (list rows).
  String? get short => switch (this) {
    idle => null,
    mine => l10n.connection,
    other => l10n.useByAnotherUser,
    someone => l10n.use,
  };

  /// The line and what connecting would do (a tooltip on Windows, the
  /// card's two-line detail on Android).
  String? get hint => switch (this) {
    idle => null,
    mine => l10n.connectedAccountIfItsConnection,
    other => l10n.anotherUserUsingWhenYou,
    someone => l10n.desktopAlreadyHasOpenConnection,
  };
}

/// The tile's lines naming the other account; without a name (an older
/// broker) the plain lines above.
extension DesktopUseAccount on DesktopUse {
  String? labelFor(String? account) =>
      this == DesktopUse.other && account != null ? l10n.accountUsingIt(account) : label;
  String? shortFor(String? account) =>
      this == DesktopUse.other && account != null ? l10n.inUseByAccount(account) : short;
  String? hintFor(String? account) =>
      this == DesktopUse.other && account != null ? l10n.accountUsingItWhenYou(account) : hint;
}

/// The account another person is connected as (broker `activeSession.account`;
/// an older server sends none): plain text up to 64 characters, else null.
String? desktopUseAccountFromJson(dynamic activeSession) {
  if (activeSession is! Map || activeSession['mine'] != false) return null;
  final account = activeSession['account'];
  if (account is! String) return null;
  final text = account.trim();
  if (text.isEmpty || text.runes.length > 64) return null;
  if (text.runes.any((r) => r < 0x20 || (r >= 0x7f && r <= 0x9f) || (r >= 0x202a && r <= 0x202e) || (r >= 0x2066 && r <= 0x2069) || r == 0x200e || r == 0x200f)) return null;
  return text;
}

/// [activeSession] from a broker that reports it (`{mine: bool}`), otherwise
/// the `sessionActive` flag of its Host update view (host-update-v1).
DesktopUse desktopUseFromJson(dynamic activeSession, dynamic hostUpdate) {
  if (activeSession is Map) {
    final mine = activeSession['mine'];
    if (mine is bool) return mine ? DesktopUse.mine : DesktopUse.other;
    return DesktopUse.someone;
  }
  if (hostUpdate is Map &&
      hostUpdate['protocol'] == 'host-update-v1' &&
      hostUpdate['sessionActive'] == true) {
    return DesktopUse.someone;
  }
  return DesktopUse.idle;
}
