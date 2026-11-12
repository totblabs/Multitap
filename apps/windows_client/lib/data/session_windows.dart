import 'broker_transport.dart';

import 'dart:async';
import 'dart:math';

import 'package:flutter/services.dart';

import '../models/session_display_mode.dart';
import '../models/stream_options.dart';
import 'client_settings.dart';
import 'desktop_thumbnails.dart';
import 'product_log.dart';
import 'session_guide_memory.dart';
import 'client_session_policy.dart';
import 'session_access.dart';
import '../l10n/l10n.dart';

bool validAuthenticationOwner(Object? value) =>
    value is String && RegExp(r'^[a-f0-9]{32}$').hasMatch(value);

/// Only crosses native messengers in this process. Never written to a file,
/// command line, preferences, or log. The server remains the authorization gate.
class SessionWindowLaunch {
  const SessionWindowLaunch({
    required this.origin,
    required this.token,
    required this.expiresAt,
    required this.desktopId,
    required this.name,
    this.localPcApproved = false,
    required this.mode,
    required this.options,
    this.thumbnailKey,
    this.thumbnailEpoch,
    this.guideAccountKey,
    this.requestedAt,
    this.pcSessionPolicy,
    this.authenticationOwner,
    this.sessionAccess,
  });
  final Uri origin;
  final String token, desktopId, name;
  final DateTime expiresAt;
  final bool localPcApproved;
  final SessionDisplayMode mode;
  final StreamOptions options;

  /// The signed-in account's picture folder (a hash, not an identity), so a
  /// streaming window can leave its last frame for the launcher tile.
  final String? thumbnailKey;
  final String? thumbnailEpoch;

  /// Domain-separated hash of origin + immutable account ID. Preferences only.
  final String? guideAccountKey;

  /// When the launcher asked for this window (connect-timing-v1 windowMs;
  /// diagnostics only, the same PC's clock).
  final DateTime? requestedAt;
  final String? pcSessionPolicy;
  final String? authenticationOwner;
  final SessionAccess? sessionAccess;

  Map<String, Object> toMap() => {
    'origin': origin.toString(),
    'httpConsentOrigin': ?BrokerTransport.consentOrigin(origin),
    'token': token,
    'expiresAt': expiresAt.toUtc().toIso8601String(),
    'desktopId': desktopId,
    'name': name,
    'localPcApproved': localPcApproved,
    'mode': mode.name,
    'options': options.toJson(),
    'thumbnailKey': ?thumbnailKey,
    'thumbnailEpoch': ?thumbnailEpoch,
    'guideAccountKey': ?guideAccountKey,
    'requestedAtMs': ?requestedAt?.millisecondsSinceEpoch,
    if (pcSessionPolicy == windowsPcSessionPolicy)
      'pcSessionPolicy': windowsPcSessionPolicy,
    'authenticationOwner': ?authenticationOwner,
    'sessionAccess': ?sessionAccess?.toJson(),
    'sessionAccessElapsedMs': ?sessionAccess?.elapsed.inMilliseconds,
  };

  factory SessionWindowLaunch.fromMap(Map<Object?, Object?> value) {
    final origin = parseServerOrigin(value['origin'] as String);
    if (origin.scheme == 'http' &&
        value['httpConsentOrigin'] != origin.origin) {
      throw const FormatException('Invalid remote HTTP consent.');
    }
    final token = value['token'] as String;
    final id = value['desktopId'] as String;
    final name = value['name'] as String;
    final expiry = DateTime.parse(value['expiresAt'] as String);
    final mode = SessionDisplayMode.values.byName(value['mode'] as String);
    final thumbnailKey = value['thumbnailKey'];
    final thumbnailEpoch = value['thumbnailEpoch'];
    final guideAccountKey = value['guideAccountKey'];
    if ((value['authenticationOwner'] != null &&
            !validAuthenticationOwner(value['authenticationOwner'])) ||
        (value['sessionAccess'] != null &&
            !validAuthenticationOwner(value['authenticationOwner']))) {
      throw const FormatException('Invalid authentication owner.');
    }
    if (value['sessionAccess'] != null &&
        (value['sessionAccessElapsedMs'] is! int ||
            (value['sessionAccessElapsedMs'] as int) < 0 ||
            (value['sessionAccessElapsedMs'] as int) > 86400000)) {
      throw const FormatException('Invalid session access elapsed time.');
    }
    if (token.isEmpty ||
        token.length > 4096 ||
        !RegExp(
          r'^(?:pve-[0-9]{1,12}|pc-[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12})$',
        ).hasMatch(id) ||
        (id.startsWith('pc-') && value['localPcApproved'] != true) ||
        name.isEmpty ||
        name.length > 256 ||
        (thumbnailKey != null && !isThumbnailAccountKey(thumbnailKey)) ||
        (thumbnailEpoch != null &&
            (thumbnailKey == null ||
                !isThumbnailPrivacyEpoch(thumbnailEpoch))) ||
        (guideAccountKey != null &&
            !isSessionGuideAccountKey(guideAccountKey))) {
      throw const FormatException('Invalid remote window launch.');
    }
    if (origin.scheme == 'http') {
      BrokerTransport.restore(origin, value['httpConsentOrigin']);
    }
    return SessionWindowLaunch(
      origin: origin,
      token: token,
      expiresAt: expiry,
      pcSessionPolicy: value['pcSessionPolicy'] == windowsPcSessionPolicy
          ? windowsPcSessionPolicy
          : null,
      authenticationOwner:
          validAuthenticationOwner(value['authenticationOwner'])
          ? value['authenticationOwner'] as String
          : null,
      sessionAccess: value['sessionAccess'] == null
          ? null
          : SessionAccess.parse(
              value['sessionAccess'],
              responseElapsed: Duration(
                milliseconds: value['sessionAccessElapsedMs'] as int,
              ),
            ),
      desktopId: id,
      name: name,
      localPcApproved: value['localPcApproved'] == true,
      mode: mode,
      options: StreamOptions.fromJson(
        Map<String, dynamic>.from(value['options'] as Map),
      ),
      thumbnailKey: thumbnailKey as String?,
      thumbnailEpoch: thumbnailEpoch as String?,
      guideAccountKey: guideAccountKey as String?,
      // Diagnostics only: anything else is left out, never a failed window.
      requestedAt:
          value['requestedAtMs'] is int &&
              (value['requestedAtMs'] as int) > 0 &&
              (value['requestedAtMs'] as int) < 8640000000000000
          ? DateTime.fromMillisecondsSinceEpoch(value['requestedAtMs'] as int)
          : null,
    );
  }
}

