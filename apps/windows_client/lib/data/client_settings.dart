import 'dart:convert';
import 'dart:io';

import 'user_data_path.dart';
import 'broker_transport.dart';

Uri parseServerOrigin(String input, {String defaultScheme = 'https'}) =>
    normalizeBrokerOrigin(input, defaultScheme: defaultScheme);

class ClientSettings {
  ClientSettings(this.file);
  final File file;

  /// A settings file this client wrote is a few dozen bytes.
  static const maximumBytes = 4096;

  factory ClientSettings.windows() {
    final appData = Platform.environment['LOCALAPPDATA'];
    if (appData == null || appData.isEmpty) {
      throw StateError('Windows app data directory is unavailable.');
    }
    return ClientSettings(File('$appData/Multitap/Client/settings.json'));
  }

  /// Legacy one-field HTTPS or a strict version-2 origin/consent record.
  Future<Uri?> _loadFile() async {
    try {
      if (await file.length() > maximumBytes) {
        BrokerTransport.clear();
        return null;
      }
      final value = jsonDecode(await file.readAsString());
      final origin = decodeBrokerTransportSettings(value);
      BrokerTransport.restore(
        origin,
        value is Map ? value['httpConsentOrigin'] : null,
      );
      return origin;
    } on FileSystemException {
      BrokerTransport.clear();
      return null;
    } on FormatException {
      BrokerTransport.clear();
      return null;
    }
  }

  /// The server origin this user confirmed in the app. The client runs with the
  /// user's own token, so this per-user file is not a
  /// privilege boundary: what an update installer may be is decided by the
  /// project's release signature, not by this origin.
  Future<Uri?> load() => _loadFile();

  Future<void> save(Uri origin) async {
    final valid = parseServerOrigin(origin.toString());
    await writeUserDataFile(
      file,
      jsonEncode(encodeBrokerTransportSettings(valid)),
    );
    BrokerTransport.stored(valid);
  }
}
