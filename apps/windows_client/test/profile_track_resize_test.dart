import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/track_stream.dart';

import 'track_stream_test.dart' as f;

TrackPlan nextPlan() => TrackPlan(const [
  TrackViewport('0', 800, 600, -800, 0),
  TrackViewport('1', 1280, 720, 0, 0),
]);
Map<String, dynamic> metadata(
  TrackPlan plan, {
  int topology = 1,
  int fps = 60,
  Map<String, int>? epochs,
  bool correlated = false,
}) => {
  ...f.info(),
  'topologyEpoch': topology,
  'fpsTarget': fps,
  if (correlated) ...{'requestId': 'resize-1', 'resizeId': 'relay-resize-1'},
  'tracks': [
    for (final t in plan.outputs)
      {
        'id': t.id,
        'epoch': epochs?[t.id] ?? 1,
        'width': t.width,
        'height': t.height,
        'sourceWidth': t.width,
        'sourceHeight': t.height,
        'left': t.x - plan.outputs.first.x,
        'top': t.y - plan.outputs.first.y,
        'primary': t.id == '0',
        'bitrateTarget': 6000000,
      },
  ],
};
Map<String, dynamic> stats(TrackStream s, {int first = 1, int second = 1}) => {
  ...f.trackStats(),
  'topologyEpoch': s.topology,
  'tracks': [
    for (final t in s.tracks)
      {
        'id': t.id,
        'epoch': t.epoch,
        'width': t.width,
        'height': t.height,
        'textureId': s.textures[t.id],
        'decodedFrames': t.id == '0' ? first : second,
      },
  ],
};
TrackStream ready() {
  final s = TrackStream('track-session', f.plan(), f.options, profile: true);
  s.configure(metadata(s.plan));
  s.bindTextures({'0': 90, '1': 91}, 0);
  for (final id in [0, 1]) {
    expect(s.accept(f.packet(id), 1), true);
  }
  expect(s.acknowledge(stats(s), 2), isNotNull);
  return s;
}

Map<String, dynamic> resizing(TrackStream s, {int fps = 60}) => {
  'type': 'stream.tracks.resizing',
  'requestId': 'resize-1',
  'resizeId': 'relay-resize-1',
  'topologyEpoch': 2,
  'fps': fps,
  'timeoutMs': 90000,
  'tracks': [
    for (final t in nextPlan().outputs)
      {...t.toJson(), 'epoch': t.id == '0' || fps != 60 ? 2 : 1},
  ],
};
Map<String, dynamic> restored() => {
  'type': 'stream.tracks.resize.restored',
  'sessionId': 'track-session',
  'requestId': 'resize-1',
  'resizeId': 'relay-resize-1',
  'topologyEpoch': 2,
  'fps': 60,
  'reason': 'unsupported_display_mode',
  'tracks': [
    for (final t in f.plan().outputs) {...t.toJson(), 'epoch': 1},
  ],
};
void begin(TrackStream s) {
  final value = s.requestResize(nextPlan(), 60, 'resize-1', 100);
  expect(value['topologyEpoch'], 1);
  expect(s.acceptResizing(resizing(s), 200), true);
}

