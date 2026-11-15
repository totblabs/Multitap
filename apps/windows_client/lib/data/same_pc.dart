import 'dart:convert';

import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// same-pc-v1: whether a listed desktop's Host runs on the PC this app runs
/// on. Connecting to it would make the Host move every window, this app
/// included, onto its virtual display, so the app refuses that connection.
///
/// The broker lists `hostMachineTag` with each desktop: the SHA-256 of a
/// fixed line, the desktop id and the Host's SMBIOS system UUID
/// (server/src/same-pc.mjs). The Windows runner reads this PC's UUID the way
/// the Host reads its own (windows/runner/machine_uuid.h) and the same value
/// is derived here; equal values mean the same PC.
///
/// Unknown on either side is never a match: a PC whose UUID cannot be read,
/// a broker that lists no tag and Android all connect as before. The UUID
/// stays on this PC.
class SamePc {
  SamePc._();

  static String? _uuid;
  static final _tags = <String, String>{};
  static final _uuidForm = RegExp(
    r'^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$',
  );
  static final _unset = RegExp(r'^(?:[0-]+|[f-]+)$');
  static final _tagForm = RegExp(r'^[0-9a-f]{64}$');

  /// Whether this PC's UUID is known (only then can a desktop match).
  static bool get known => _uuid != null;

  /// The runner's answer as a UUID, or null for anything else (no answer,
  /// another shape, all zero or all F: a board that never set one).
  static String? parseUuid(Object? value) =>
      value is String && _uuidForm.hasMatch(value) && !_unset.hasMatch(value)
      ? value
      : null;

  /// The broker's `hostMachineTag`, or null where it lists none.
  static String? parseTag(Object? value) =>
      value is String && _tagForm.hasMatch(value) ? value : null;

  /// Reads this PC's UUID once, before the first desktop list is read.
  static Future<void> load({
    MethodChannel channel = const MethodChannel('multitap/window'),
  }) async {
    String? uuid;
    try {
      uuid = parseUuid(await channel.invokeMethod<Object?>('machineUuid'));
    } on MissingPluginException {
      uuid = null;
    } on PlatformException {
      uuid = null;
    }
    _set(uuid);
  }

  static void _set(String? uuid) {
    _uuid = uuid;
    _tags.clear();
  }

  /// Tests stand in for the runner's answer (null: unknown).
  @visibleForTesting
  static void debugSetMachineUuid(Object? uuid) => _set(parseUuid(uuid));

  /// The value the broker lists for [desktopId] when its Host's UUID is
  /// [uuid] (lowercase).
  static String tag(String desktopId, String uuid) => sha256
      .convert(utf8.encode('multitap-same-pc-v1\n$desktopId\n$uuid'))
      .toString();

  /// The desktop [desktopId], listed with [hostMachineTag], is this PC.
  static bool matches(String desktopId, String? hostMachineTag) {
    final uuid = _uuid;
    if (uuid == null || hostMachineTag == null) return false;
    return (_tags[desktopId] ??= tag(desktopId, uuid)) == hostMachineTag;
  }
}
