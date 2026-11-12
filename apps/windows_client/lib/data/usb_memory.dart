import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';

import 'user_data_path.dart';

/// The USB devices the user has already given to a VM, so the next time they
/// are listed right after the ones just plugged in (the certificate USB stays
/// near the top). Only a hash of "VID:PID[:serial]" is stored, on this PC,
/// in the user's own profile; it is never sent anywhere.
abstract interface class UsbMemory {
  Future<Set<String>> load();
  Future<void> remember(String memoryKey);
}

/// Hash of a device's memory key (what [UsbMemory] stores and compares).
String usbMemoryHash(String memoryKey) =>
    sha256.convert(utf8.encode('multitap-usb\n$memoryKey')).toString().substring(0, 24);

/// This process only (tests and previews).
class SessionUsbMemory implements UsbMemory {
  SessionUsbMemory([Iterable<String> keys = const []])
    : _hashes = {for (final key in keys) usbMemoryHash(key)};
  final Set<String> _hashes;
  @override
  Future<Set<String>> load() async => Set.of(_hashes);
  @override
  Future<void> remember(String memoryKey) async {
    _hashes.add(usbMemoryHash(memoryKey));
  }
}

/// `%LOCALAPPDATA%\Multitap\Client\usb-memory.json`: the newest [maxKeys] hashes.
class FileUsbMemory implements UsbMemory {
  FileUsbMemory(this.file);
  final File file;
  static const maxKeys = 64;

  factory FileUsbMemory.windows() {
    final root = Platform.environment['LOCALAPPDATA'];
    if (root == null || root.isEmpty) {
      throw StateError('Windows app data directory is unavailable.');
    }
    return FileUsbMemory(File('$root/Multitap/Client/usb-memory.json'));
  }

  Future<List<String>> _read() async {
    try {
      if (await file.length() > 8192) return [];
      final value = jsonDecode(await file.readAsString());
      if (value is! Map || value['devices'] is! List) return [];
      return [
        for (final item in value['devices'] as List)
          if (item is String && RegExp(r'^[0-9a-f]{24}$').hasMatch(item)) item,
      ];
    } on FileSystemException {
      return [];
    } on FormatException {
      return [];
    }
  }

  @override
  Future<Set<String>> load() async => (await _read()).toSet();

  @override
  Future<void> remember(String memoryKey) async {
    final hash = usbMemoryHash(memoryKey);
    final hashes = await _read();
    hashes.remove(hash);
    hashes.add(hash);
    final kept = hashes.length > maxKeys
        ? hashes.sublist(hashes.length - maxKeys)
        : hashes;
    await writeUserDataFile(file, jsonEncode({'devices': kept}));
  }
}
