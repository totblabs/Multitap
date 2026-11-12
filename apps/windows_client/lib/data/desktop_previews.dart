import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/painting.dart';

import '../models/desktop_machine.dart';
import 'desktop_thumbnails.dart';
import '../l10n/l10n.dart';

// desktop-preview-v1: a recent
// picture of each assigned desktop, taken by its Host when the broker is asked
// (GET /api/v1/desktops/:id/preview), so a card shows the VM before the first
// connection. The launcher asks only while it is visible, about once a
// minute, and only for desktops whose list entry carries previewProtocol.
//
// The picture stays in this process's memory (never on disk or in
// diagnostics), is shown only while the desktop's current list entry still
// carries the same allowed thumbnail-privacy epoch it was fetched under, and
// leaves with the account (sign-out, another server or account).
const desktopPreviewProtocol = 'desktop-preview-v1';

/// How often a visible launcher asks again. The broker asks each Host at
/// most every 30 s whatever clients do.
const desktopPreviewInterval = Duration(seconds: 60);

/// A list change (a new desktop, a changed Windows session) asks one desktop
/// again at most this often.
const desktopPreviewNudge = Duration(seconds: 15);

/// The largest picture accepted (the broker's own bound).
const desktopPreviewMaxBytes = 48 * 1024;

enum DesktopPreviewState { captured, locked, signin, asleep, unavailable }

/// One answer of the broker.
@immutable
class DesktopPreview {
  const DesktopPreview(this.state, {this.picture, this.epoch});
  const DesktopPreview.unavailable()
    : state = DesktopPreviewState.unavailable,
      picture = null,
      epoch = null;
  final DesktopPreviewState state;

  /// Only for [DesktopPreviewState.captured].
  final DesktopThumbnail? picture;

  /// The thumbnail-privacy epoch the list carried when this was asked for.
  final String? epoch;

  DesktopPreview withEpoch(String? value) =>
      DesktopPreview(state, picture: picture, epoch: value);

  /// A strict reading; anything unexpected is [DesktopPreviewState.unavailable]
  /// (the card keeps today's text), never a picture.
  static DesktopPreview parse(Object? value, {DateTime? now}) {
    if (value is! Map || value['protocol'] != desktopPreviewProtocol) {
      return const DesktopPreview.unavailable();
    }
    final state = switch (value['state']) {
      'captured' => DesktopPreviewState.captured,
      'locked' => DesktopPreviewState.locked,
      'signin' => DesktopPreviewState.signin,
      'asleep' => DesktopPreviewState.asleep,
      _ => DesktopPreviewState.unavailable,
    };
    if (state != DesktopPreviewState.captured) return DesktopPreview(state);
    final image = value['image'];
    if (image is! String ||
        image.isEmpty ||
        image.length > (desktopPreviewMaxBytes + 2) ~/ 3 * 4) {
      return const DesktopPreview.unavailable();
    }
    final Uint8List bytes;
    try {
      bytes = base64Decode(image);
    } on FormatException {
      return const DesktopPreview.unavailable();
    }
    if (bytes.length < 4 ||
        bytes.length > desktopPreviewMaxBytes ||
        bytes[0] != 0xff ||
        bytes[1] != 0xd8 ||
        bytes[bytes.length - 2] != 0xff ||
        bytes.last != 0xd9) {
      return const DesktopPreview.unavailable();
    }
    // The broker's own age, so a skewed PC clock cannot misstate it; the
    // timestamp only when no age came (older broker builds never send one).
    final clock = now ?? DateTime.now();
    final age = value['ageMs'];
    final DateTime taken;
    if (age is int && age >= 0 && age <= const Duration(hours: 1).inMilliseconds) {
      taken = clock.subtract(Duration(milliseconds: age));
    } else {
      final stamp = value['takenAt'] is String
          ? DateTime.tryParse(value['takenAt'] as String)
          : null;
      if (stamp == null || !stamp.isUtc) {
        return const DesktopPreview.unavailable();
      }
      taken = stamp.isAfter(clock) ? clock : stamp.toLocal();
    }
    return DesktopPreview(
      state,
      picture: DesktopThumbnail(
        image: MemoryImage(bytes),
        modified: taken,
        remote: true,
      ),
    );
  }
}

