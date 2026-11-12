// DTO and parser only: no broker socket, platform channel or display mutation.
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/data/video_geometry.dart';

Map<String, dynamic> _caps() => {
  'videoTrackProtocol': 'tracks-v1',
  'trackViewportProvisioning': true,
  'service': true,
  'interactiveSession': true,
  'streaming': true,
  'virtualDisplayReady': true,
  'virtualMonitorControl': true,
  // These VM-only fields can be incomplete while ordinary PC tracks are ready.
  'streamProfileProtocol': 'console-profile-v1',
  'streamProfileTracksProtocol': 'group-v1',
  'resizeProtocol': 'epoch-v1',
  'viewportProvisioning': false,
  'dynamicResolution': false,
  'monitors': [
    {'id': '0', 'width': 2560, 'height': 1440, 'primary': true},
    {'id': '1', 'width': 1920, 'height': 1080, 'primary': false},
  ],
};

BrokerDesktop _desktop(Map<String, dynamic> caps, {String kind = 'pc'}) =>
    BrokerDesktop.fromJson({
      'id': kind == 'pc'
          ? 'pc-11111111-1111-4111-8111-111111111111'
          : 'pve-149',
      'name': 'Synthetic fixture',
      'kind': kind,
      'localPcApproved': true,
      'description': '',
      'os': 'Windows',
      'status': 'ready',
      'canConnect': true,
      'connectionModes': {'stream': true},
      'host': {'connected': true, 'capabilities': caps},
      'display': {'supported': true, 'activeCount': 2, 'stale': false},
    });

bool _ordinary(Map<String, dynamic> caps) =>
    _desktop(caps).independentDisplayStreaming;

void main() {
  test('approved PC DTO uses ordinary tracks despite incomplete VM profile metadata', () {
    final caps = _caps();
    expect(profileTracksAdvertised(caps), isFalse);
    expect(
      independentTracksAdvertised(caps),
      isFalse,
      reason: 'default VM guard remains strict',
    );
    final desktop = _desktop(caps);
    expect(desktop.independentDisplayStreaming, isTrue);
    final pc = machineFromBroker(desktop);
    expect(pc.localPcApproved, isTrue);
    expect(pc.canConfigureDisplays, isTrue);
    expect(pc.independentDisplayStreaming, isTrue);
    expect(pc.profileTracks, isFalse);
    expect(pc.requiresProfileTracks, isFalse);
    final vm = machineFromBroker(_desktop(caps, kind: 'vm'));
    expect(vm.independentDisplayStreaming, isFalse);
    expect(vm.profileTracks, isFalse);
  });

  test('complete VM profile remains valid while PC never requests that transaction', () {
    final caps = _caps()
      ..addAll({'viewportProvisioning': true, 'dynamicResolution': true});
    final pc = machineFromBroker(_desktop(caps));
    final vm = machineFromBroker(_desktop(caps, kind: 'vm'));
    expect(pc.independentDisplayStreaming, isTrue);
    expect(
      pc.profileTracks,
      isTrue,
      reason: 'raw advertised observation is retained',
    );
    expect(pc.requiresProfileTracks, isFalse);
    expect(vm.independentDisplayStreaming, isTrue);
    expect(vm.requiresProfileTracks, isTrue);
  });

  test('ordinary PC does not depend on the presence of any VM profile marker', () {
    final caps = _caps()
      ..remove('streamProfileProtocol')
      ..remove('streamProfileTracksProtocol')
      ..remove('resizeProtocol')
      ..remove('viewportProvisioning')
      ..remove('dynamicResolution');
    expect(_ordinary(caps), isTrue);
    expect(machineFromBroker(_desktop(caps)).requiresProfileTracks, isFalse);
    // Preserve the existing default helper contract rather than broadening it.
    expect(
      independentTracksAdvertised({
        'videoTrackProtocol': 'tracks-v1',
        'trackViewportProvisioning': true,
      }),
      isTrue,
    );
  });

  for (final key in [
    'trackViewportProvisioning',
    'service',
    'interactiveSession',
    'streaming',
    'virtualDisplayReady',
  ]) {
    test('PC requires actual basic readiness: $key false or missing', () {
      for (final missing in [false, true]) {
        final caps = _caps();
        if (missing) {
          caps.remove(key);
        } else {
          caps[key] = false;
        }
        expect(_ordinary(caps), isFalse);
        expect(_desktop(caps).independentDisplayStreaming, isFalse);
      }
    });
  }

  test(
    'PC requires the exact protocol and boolean readiness, not truthy values',
    () {
      for (final protocol in [null, 'tracks-v2', 'media-v1']) {
        expect(_ordinary(_caps()..['videoTrackProtocol'] = protocol), isFalse);
      }
      for (final value in [null, 1, 'true']) {
        expect(_ordinary(_caps()..['service'] = value), isFalse);
      }
    },
  );

  test(
    'PC rejects ambiguous monitor identity and unsupported source geometry',
    () {
      final good = _caps()['monitors'] as List;
      final first = Map<String, dynamic>.from(good.first as Map);
      final invalid = <Object?>[
        null,
        [],
        [first, first],
        [
          {...first, 'id': '1'},
        ],
        [
          {...first, 'id': '2'},
        ],
        [
          {...first, 'id': 0},
        ],
        [first, good.last, good.last],
        [
          {...first, 'width': 319},
        ],
        [
          {...first, 'height': 179},
        ],
        [
          {...first, 'width': 2561},
        ],
        [
          {...first, 'height': 1081},
        ],
        [
          {...first, 'width': 2560.0},
        ],
        [
          {...first, 'width': maximumGeometrySide + 2},
        ],
        [
          {
            ...first,
            'width': maximumGeometrySide,
            'height': maximumGeometrySide,
          },
        ],
      ];
      for (final monitors in invalid) {
        expect(
          _ordinary(_caps()..['monitors'] = monitors),
          isFalse,
          reason: '$monitors',
        );
      }
      expect(
        _ordinary(
          _caps()
            ..['monitors'] = [
              {...first, 'width': 5120, 'height': 2880},
            ],
        ),
        isTrue,
      );
    },
  );
}
