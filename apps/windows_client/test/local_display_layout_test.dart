import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/local_display_layout.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/models/stream_options.dart';

Map<Object?, Object?> physicalLayout() => {
  'x': -1920,
  'y': 0,
  'width': 4480,
  'height': 1440,
  'monitors': [
    {
      'id': '0',
      'x': 0,
      'y': 0,
      'width': 2560,
      'height': 1440,
      'primary': true,
      'dpi': 144,
    },
    {
      'id': '1',
      'x': -1920,
      'y': 0,
      'width': 1920,
      'height': 1080,
      'primary': false,
      'dpi': 96,
    },
  ],
};

void main() {
  test('mixed DPI physical monitors retain independent requested pixels and relative placement', () {
    final layout = LocalDisplayLayout.fromMap(physicalLayout());
    expect(layout.plan.toJson(), [
      {'id': '0', 'width': 2560, 'height': 1440, 'x': 0, 'y': 0},
      {'id': '1', 'width': 1920, 'height': 1080, 'x': -1920, 'y': 0},
    ]);
    expect(layout.surfaces.map((s) => s.dpi), [144, 96]);
    expect(
      layout.options(const StreamOptions(fps: 30, audio: false)).toJson(),
      {
        'maxWidth': 2560,
        'maxHeight': 1440,
        'fps': 30,
        'bitrate': 20000000,
        'audio': false,
        'monitor': 'all',
      },
    );
    expect(
      layout.matchesViewport(const Size(4480 / 1.5, 1440 / 1.5), 1.5),
      isTrue,
    );
    expect(layout.matchesViewport(const Size(1000, 800), 1.5), isFalse);
  });

  test(
    'one/three monitors, stale bounds, duplicate primary and ABI overflow fail',
    () {
      final bad = <Map<Object?, Object?>>[];
      for (final count in [1, 3]) {
        bad.add({
          ...physicalLayout(),
          'monitors': List.filled(
            count,
            (physicalLayout()['monitors'] as List).first,
          ),
        });
      }
      bad.add({...physicalLayout(), 'width': 4481});
      bad.add({
        ...physicalLayout(),
        'monitors': [
          (physicalLayout()['monitors'] as List).first,
          {
            ...(physicalLayout()['monitors'] as List).last as Map,
            'primary': true,
          },
        ],
      });
      bad.add({
        ...physicalLayout(),
        'monitors': [
          {
            ...(physicalLayout()['monitors'] as List).first as Map,
            'width': 0x80000000,
          },
          (physicalLayout()['monitors'] as List).last,
        ],
      });
      for (final value in bad) {
        expect(() => LocalDisplayLayout.fromMap(value), throwsFormatException);
      }
    },
  );

  test('v2 dual 4K, 5K, portrait and ultrawide geometry preserves each source without granting a decoder capability', () {
    for (final (width, height) in [
      (3840, 2160),
      (5120, 2880),
      (2880, 5120),
      (5120, 1440),
    ]) {
      final layout = LocalDisplayLayout.fromMap({
        'x': 0,
        'y': 0,
        'width': width * 2,
        'height': height,
        'monitors': [
          for (var id = 0; id < 2; id++)
            {
              'id': '$id',
              'x': id * width,
              'y': 0,
              'width': width,
              'height': height,
              'primary': id == 0,
              'dpi': id == 0 ? 144 : 96,
            },
        ],
      });
      expect(
        layout.plan.outputs.map((output) => (output.width, output.height)),
        [(width, height), (width, height)],
      );
      expect(
        layout.plan.legacyCompatible,
        isFalse,
        reason: 'the old v1 aggregate/portrait bounds remain explicit',
      );
      expect(
        layout.pointAt(Offset(width * 1.5 / 1.5, height * .5 / 1.5), 1.5),
        (track: '1', x: .5, y: .5),
      );
    }
  });

  test('pointer routing resolves negative-origin mixed-DPI outputs and rejects union gaps', () {
    final layout = LocalDisplayLayout.fromMap(physicalLayout());
    final secondary = layout.pointAt(const Offset(960 / 1.5, 540 / 1.5), 1.5);
    expect(secondary, (track: '1', x: .5, y: .5));
    final primary = layout.pointAt(const Offset(3200 / 1.5, 720 / 1.5), 1.5);
    expect(primary, (track: '0', x: .5, y: .5));
    // The exact common edge belongs to the display starting there, not a
    // clamped point on both surfaces. The shorter display leaves an empty gap.
    expect(layout.pointAt(const Offset(1920 / 1.5, 720 / 1.5), 1.5), (
      track: '0',
      x: 0.0,
      y: .5,
    ));
    expect(layout.pointAt(const Offset(960 / 1.5, 1200 / 1.5), 1.5), isNull);
    expect(layout.pointAt(const Offset(-1, 100), 1.5), isNull);
    expect(layout.pointAt(const Offset(4480 / 1.5, 100), 1.5), isNull);
    expect(layout.pointAt(const Offset(100, 100), 0), isNull);
    expect(layout.pointAt(const Offset(double.nan, 100), 1.5), isNull);
  });
  test('current source proof ignores encoded scale and host-global origin, but requires each physical output arrangement', () {
    final layout = LocalDisplayLayout.fromMap(physicalLayout());
    List<StreamTrack> sources({
      int offsetX = 640,
      int offsetY = -360,
      bool shifted = false,
      bool scaled = false,
    }) => [
      StreamTrack(
        id: '0',
        epoch: 4,
        width: 2560,
        height: 1440,
        left: offsetX,
        top: offsetY,
        primary: true,
        bitrate: 10000000,
        encodedWidth: scaled ? 1280 : 2560,
        encodedHeight: scaled ? 720 : 1440,
      ),
      StreamTrack(
        id: '1',
        epoch: 8,
        width: 1920,
        height: 1080,
        left: offsetX + (shifted ? 2560 : -1920),
        top: offsetY,
        primary: false,
        bitrate: 10000000,
        encodedWidth: scaled ? 960 : 1920,
        encodedHeight: scaled ? 540 : 1080,
      ),
    ];
    expect(layout.matchesSources(sources()), isTrue);
    expect(
      layout.matchesSources(sources(offsetX: -7000, offsetY: 1200)),
      isTrue,
    );
    expect(
      layout.matchesSources(sources(scaled: true)),
      isTrue,
      reason: 'encoded picture scale must not change source mouse speed or placement',
    );
    expect(
      layout.matchesSources(sources(shifted: true)),
      isFalse,
      reason: 'a different monitor side is not the same pointer mapping',
    );
    expect(
      layout.matchesSources(sources().reversed.toList()),
      isTrue,
      reason: 'match actual output IDs rather than list order',
    );
    expect(layout.matchesSources([sources().first]), isFalse);
    expect(layout.matchesSources([sources().first, sources().first]), isFalse);
  });

  test('DPI reobservation can preserve source coordinates while a same-union rearrangement cannot', () {
    final original = LocalDisplayLayout.fromMap(physicalLayout());
    final changedDpi = LocalDisplayLayout.fromMap({
      ...physicalLayout(),
      'monitors': [
        for (final monitor in physicalLayout()['monitors'] as List)
          {...monitor as Map, 'dpi': monitor['id'] == '0' ? 192 : 120},
      ],
    });
    final tracks = [
      const StreamTrack(
        id: '0',
        epoch: 2,
        width: 2560,
        height: 1440,
        left: 0,
        top: 0,
        primary: true,
        bitrate: 10000000,
      ),
      const StreamTrack(
        id: '1',
        epoch: 2,
        width: 1920,
        height: 1080,
        left: -1920,
        top: 0,
        primary: false,
        bitrate: 10000000,
      ),
    ];
    expect(original.matchesSources(tracks), isTrue);
    expect(changedDpi.matchesSources(tracks), isTrue);
    expect(changedDpi.matchesViewport(const Size(2240, 720), 2), isTrue);
    expect(
      changedDpi.matchesViewport(const Size(2240, 720), 1.5),
      isFalse,
      reason: 'stale viewport/DPR cannot authorize input',
    );
    expect(
      changedDpi.pointAt(const Offset(1600, 360), 2),
      original.pointAt(const Offset(3200 / 1.5, 720 / 1.5), 1.5),
    );
    // Same aggregate size is not a proof that a secondary display stayed on
    // the same side of the primary output.
    final moved = LocalDisplayLayout.fromMap({
      'x': 0,
      'y': 0,
      'width': 4480,
      'height': 1440,
      'monitors': [
        {
          'id': '0',
          'x': 0,
          'y': 0,
          'width': 2560,
          'height': 1440,
          'primary': true,
          'dpi': 144,
        },
        {
          'id': '1',
          'x': 2560,
          'y': 0,
          'width': 1920,
          'height': 1080,
          'primary': false,
          'dpi': 96,
        },
      ],
    });
    expect(moved.bounds.size, original.bounds.size);
    expect(moved.matchesSources(tracks), isFalse);
  });
}