/// What the card says instead of a picture; null keeps today's text.
String? desktopPreviewNote(DesktopPreviewState? state) => switch (state) {
  DesktopPreviewState.locked => l10n.locked,
  DesktopPreviewState.signin => l10n.signScreen,
  DesktopPreviewState.asleep => l10n.screenOff,
  _ => null,
};

/// The signed-in account's broker pictures.
abstract interface class DesktopPreviewSource {
  /// The current list offers a picture of this desktop.
  bool offered(String desktopId);
  Future<DesktopPreview> read(String desktopId);
}

/// The broker side of [DesktopPreviewSource] (HttpBrokerApi implements it).
abstract interface class DesktopPreviewBrokerApi {
  bool previewOffered(String token, String desktopId);
  Future<DesktopPreview> desktopPreview(String token, String desktopId);
}

/// One account's source: bound to its token, so a new sign-in gets a new one.
class BrokerDesktopPreviews implements DesktopPreviewSource {
  BrokerDesktopPreviews(this.api, this.token);
  final DesktopPreviewBrokerApi api;
  final String token;

  @override
  bool offered(String desktopId) => api.previewOffered(token, desktopId);

  @override
  Future<DesktopPreview> read(String desktopId) =>
      api.desktopPreview(token, desktopId);
}

/// One launcher's broker pictures (the Windows and Android launchers share
/// it). While `visible` says the launcher is on screen it asks at [start],
/// about every [desktopPreviewInterval], on [poll] (back on screen) and, from
/// [listed], at once for a new desktop or a changed Windows session (at most
/// every [desktopPreviewNudge] per desktop). Only desktops the list offers are
/// asked, four at a time. The answers stay in memory and leave at [stop].
class DesktopPreviewBoard {
  DesktopPreviewBoard({required this.changed, DateTime Function()? clock})
    : _clock = clock ?? DateTime.now;

  /// A picture or note changed; the owner redraws.
  final VoidCallback changed;
  final DateTime Function() _clock;

  final _previews = <String, DesktopPreview>{};
  // When each desktop was last asked.
  final _asked = <String, DateTime>{};
  DesktopPreviewSource? _source;
  Iterable<DesktopMachine> Function() _machines = _none;
  bool Function() _visible = _hidden;
  Timer? _timer;
  int _generation = 0;
  int? _polling;

  static Iterable<DesktopMachine> _none() => const [];
  static bool _hidden() => false;

  /// Binds one account's [source] (anything held before leaves first) and
  /// asks at once if [visible].
  void start(
    DesktopPreviewSource source, {
    required Iterable<DesktopMachine> Function() machines,
    required bool Function() visible,
  }) {
    stop();
    _source = source;
    _machines = machines;
    _visible = visible;
    _timer = Timer.periodic(desktopPreviewInterval, (_) => unawaited(poll()));
    unawaited(poll());
  }

  /// Sign-out, another account or server, pictures turned off: nothing more
  /// is asked and every picture leaves memory (an answer still on its way
  /// is dropped).
  void stop() {
    _generation++;
    _polling = null;
    _timer?.cancel();
    _timer = null;
    _source = null;
    _machines = _none;
    _visible = _hidden;
    for (final value in _previews.values) {
      _evict(value);
    }
    _previews.clear();
    _asked.clear();
  }

  static void _evict(DesktopPreview? value) {
    final image = value?.picture?.image;
    if (image != null) unawaited(image.evict().catchError((Object _) => false));
  }

  void _remove(String id) => _evict(_previews.remove(id));

  Map<String, String?> _epochs() => {
    for (final machine in _machines())
      machine.id: machine.thumbnailPrivacy?.epoch,
  };

