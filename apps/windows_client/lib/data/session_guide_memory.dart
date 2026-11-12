import 'broker_transport.dart';

import 'dart:convert';
import 'dart:io';
import 'dart:isolate';

import 'package:crypto/crypto.dart';

import 'user_data_path.dart';

final _guideKey = RegExp(r'^[0-9a-f]{64}$');

bool isSessionGuideAccountKey(Object? value) =>
    value is String && _guideKey.hasMatch(value);

/// Presentation preference scope only. Never persist an account name, ID,
/// bearer or password. Renaming the login ID cannot change this key.
String? sessionGuideAccountKey(Uri origin, String? immutableAccountId) {
  if (!brokerOriginAllowed(origin) ||
      origin.host.isEmpty ||
      immutableAccountId == null ||
      immutableAccountId.isEmpty ||
      immutableAccountId.length > 256) {
    return null;
  }
  return sha256
      .convert(
        utf8.encode(
          'Multitap/session-guide/account/v1\n${origin.origin}\n$immutableAccountId',
        ),
      )
      .toString();
}

class SessionGuideIdentity {
  SessionGuideIdentity({required this.accountKey, required this.desktopId}) {
    if (!isSessionGuideAccountKey(accountKey) ||
        !RegExp(r'^[A-Za-z0-9._:-]{1,128}$').hasMatch(desktopId)) {
      throw const FormatException('Invalid guide preference scope.');
    }
  }
  final String accountKey, desktopId;
  String get storageKey => sha256
      .convert(
        utf8.encode(
          'Multitap/session-guide/desktop/v1\n$accountKey\n$desktopId',
        ),
      )
      .toString();
}

abstract interface class SessionGuideStore {
  bool get persistent;
  Future<bool> hidden(SessionGuideIdentity identity);
  Future<void> setHidden(SessionGuideIdentity identity, bool hidden);
}

/// Optional capability; existing LauncherMemory implementers need no new API.
abstract interface class SessionGuideStoreProvider {
  SessionGuideStore get sessionGuideStore;
}

class MemorySessionGuideStore implements SessionGuideStore {
  final _values = <String, bool>{};
  @override
  bool get persistent => false;
  @override
  Future<bool> hidden(SessionGuideIdentity identity) async =>
      _values[identity.storageKey] ?? false;
  @override
  Future<void> setHidden(SessionGuideIdentity identity, bool hidden) async {
    _values[identity.storageKey] = hidden;
  }
}

/// One tiny plain file per server/account/VM hash, so separate VM windows do
/// not overwrite a shared JSON map or the launcher's preferences.
class FileSessionGuideStore implements SessionGuideStore {
  FileSessionGuideStore(this.directory);
  final Directory directory;
  @override
  bool get persistent => true;
  File _file(SessionGuideIdentity identity) =>
      File('${directory.path}/${identity.storageKey}.json');
  @override
  Future<bool> hidden(SessionGuideIdentity identity) {
    final path = _file(identity).path;
    // The path validator deliberately uses synchronous link checks. Keep those
    // checks, file decoding and disk access off the UI isolate as well.
    return Isolate.run(() => FileSessionGuideStore._readHidden(path));
  }

  static Future<bool> _readHidden(String path) async {
    final file = File(path);
    requirePlainUserDataPath(file.path);
    try {
      final bytes = await file
          .openRead(0, 1025)
          .fold<List<int>>(<int>[], (bytes, chunk) => bytes..addAll(chunk));
      if (bytes.length > 1024) {
        throw const FormatException('Oversized guide preference.');
      }
      final value = jsonDecode(utf8.decode(bytes));
      if (value is! Map || value['schema'] != 1 || value['hidden'] is! bool) {
        throw const FormatException('Invalid guide preference.');
      }
      return value['hidden'] as bool;
    } on FileSystemException catch (error) {
      if (error.osError?.errorCode == 2 || error.osError?.errorCode == 3) {
        return false;
      }
      rethrow;
    }
  }

  @override
  Future<void> setHidden(SessionGuideIdentity identity, bool hidden) {
    final path = _file(identity).path;
    final contents = jsonEncode({'schema': 1, 'hidden': hidden});
    return Isolate.run(() => writeUserDataFile(File(path), contents));
  }
}
