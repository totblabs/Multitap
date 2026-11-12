import 'dart:async';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';

import 'broker_api.dart';

/// Native WebViews receive a short-lived attachment code. The parent login
/// stays in Dart; the embedded console gets a separate HttpOnly child session.
class ConsoleBrowser {
  const ConsoleBrowser.windows()
    : channel = const MethodChannel('multitap/window');
  const ConsoleBrowser.android()
    : channel = const MethodChannel('multitap/android/application');
  final MethodChannel channel;
  static final _owners = <String, _ConsoleOwner>{};
  static final _operations = <String, Future<void>>{};
  static bool _windowsListening = false;

  static void requireSupport(Uri origin, {required bool supported}) {
    if (origin.scheme != 'https') {
      throw const BrokerException('console_browser_https_required');
    }
    if (!supported) {
      throw const BrokerException('console_webview_update_required');
    }
  }

  static Uri target(Uri origin, String desktopId) {
    if (origin.scheme != 'https' ||
        origin.host.isEmpty ||
        origin.userInfo.isNotEmpty ||
        origin.hasQuery ||
        origin.hasFragment ||
        (origin.path.isNotEmpty && origin.path != '/') ||
        origin.port < 1 ||
        origin.port > 65535 ||
        !BrokerConsoleWebViewGrant.desktopIdValid(desktopId)) {
      throw const BrokerException('console_webview_unavailable');
    }
    return Uri.parse('${origin.origin}/console/$desktopId');
  }

  static Future<void> _serialize(
    String key,
    Future<void> Function() action,
  ) async {
    final previous = _operations[key];
    final finished = Completer<void>();
    _operations[key] = finished.future;
    try {
      if (previous != null) await previous;
      await action();
    } finally {
      if (_operations[key] == finished.future) _operations.remove(key);
      finished.complete();
    }
  }

  static Future<void> handleClosed(
    Object? arguments, {
    String channelName = 'multitap/window',
  }) async {
    if (arguments is! Map ||
        arguments.length != 3 ||
        arguments['origin'] is! String ||
        arguments['desktopId'] is! String ||
        arguments['ownerId'] is! String) {
      return;
    }
    final key = '$channelName|${arguments['origin']}|${arguments['desktopId']}';
    final owner = _owners[key];
    if (owner == null || owner.ownerId != arguments['ownerId']) return;
    await _serialize(key, () async {
      if (_owners[key] != owner) return;
      await owner.revoke();
      if (_owners[key] == owner) _owners.remove(key);
    });
  }

  Future<void> open(
    Uri origin,
    String desktopId, {
    required ConsoleWebViewBrokerApi api,
    required String token,
    required bool Function() current,
  }) async {
    target(origin, desktopId);
    final key = '${channel.name}|${origin.origin}|$desktopId';
    await _serialize(
      key,
      () => _open(key, origin, desktopId, api, token, current),
    );
  }