  /// Asks the broker about the offered desktops; [only] limits it to those.
  /// Not on screen: nothing (the Host captures nothing).
  Future<void> poll({Set<String>? only}) async {
    final source = _source;
    if (source == null || !_visible() || (_polling != null && only == null)) {
      return;
    }
    final generation = _generation;
    if (only == null) _polling = generation;
    try {
      final ids = [
        for (final id in _epochs().keys)
          if (source.offered(id) && (only == null || only.contains(id))) id,
      ];
      final asked = _clock();
      for (final id in ids) {
        _asked[id] = asked;
      }
      for (var at = 0; at < ids.length; at += 4) {
        final batch = ids.skip(at).take(4).toList();
        // The session each answer belongs to: the one listed when asked.
        final epochs = _epochs();
        final answers = await Future.wait([
          for (final id in batch)
            source
                .read(id)
                .then<DesktopPreview?>((value) => value)
                .catchError((Object _) => null),
        ]);
        if (generation != _generation) {
          answers.forEach(_evict);
          return;
        }
        final listed = _epochs();
        for (var i = 0; i < batch.length; i++) {
          final id = batch[i], value = answers[i];
          _remove(id);
          if (value != null && source.offered(id) && listed.containsKey(id)) {
            _previews[id] = value.withEpoch(epochs[id]);
          } else {
            _evict(value);
          }
        }
        changed();
      }
    } finally {
      if (_polling == generation && only == null) _polling = null;
    }
  }

  /// The list was read again. A desktop gone from it (or no longer offered)
  /// loses its picture; with [ask], a new one or one whose Windows session
  /// changed (lock, sign-out, switch) is asked at once.
  void listed(Iterable<DesktopMachine> machines, {bool ask = true}) {
    final source = _source;
    if (source == null) return;
    final epochs = {
      for (final machine in machines)
        machine.id: machine.thumbnailPrivacy?.epoch,
    };
    final now = _clock(), again = <String>{};
    bool due(String id) =>
        now.difference(_asked[id] ?? DateTime(0)) >= desktopPreviewNudge;
    // A desktop gone from the list keeps its time only while that still
    // spaces a quick return; otherwise a long launcher run keeps every VM.
    _asked.removeWhere((id, _) => !epochs.containsKey(id) && due(id));
    for (final id in _previews.keys.toList()) {
      if (!epochs.containsKey(id) || !source.offered(id)) {
        _remove(id);
      } else if (_previews[id]!.epoch != epochs[id] && due(id)) {
        again.add(id);
      }
    }
    for (final id in epochs.keys) {
      if (!_previews.containsKey(id) && source.offered(id) && due(id)) {
        again.add(id);
      }
    }
    if (ask && again.isNotEmpty) unawaited(poll(only: again));
  }

  /// The broker's answer while the list still shows the Windows session it
  /// was asked under (a lock, sign-out or user switch moves the epoch).
  DesktopPreview? current(DesktopMachine machine) {
    final value = _previews[machine.id], privacy = machine.thumbnailPrivacy;
    if (value == null || privacy == null || value.epoch != privacy.epoch) {
      return null;
    }
    if (value.picture != null && !privacy.allowed) return null;
    return value;
  }

  /// The newer of this device's last streamed frame ([local]) and the
  /// Host's picture.
  DesktopThumbnail? pictureFor(
    DesktopMachine machine, {
    DesktopThumbnail? local,
  }) {
    final remote = current(machine)?.picture;
    if (remote == null) return local;
    if (local == null) return remote;
    return remote.modified.isAfter(local.modified) ? remote : local;
  }

  /// Why there is no picture (잠겨 있어요 …); null keeps the card's own text.
  String? noteFor(DesktopMachine machine, {DesktopThumbnail? local}) =>
      pictureFor(machine, local: local) != null
      ? null
      : desktopPreviewNote(current(machine)?.state);
}
