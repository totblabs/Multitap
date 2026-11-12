import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';

import 'client_settings.dart';
import 'broker_transport.dart';
import 'user_data_path.dart';

/// A password is remembered only for an HTTPS server: over plain HTTP it
/// crosses the network readable, and the
/// silent re-login would replay it. For an HTTP server the "비밀번호 기억"
/// switch is off and disabled, and a password stored earlier is deleted at
/// the next sign-in (and never read back meanwhile). Windows and Android.
bool passwordMemoryAllowed(Uri? origin) => origin?.scheme == 'https';

class RememberedLogin {
  const RememberedLogin({this.username = '', this.password});

  final String username;
  final String? password;
}

abstract interface class LoginMemory {
  Future<RememberedLogin> load(Uri origin);
  Future<void> saveUsername(Uri origin, String username);
  Future<void> savePassword(Uri origin, String username, String password);
  Future<void> forgetPassword(Uri origin);
}

/// Optional serialized compare-and-forget. This protects this memory instance's
/// queued writes; it is not a transaction across unrelated WinCred processes.
abstract interface class ConditionalLoginMemory {
  Future<bool> forgetPasswordIfMatches(
    Uri origin,
    RememberedLogin expected, {
    required bool Function() current,
  });
}

/// The default for design previews and tests: no disk or platform access.
class NoopLoginMemory implements LoginMemory {
  const NoopLoginMemory();

  @override
  Future<RememberedLogin> load(Uri origin) async => const RememberedLogin();
  @override
  Future<void> saveUsername(Uri origin, String username) async {}
  @override
  Future<void> savePassword(
    Uri origin,
    String username,
    String password,
  ) async {}
  @override
  Future<void> forgetPassword(Uri origin) async {}
}

abstract interface class CredentialVault {
  Future<RememberedLogin?> load(String origin);
  Future<void> save(String origin, String username, String password);
  Future<void> delete(String origin);
}

class WindowsCredentialVault implements CredentialVault {
  const WindowsCredentialVault();

  static const _channel = MethodChannel('multitap/credentials');

  @override
  Future<RememberedLogin?> load(String origin) async {
    final result = await _channel.invokeMapMethod<String, dynamic>('load', {
      'origin': origin,
    });
    if (result == null) return null;
    final username = result['username'];
    final password = result['password'];
    if (username is! String || password is! String) {
      throw const FormatException('Invalid credential vault response.');
    }
    return RememberedLogin(username: username, password: password);
  }

  @override
  Future<void> save(String origin, String username, String password) =>
      _channel.invokeMethod<void>('save', {
        'origin': origin,
        'username': username,
        'password': password,
      });

  @override
  Future<void> delete(String origin) =>
      _channel.invokeMethod<void>('delete', {'origin': origin});
}

/// IDs are ordinary per-server metadata. Passwords only enter the Windows
/// Credential Manager; failures never fall back to a password file.
class WindowsLoginMemory implements LoginMemory, ConditionalLoginMemory {
  WindowsLoginMemory(this.metadataFile, {CredentialVault? vault})
    : _vault = vault ?? const WindowsCredentialVault();

  factory WindowsLoginMemory.windows() {
    final appData = Platform.environment['LOCALAPPDATA'];
    if (appData == null || appData.isEmpty) {
      throw StateError('Windows app data directory is unavailable.');
    }
    return WindowsLoginMemory(File('$appData/Multitap/Client/login.json'));
  }

  final File metadataFile;

  /// The removed "로그아웃 후 비밀번호 다시 입력" option kept its state in a
  /// file beside the metadata. Nothing reads it any more: delete the leftover.
  Future<void> deleteRetiredLogoutOption() async {
    try {
      final leftover = File('${metadataFile.parent.path}/login-privacy.json');
      if (await leftover.exists()) await leftover.delete();
    } on FileSystemException {
      // Locked or unreadable: it stays, and the next start tries again.
    }
  }

  final CredentialVault _vault;
  Future<void> _pending = Future<void>.value();