  Future<void> _open(
    String key,
    Uri origin,
    String desktopId,
    ConsoleWebViewBrokerApi api,
    String token,
    bool Function() current,
  ) async {
    if (!current()) throw const BrokerException('session_expired', 401);
    final previous = _owners[key];
    final sameParent =
        previous != null &&
        previous._revocation == null &&
        identical(previous.api, api) &&
        previous.token == token;
    if (previous != null && !sameParent && !await _closeOwner(key, previous)) {
      throw const BrokerException('console_webview_unavailable');
    }
    final grant = await api.createConsoleWebView(token, desktopId);
    if (!current()) {
      await _ConsoleOwner(
        api,
        token,
        origin.origin,
        desktopId,
        channel.name,
        sha256.convert(grant.code.codeUnits).toString(),
      ).revoke();
      throw const BrokerException('session_expired', 401);
    }
    if (!grant.validFor(origin, desktopId)) {
      throw const BrokerException('invalid_response');
    }
    if (channel.name == 'multitap/window' && !_windowsListening) {
      _windowsListening = true;
      channel.setMethodCallHandler((call) async {
        if (call.method == 'consoleWebViewClosed') {
          await handleClosed(call.arguments, channelName: channel.name);
        }
      });
    }
    final owner = sameParent
        ? previous
        : _ConsoleOwner(
            api,
            token,
            origin.origin,
            desktopId,
            channel.name,
            sha256.convert(grant.code.codeUnits).toString(),
          );
    _owners[key] = owner;
    try {
      final opened = await channel
          .invokeMethod<bool>('openConsoleWebView', {
            'origin': origin.origin,
            'desktopId': desktopId,
            'attachPath': grant.path,
            'code': grant.code,
          })
          .timeout(const Duration(seconds: 5));
      if (opened != true) {
        throw const BrokerException('console_webview_unavailable');
      }
      if (!current()) {
        await _closeOwner(key, owner);
        throw const BrokerException('session_expired', 401);
      }
    } on PlatformException catch (error) {
      if (error.code == 'console_webview_runtime_required') {
        if (_owners[key] == owner) _owners.remove(key);
        await owner.revoke();
      } else {
        await _closeOwner(key, owner);
      }
      throw BrokerException(switch (error.code) {
        'console_webview_runtime_required' ||
        'console_webview_busy' => error.code,
        _ => 'console_webview_unavailable',
      });
    } on MissingPluginException {
      await _closeOwner(key, owner);
      throw const BrokerException('console_webview_unavailable');
    } on TimeoutException {
      await _closeOwner(key, owner);
      throw const BrokerException('console_webview_unavailable');
    } catch (_) {
      await _closeOwner(key, owner);
      rethrow;
    }
  }

  Future<bool> _closeOwner(String key, _ConsoleOwner owner) async {
    if (_owners[key] != owner) return true;
    var nativeClosed = false;
    try {
      await channel
          .invokeMethod<void>('closeConsoleWebViews', {
            'origin': owner.origin,
            'desktopId': owner.desktopId,
          })
          .timeout(const Duration(seconds: 5));
      nativeClosed = true;
    } on PlatformException {
      // Keep ownership to retry native cleanup before another account opens.
    } on MissingPluginException {
      nativeClosed = true;
    } on TimeoutException {
      // A lost close response is not proof that the native window is gone.
    }
    await owner.revoke();
    if (nativeClosed && _owners[key] == owner) _owners.remove(key);
    return nativeClosed;
  }

  Future<void> closeAll({Object? api, String? token}) async {
    await closeAllVerified(api: api, token: token);
  }

  Future<void> hideAll({Object? api, String? token}) {
    final owns = _owners.values.any(
      (owner) =>
          owner.channel == channel.name &&
          (api == null || identical(owner.api, api)) &&
          (token == null || owner.token == token),
    );
    return owns
        ? channel.invokeMethod<void>('hideConsoleWebViews')
        : Future<void>.value();
  }

  Future<bool> closeAllVerified({Object? api, String? token}) async {
    final entries = _owners.entries
        .where(
          (entry) =>
              entry.value.channel == channel.name &&
              (api == null || identical(entry.value.api, api)) &&
              (token == null || entry.value.token == token),
        )
        .toList();
    var verified = true;
    await Future.wait(
      entries.map(
        (entry) => _serialize(entry.key, () async {
          if (!await _closeOwner(entry.key, entry.value)) verified = false;
        }),
      ),
    );
    return verified;
  }
}

class _ConsoleOwner {
  _ConsoleOwner(
    this.api,
    this.token,
    this.origin,
    this.desktopId,
    this.channel,
    this.ownerId,
  );
  final ConsoleWebViewBrokerApi api;
  final String token, origin, desktopId, channel, ownerId;
  Future<void>? _revocation;
  Future<void> revoke() => _revocation ??= _revoke();
  Future<void> _revoke() async {
    try {
      await api
          .closeConsoleWebViews(token, desktopId, ownerId: ownerId)
          .timeout(const Duration(seconds: 5));
    } catch (_) {
      // Parent logout/expiry also revokes children; offline close is best effort.
    }
  }
}