class SessionWindowOpenResult {
  const SessionWindowOpenResult({required this.opened});

  /// A new window opened; false: the desktop's open window was shown.
  final bool opened;
}

class SessionWindows {
  static const channel = MethodChannel('multitap/session-windows');
  static bool _retireRequested = false;
  static Future<void> Function()? _retire;
  static String? _authenticationOwner;
  static void Function()? _human;
  static void Function(String)? _terminal;
  static void Function(SessionAccess)? _deadline;
  static String newAuthenticationOwner() {
    final random = Random.secure();
    return List.generate(
      16,
      (_) => random.nextInt(256).toRadixString(16).padLeft(2, '0'),
    ).join();
  }

  static void observeAuthentication(
    String? owner, {
    void Function()? human,
    void Function(String)? terminal,
    void Function(SessionAccess)? deadline,
  }) {
    _authenticationOwner = owner;
    _human = human;
    _terminal = terminal;
    _deadline = deadline;
  }

  static Future<void> bindAuthentication(String owner, SessionAccess? policy) =>
      channel.invokeMethod<void>('bindAuthentication', {
        'owner': owner,
        if (policy != null) 'idleRemainingMs': policy.remaining?.inMilliseconds,
      });
  static Future<void> forwardHuman(String owner) =>
      channel.invokeMethod<void>('humanActivity', {'owner': owner});
  static Future<void> forwardTerminal(String owner, String reason) =>
      channel.invokeMethod<void>('authenticationEnded', {
        'owner': owner,
        'reason': reason,
      });
  static Future<void> broadcastDeadline(String owner, SessionAccess policy) =>
      channel.invokeMethod<void>('authenticationDeadline', {
        'owner': owner,
        'sessionAccess': policy.toJson(),
        'elapsedMs': policy.elapsed.inMilliseconds,
        'remainingMs': policy.remaining?.inMilliseconds,
      });
  static Future<void> hideAuthentication(String owner) =>
      channel.invokeMethod<void>('hideAuthentication', {'owner': owner});
  static Future<void> retireAuthentication(String owner) =>
      channel.invokeMethod<void>('retireAuthentication', {'owner': owner});
  static final _thumbnails = StreamController<ThumbnailUpdate>.broadcast();

  /// The dashboard hears when a session window replaced a desktop's picture.
  /// Only the folder key and desktop id cross; never the picture itself.
  static Stream<ThumbnailUpdate> get thumbnailUpdates => _thumbnails.stream;