  // A single queue keeps a fast ID edit/opt-out/login sequence in order.
  Future<T> _ordered<T>(Future<T> Function() action) {
    final result = _pending.then((_) => action());
    _pending = result.then<void>((_) {}, onError: (Object _, StackTrace _) {});
    return result;
  }

  // Native read/write enforces current exact consent. Keep erasure possible
  // after consent withdrawal; normalizing a key does not authorize networking.
  String _key(Uri origin) =>
      parseServerOrigin(origin.toString()).origin.toLowerCase();

  String _username(String value) {
    final username = value.trim();
    if (username.runes.length > 64 || username.contains('\u0000')) {
      throw const FormatException('Invalid remembered login ID.');
    }
    return username;
  }

  Future<Map<String, String>> _readUsernames() async {
    try {
      final data = jsonDecode(await metadataFile.readAsString());
      if (data is! Map || data['version'] != 1 || data['usernames'] is! Map) {
        return {};
      }
      final result = <String, String>{};
      for (final entry in (data['usernames'] as Map).entries) {
        if (entry.key is! String || entry.value is! String) continue;
        try {
          final key = _key(Uri.parse(entry.key as String));
          result[key] = _username(entry.value as String);
        } on FormatException {
          continue;
        }
      }
      return result;
    } on FileSystemException {
      return {};
    } on FormatException {
      return {};
    }
  }

  Future<void> _writeUsernames(Map<String, String> usernames) async {
    await writeUserDataFile(
      metadataFile,
      jsonEncode({'version': 1, 'usernames': usernames}),
    );
  }

  Future<void> _saveUsername(String key, String username) async {
    final usernames = await _readUsernames();
    if (usernames[key] == username) return;
    // A password from a different account must not follow the changed ID.
    await _vault.delete(key);
    usernames[key] = username;
    await _writeUsernames(usernames);
  }

  @override
  Future<RememberedLogin> load(Uri origin) =>
      _ordered(() => _loadRemembered(origin));

  Future<RememberedLogin> _loadRemembered(Uri origin) async {
    final key = _key(origin);
    final admission = BrokerTransport.token(origin);
    final usernames = await _readUsernames();
    final username = usernames[key] ?? '';
    if (username.isEmpty) return const RememberedLogin();
    if (!passwordMemoryAllowed(origin)) return RememberedLogin(username: username);
    BrokerTransport.requireCurrent(origin, admission);
    final credential = await _vault.load(key);
    if (credential?.username != username) {
      return RememberedLogin(username: username);
    }
    return credential!;
  }

  @override
  Future<bool> forgetPasswordIfMatches(
    Uri origin,
    RememberedLogin expected, {
    required bool Function() current,
  }) => _ordered(() async {
    if (!current() || expected.password == null || expected.password!.isEmpty) {
      return false;
    }
    final actual = await _loadRemembered(origin);
    if (!current() ||
        actual.username != expected.username ||
        actual.password != expected.password) {
      return false;
    }
    // No other operation of this instance can save between comparison and
    // deletion. A later save remains queued after this exact erasure.
    await _vault.delete(_key(origin));
    return true;
  });

  @override
  Future<void> saveUsername(Uri origin, String username) => _ordered(() async {
    final admission = BrokerTransport.token(origin);
    final key = _key(origin), validUsername = _username(username);
    BrokerTransport.requireCurrent(origin, admission);
    await _saveUsername(key, validUsername);
  });

  @override
  Future<void> savePassword(Uri origin, String username, String password) =>
      _ordered(() async {
        final key = _key(origin);
        final admission = BrokerTransport.token(origin);
        final validUsername = _username(username);
        if (validUsername.isEmpty ||
            password.isEmpty ||
            utf8.encode(password).length > 2560 ||
            password.contains('\u0000')) {
          throw const FormatException('Invalid remembered credential.');
        }
        await _saveUsername(key, validUsername);
        if (!passwordMemoryAllowed(origin)) {
          // Never remembered over HTTP; whatever was stored goes.
          await _vault.delete(key);
          return;
        }
        BrokerTransport.requireCurrent(origin, admission);
        await _vault.save(key, validUsername, password);
      });

  @override
  Future<void> forgetPassword(Uri origin) =>
      _ordered(() => _vault.delete(_key(origin)));
}