void main() {
  final vectors = Platform.environment['MULTITAP_TRACK_VECTORS'];
  test(
    'Actual Microsoft CPU-encoded fixture bytes retain both topology generations through correlated group resize',
    () async {
      final root = Directory(vectors!);
      final manifest = jsonDecode(
        await File('${root.path}/manifest.json').readAsString(),
      ) as Map;
      expect(manifest['actualMicrosoftCpuEncoder'], true);
      final before = TrackPlan(const [
        TrackViewport('0', 640, 360, 0, 0),
        TrackViewport('1', 800, 600, 640, 0),
      ]);
      final after = TrackPlan(const [
        TrackViewport('0', 640, 360, 0, 0),
        TrackViewport('1', 960, 540, 640, 0),
      ]);
      final s = TrackStream('track-session', before, f.options, profile: true);
      var count = 0;
      for (final topology in [1, 2]) {
        if (topology == 2) {
          expect(
            s.acknowledge(stats(s), count),
            isNotNull,
          ); // Injected decoder evidence.
          s.requestResize(after, 60, 'resize-1', 100);
          s.acceptResizing({
            'type': 'stream.tracks.resizing',
            'requestId': 'resize-1',
            'resizeId': 'relay-resize-1',
            'topologyEpoch': 2,
            'fps': 60,
            'timeoutMs': 90000,
            'tracks': [
              for (final t in after.outputs)
                {...t.toJson(), 'epoch': t.id == '0' ? 1 : 2},
            ],
          }, 200);
        }
        s.configure(
          metadata(
            topology == 1 ? before : after,
            topology: topology,
            epochs: {'0': 1, '1': topology},
            correlated: topology == 2,
          ),
        );
        s.bindTextures({'0': 90, '1': 91}, count);
        for (final value in manifest['frames'] as List) {
          final frame = value as Map;
          if (frame['topologyEpoch'] != topology) continue;
          final bytes = await File('${root.path}/${frame['file']}')
              .readAsBytes();
          final preserved = Uint8List.fromList(bytes);
          expect(s.accept(bytes, ++count), true);
          expect(bytes, preserved);
        }
      }
      expect(count, 32);
      expect(s.topology, 2);
      expect(s.ready, false);
    },
    skip: vectors == null
        ? 'Set MULTITAP_TRACK_VECTORS to retained native CPU encoder vectors.'
        : false,
  );
  test('Resize retains applied state until correlated metadata, then gates both outputs on fresh IDRs and decoded frames', () {
    final s = ready();
    begin(s);
    expect(s.ready, false);
    expect(s.plan.matches(f.plan().toJson()), true);
    expect(s.input({'type': 'input', 'event': 'key'}), isNull);
    expect(s.accept(f.packet(0, pts: 2), 300), false);
    expect(s.acknowledge(stats(s), 300), isNull);
    expect(s.videoStalled(20000000), false);
    expect(s.configure(metadata(f.plan())), false);
    final changed = metadata(
      nextPlan(),
      topology: 2,
      epochs: {'0': 2, '1': 1},
      correlated: true,
    );
    expect(s.configure(changed), true);
    s.bindTextures({'0': 92, '1': 93}, 400);
    expect(s.topology, 2);
    expect(s.fps, 60);
    expect(s.ready, false);
    expect(s.accept(f.packet(0, topology: 1), 500), false);
    expect(s.accept(f.packet(0, topology: 2, epoch: 1), 500), false);
    expect(s.accept(f.packet(0, topology: 2, epoch: 2, pts: 0), 500), true);
    expect(s.acknowledge(stats(s, second: 0), 600), isNull);
    expect(s.accept(f.packet(1, topology: 2, pts: 0), 700), true);
    expect(s.acknowledge(stats(s, second: 0), 800), isNull);
    expect(s.acknowledge(stats(s), 900), {
      'type': 'stream.tracks.ready',
      'topologyEpoch': 2,
      'tracks': [
        {'id': '0', 'epoch': 2},
        {'id': '1', 'epoch': 1},
      ],
    });
    expect(s.ready, true);
    expect(s.configure(changed), false);
    expect(s.requestResize(f.plan(), 30, 'resize-2', 1000)['topologyEpoch'], 2);
  });
  test('Only exact old geometry may be restored under the new authorized topology; old timestamps reset only with that topology', () {
    final s = ready();
    begin(s);
    final deadline = s.preparationDeadlineUs;
    expect(s.acceptRestored(restored(), 300), true);
    expect(s.acceptRestored(restored(), 400), true);
    expect(s.preparationDeadlineUs, deadline);
    expect(s.restoreReason, 'unsupported_display_mode');
    expect(
      s.configure(metadata(f.plan(), topology: 2, correlated: true)),
      true,
    );
    s.bindTextures({'0': 92, '1': 93}, 500);
    expect(s.ready, false);
    expect(s.topology, 2);
    for (final id in [0, 1]) {
      expect(s.accept(f.packet(id, topology: 2, pts: 0), 600), true);
    }
    expect(s.acknowledge(stats(s), 700), isNotNull);
    expect(
      s.input({'type': 'input', 'event': 'key', 'topologyEpoch': 1}),
      isNull,
    );
    expect(s.plan.matches(f.plan().toJson()), true);
  });
  test('Mismatched restore and acknowledgements cannot adopt speculative geometry or mutate the pending request', () {
    for (final mutate in <Map<String, dynamic> Function(Map<String, dynamic>)>[
      (v) => {...v, 'extra': true},
      (v) => {...v, 'fps': 30},
      (v) => {...v, 'reason': 'arbitrary exception text'},
      (v) => {...v, 'tracks': resizing(ready())['tracks']},
      (v) => {
        ...v,
        'tracks': [
          {'id': null},
        ],
      },
    ]) {
      final s = ready();
      begin(s);
      expect(
        () => s.acceptRestored(mutate(restored()), 300),
        throwsFormatException,
      );
      expect(s.restoreReason, isNull);
      expect(s.topology, 1);
      expect(s.acceptRestored(restored(), 400), true);
    }
    for (final field in [
      'sessionId',
      'requestId',
      'resizeId',
      'topologyEpoch',
    ]) {
      final s = ready();
      begin(s);
      expect(
        s.acceptRestored({
          ...restored(),
          field: field == 'topologyEpoch' ? 1 : 'other',
        }, 300),
        false,
      );
      expect(s.restoreReason, isNull);
    }
    final s = ready();
    begin(s);
    expect(
      () => s.acceptRestored(restored(), s.preparationDeadlineUs! + 1),
      throwsFormatException,
    );
  });
  test('Undispatched rejection keeps the existing decoder; rejection after dispatch cannot claim unchanged', () {
    final rejection = {
      'type': 'stream.tracks.resize.failed',
      'requestId': 'resize-1',
      'reason': 'track_streaming_unavailable',
      'unchanged': true,
    };
    final s = ready();
    s.requestResize(nextPlan(), 60, 'resize-1', 100);
    expect(s.rejectUnchanged(rejection), true);
    expect(s.ready, true);
    expect(s.textures, {'0': 90, '1': 91});
    expect(s.accept(f.packet(0, pts: 2), 300), true);
    begin(s);
    expect(() => s.rejectUnchanged(rejection), throwsFormatException);
    expect(s.ready, false);
  });
  test('Negotiated profile requires owned zero as the primary at origin and rejects unannounced changes', () {
    for (final mutate in <Map<String, dynamic> Function(Map<String, dynamic>)>[
      (v) => {...v, 'topologyEpoch': 2},
      (v) => {...v, 'tracks': f.info()['tracks']},
      (v) => {...v, 'requestId': 'other', 'resizeId': 'other'},
    ]) {
      final s = TrackStream(
        'track-session',
        f.plan(),
        f.options,
        profile: true,
      );
      expect(
        () => s.configure(mutate(metadata(f.plan()))),
        throwsFormatException,
      );
    }
    final legacy = f.configured();
    expect(
      () => legacy.requestResize(nextPlan(), 60, 'resize-1', 100),
      throwsFormatException,
    );
    final s = ready();
    begin(s);
    expect(
      () => s.configure(metadata(nextPlan(), topology: 2, correlated: true)),
      throwsFormatException,
    );
    expect(s.topology, 1);
    expect(
      () => s.acceptResizing({...resizing(s), 'timeoutMs': 180000}, 300),
      throwsFormatException,
    );
  });
}
