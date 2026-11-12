import 'usb_memory.dart';
import 'usb_redirection.dart';

/// How long a device counts as "방금 연결됨" after Windows saw it arrive.
const usbJustPluggedWindow = Duration(minutes: 2);

class UsbPickerRow {
  const UsbPickerRow(this.entry, {required this.justPlugged, required this.remembered});
  final UsbEntry entry;

  /// Plugged in within the last [usbJustPluggedWindow] (and not one this
  /// session just gave back).
  final bool justPlugged;

  /// Given to a VM before.
  final bool remembered;
}

/// The USB panel's list: drives and security tokens up front, everything else
/// under "기타 USB 장치", and how many devices this PC needs for itself were left
/// out (keyboard, mouse, hub, USB network adapter).
class UsbPicker {
  const UsbPicker({
    required this.main,
    required this.other,
    required this.hiddenNeeded,
    this.badgeEnds,
  });
  final List<UsbPickerRow> main, other;
  final int hiddenNeeded;

  /// When the first "방금 연결됨" badge runs out (the order changes then), or null.
  final Duration? badgeEnds;

  List<UsbPickerRow> get all => [...main, ...other];
  bool get isEmpty => main.isEmpty && other.isEmpty;
}

bool _isMain(UsbKind kind) => kind == UsbKind.storage || kind == UsbKind.token;

UsbPicker buildUsbPicker({
  required List<UsbEntry> entries,
  required DateTime now,
  required Set<String> remembered,
  required int hiddenNeeded,
}) {
  Duration? ends;
  final rows = <UsbPickerRow>[];
  for (final entry in entries) {
    final arrival = entry.device.arrivalMs;
    var just = false;
    if (arrival > 0 && !entry.everShared) {
      final age = now.difference(DateTime.fromMillisecondsSinceEpoch(arrival));
      // A clock that is a little ahead of Windows' stamp still counts as new.
      just = age < usbJustPluggedWindow && age > const Duration(seconds: -30);
      if (just) {
        final left = usbJustPluggedWindow - age;
        if (ends == null || left < ends) ends = left;
      }
    }
    final key = entry.device.memoryKey;
    rows.add(
      UsbPickerRow(
        entry,
        justPlugged: just,
        remembered: key.isNotEmpty && remembered.contains(usbMemoryHash(key)),
      ),
    );
  }
  // Just plugged in first, then the ones given to a VM before, then the rest;
  // newest first inside each group, ties by name.
  int rank(UsbPickerRow row) => row.justPlugged ? 0 : (row.remembered ? 1 : 2);
  rows.sort((a, b) {
    final byRank = rank(a).compareTo(rank(b));
    if (byRank != 0) return byRank;
    final byArrival = b.entry.device.arrivalMs.compareTo(a.entry.device.arrivalMs);
    if (byArrival != 0) return byArrival;
    final byName = a.entry.device.name.toLowerCase().compareTo(b.entry.device.name.toLowerCase());
    return byName != 0 ? byName : a.entry.device.id.compareTo(b.entry.device.id);
  });
  return UsbPicker(
    main: [for (final row in rows) if (_isMain(row.entry.kind)) row],
    other: [for (final row in rows) if (!_isMain(row.entry.kind)) row],
    hiddenNeeded: hiddenNeeded,
    badgeEnds: ends,
  );
}