  static void listen({ProductLog? log}) {
    channel.setMethodCallHandler((call) async {
      if (call.method == 'authenticationEvent') {
        final value = call.arguments;
        if (value is! Map ||
            value['owner'] != _authenticationOwner ||
            !validAuthenticationOwner(value['owner'])) {
          return;
        }
        if (value['kind'] == 'human') _human?.call();
        if (value['kind'] == 'terminal' &&
            const {
              'pc_login_replaced',
              'session_idle_timeout',
              'session_reauthentication_required',
            }.contains(value['reason'])) {
          _terminal?.call(value['reason'] as String);
        }
        if (value['kind'] == 'deadline' &&
            value['elapsedMs'] is int &&
            (value['elapsedMs'] as int) >= 0 &&
            (value['elapsedMs'] as int) <= 86400000) {
          try {
            _deadline?.call(
              SessionAccess.parse(
                value['sessionAccess'],
                responseElapsed: Duration(
                  milliseconds: value['elapsedMs'] as int,
                ),
              ),
            );
          } on FormatException {
            /* Unknown metadata never updates a deadline. */
          }
        }
      } else if (call.method == 'log') {
        (log ?? ProductLog.instance).acceptForwarded(call.arguments);
      } else if (call.method == 'closeRequested') {
        _retireRequested = true;
        await _retire?.call();
      } else if (call.method == 'thumbnailUpdated') {
        final value = call.arguments;
        if (value is Map &&
            isThumbnailAccountKey(value['key']) &&
            value['desktopId'] is String &&
            isThumbnailPrivacyEpoch(value['epoch']) &&
            (value['generation'] == null ||
                (value['generation'] is int &&
                    (value['generation'] as int) >= 0)) &&
            (value['invalidated'] == null || value['invalidated'] is bool)) {
          _thumbnails.add(
            ThumbnailUpdate(
              value['key'] as String,
              value['desktopId'] as String,
              generation:
                  value['generation'] is int &&
                      (value['generation'] as int) >= 0
                  ? value['generation'] as int
                  : 0,
              invalidated: value['invalidated'] == true,
              epoch: value['epoch'] as String,
            ),
          );
        }
      } else if (call.method == 'consoleRequested') {
        // A session window asked to open the Console of its VM (signed out or
        // locked Windows): the launcher opens it after its own checks.
        final value = call.arguments;
        final desktop = value is Map ? value['desktopId'] : null;
        if (desktop is String &&
            RegExp(r'^[A-Za-z0-9._-]{1,128}$').hasMatch(desktop)) {
          await _console?.call(desktop);
        }
      }
    });
  }

  /// The launcher's handler for [openConsoleInLauncher] (null: none).
  static Future<void> Function(String desktopId)? _console;
  static void bindConsoleRequests(Future<void> Function(String)? open) =>
      _console = open;

  /// From a session window: asks the launcher to open this VM's Console.
  static Future<bool> openConsoleInLauncher(String desktopId) async {
    try {
      return await channel.invokeMethod<bool>('openConsoleInLauncher', {
            'desktopId': desktopId,
          }) ==
          true;
    } on PlatformException {
      return false;
    } on MissingPluginException {
      return false;
    }
  }

  static Future<void> forwardLog(ProductLogEntry entry) async {
    // Recheck at the boundary too: callers cannot forward arbitrary text by
    // constructing a ProductLogEntry directly.
    final safe = ProductLogEntry.parse(entry.toJson());
    if (safe == null) return;
    await channel
        .invokeMethod<void>('log', safe.toJson())
        .timeout(const Duration(seconds: 2));
  }

  static void bindRetirement(Future<void> Function()? action) {
    _retire = action;
    if (action != null && _retireRequested) scheduleMicrotask(action);
  }

  static Future<SessionWindowLaunch?> bootstrap() async {
    final value = await channel.invokeMapMethod<Object?, Object?>('bootstrap');
    return value == null ? null : SessionWindowLaunch.fromMap(value);
  }

  static Future<SessionWindowOpenResult?> open(
    SessionWindowLaunch launch,
  ) async {
    final result = await channel.invokeMapMethod<Object?, Object?>('open', {
      'key': '${launch.origin}|${launch.desktopId}',
      'title':
          '${launch.name} · ${l10n.streaming} · Multitap',
      'payload': launch.toMap(),
    });
    if (result?['opened'] is! bool) return null;
    return SessionWindowOpenResult(opened: result!['opened'] as bool);
  }

  static Future<void> closeAll(String reason) async {
    try {
      await channel.invokeMethod<void>('closeAll', {'reason': reason});
    } on MissingPluginException {
      // Non-Windows previews have no detached windows.
    }
  }

  /// Cancels only this captured epoch and queues native exact-handle cleanup.
  /// Accepted is not evidence that the JPEG was deleted from disk.
  static Future<bool> invalidateThumbnail(
    String key,
    String desktopId,
    String epoch,
  ) async {
    if (!isThumbnailAccountKey(key) ||
        !isThumbnailDesktopId(desktopId) ||
        !isThumbnailPrivacyEpoch(epoch)) {
      return false;
    }
    try {
      return await channel
              .invokeMethod<bool>('thumbnailInvalidate', {
                'key': key,
                'desktopId': desktopId,
                'epoch': epoch,
              })
              .timeout(const Duration(seconds: 2)) ==
          true;
    } on MissingPluginException {
      return false;
    } on PlatformException {
      return false;
    } on TimeoutException {
      return false;
    }
  }
}

/// A view registers its actual cleanup, so X, toolbar, logout and expiry all
/// retire the same native media owner before destroying its window.
class SessionWindowActions {
  SessionWindowActions({required this.closeWindow});
  final Future<void> Function() closeWindow;
  Future<void> Function()? fallback;
  Object? _owner;
  Future<void> Function()? _confirm, _retire;
  void bind(
    Object owner, {
    required Future<void> Function() confirm,
    required Future<void> Function() retire,
  }) {
    _owner = owner;
    _confirm = confirm;
    _retire = retire;
  }

  void unbind(Object owner) {
    if (!identical(owner, _owner)) return;
    _owner = null;
    _confirm = null;
    _retire = null;
  }

  Future<void> requestClose() async => await (_confirm ?? fallback)?.call();
  Future<void> retire() async => await (_retire ?? closeWindow).call();
}
