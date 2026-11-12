// udp-media-v1 on the client, Dart's side: the grant, the capability gate of the
// connect request, and the control flow around the native leg with a fake leg.
// The schedule, the framing and the sockets moved to the leg and are tested in
// test/native/udp_media_leg_test.cpp. Nothing UDP does may reach the session.
import 'dart:async';
import 'dart:convert';

import 'package:fake_async/fake_async.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_stats_report.dart';
import 'package:multitap_client/data/track_stream.dart'
    show profileTracksProtocol;
import 'package:multitap_client/data/udp_probe.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'initial_viewport_test.dart' as viewport;
import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, flush, info, packet;
import 'track_stream_test.dart' as tracks;
import 'viewport_session_test.dart' show applied, resizeAck, startInfo;

const _ticket = 'AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw=';
const _id = 'obLD1OX2Bxg=';
const _hostA = '192.0.2.1', _hostB = '192.0.2.2', _relayPort = 18481;

Map<String, dynamic> _event({List<Object?>? relays}) => {
  'type': 'udp.grant',
  'protocol': 'udp-media-v1',
  'relays':
      relays ??
      [
        {'host': _hostA, 'port': _relayPort},
      ],
  'id': _id,
  'ticket': _ticket,
  'expiresAt': 1793520000000,
};

const _bothRelays = [
  {'host': _hostA, 'port': _relayPort},
  {'host': _hostB, 'port': _relayPort},
];

void main() {
  test('direct grant is opt-in; candidates are session-bound and local reporting is once', () {
    expect(UdpGrant.parse(_event())!.direct, false);
    expect(
      UdpGrant.parse({..._event(), 'directProtocol': udpDirectProtocol})!
          .direct,
      false,
    );
    fakeAsync((time) {
      final leg = _FakeLeg()
        ..reading = const UdpLegReading(local: ('192.168.1.2', 1234));
      final local = <(String, int)>[];
      final udp = UdpMedia(
        UdpGrant.parse({
          ..._event(),
          'keys': true,
          'directProtocol': udpDirectProtocol,
        })!,
        leg,
        sessionId: 'direct-fixture',
        nowUs: () => 1,
        mediaAllowed: true,
        sendKey: (_) {},
        delivered: (_) {},
        keyed: () {},
        sendLocal: (a, p) => local.add((a, p)),
      );
      udp.start(0);
      time.elapse(const Duration(milliseconds: 300));
      time.flushMicrotasks();
      expect(local, [('192.168.1.2', 1234)]);
      final peer = {
        'protocol': udpDirectProtocol,
        'sessionId': 'direct-fixture',
        'candidates': [
          {'address': '192.0.2.1', 'port': 4321},
        ],
      };
      udp.peer({...peer, 'sessionId': 'another'});
      udp.peer({
        ...peer,
        'candidates': [
          {'address': 'example.com', 'port': 4321},
        ],
      });
      time.flushMicrotasks();
      expect(leg.peers, isEmpty);
      udp.peer(peer);
      time.flushMicrotasks();
      expect(leg.peers, [
        [('192.0.2.1', 4321)],
      ]);

      final wrapped = udp.wrapInput({
        'type': 'input',
        'event': 'key',
        'vk': 65,
        'scan': 30,
        'down': true,
        'extended': false,
      }, 2)!;
      expect(wrapped['sequence'], 1);
      expect(wrapped['epoch'], 2);
      udp.fastInput(wrapped);
      time.flushMicrotasks();
      expect(leg.inputs.length, 1);
      expect(leg.inputs.single.$1, 1);
      expect(utf8.decode(leg.inputs.single.$3), contains('"event":"key"'));
      final reset = udp.wrapInput({'type': 'input.reset'}, 2)!;
      expect(reset['sequence'], 2);
      final fence = udp.wrapInput({'type': 'input.fence'}, 2)!;
      udp.fastInput(fence);
      final text = udp.wrapInput({
        'type': 'input',
        'event': 'text',
        'text': '한글',
      }, 2)!;
      udp.fastInput(text);
      time.flushMicrotasks();
      expect(leg.inputs.length, 1, reason: 'fences and text stay on WSS');
      expect(
        udp.wrapInput({
          'type': 'input',
          'event': 'text',
          'text': '한' * 3000,
        }, 2),
        isNull,
      );
      final relative = udp.wrapInput({
        'type': 'input',
        'event': 'moveRelative',
        'dx': 3,
        'dy': -2,
      }, 2)!;
      udp.fastInput(relative);
      time.flushMicrotasks();
      expect(leg.inputs.last.$1, relative['sequence']);
      udp.close();
      time.flushMicrotasks();
    });
  });

  group('grant', () {
    test('the contract\'s grant parses', () {
      final grant = UdpGrant.parse(_event(relays: _bothRelays))!;
      expect(grant.relays, [(_hostA, _relayPort), (_hostB, _relayPort)]);
      expect(grant.id, base64.decode(_id));
      expect(grant.ticket, base64.decode(_ticket));
      expect(
        UdpGrant.parse({
          ..._event(),
          'relays': [
            {'host': 'vdi.example.com', 'port': 1},
            {'host': 'a' * 253, 'port': 65535},
          ],
        }),
        isNotNull,
      );
    });

    test('anything else is ignored', () {
      final relay = {'host': _hostA, 'port': _relayPort};
      for (final (name, bad) in <(String, Map<String, dynamic>)>[
        ('protocol', {..._event(), 'protocol': 'udp-media-v2'}),
        ('no protocol', {..._event()}..remove('protocol')),
        ('short id', {..._event(), 'id': base64.encode(List.filled(7, 1))}),
        ('long id', {..._event(), 'id': base64.encode(List.filled(9, 1))}),
        ('id type', {..._event(), 'id': 5}),
        ('id url alphabet', {..._event(), 'id': '-_-_-_-_-_8='}),
        ('id unpadded', {..._event(), 'id': 'obLD1OX2Bxg'}),
        (
          'short ticket',
          {..._event(), 'ticket': base64.encode(List.filled(31, 1))},
        ),
        (
          'long ticket',
          {..._event(), 'ticket': base64.encode(List.filled(33, 1))},
        ),
        ('ticket text', {..._event(), 'ticket': 'not base64!'}),
        ('no ticket', {..._event()}..remove('ticket')),
        ('no expiry', {..._event()}..remove('expiresAt')),
        ('expiry type', {..._event(), 'expiresAt': '1793520000000'}),
        ('expiry value', {..._event(), 'expiresAt': 0}),
        ('no relays', _event(relays: [])),
        ('relays type', {..._event(), 'relays': 'host'}),
        ('nine relays', _event(relays: List.filled(9, relay))),
        ('relay type', _event(relays: ['host'])),
        (
          'empty host',
          _event(
            relays: [
              {'host': '', 'port': 1},
            ],
          ),
        ),
        (
          'long host',
          _event(
            relays: [
              {'host': 'a' * 254, 'port': 1},
            ],
          ),
        ),
        (
          'host text',
          _event(
            relays: [
              {'host': 'a b', 'port': 1},
            ],
          ),
        ),
        (
          'host newline',
          _event(
            relays: [
              {'host': 'a\n', 'port': 1},
            ],
          ),
        ),
        (
          'host type',
          _event(
            relays: [
              {'host': 5, 'port': 1},
            ],
          ),
        ),
        (
          'port zero',
          _event(
            relays: [
              {'host': _hostA, 'port': 0},
            ],
          ),
        ),
        (
          'port high',
          _event(
            relays: [
              {'host': _hostA, 'port': 65536},
            ],
          ),
        ),
        (
          'port type',
          _event(
            relays: [
              {'host': _hostA, 'port': '1'},
            ],
          ),
        ),
        (
          'one bad of two',
          _event(
            relays: [
              relay,
              {'host': _hostA, 'port': 0},
            ],
          ),
        ),
      ]) {
        expect(UdpGrant.parse(bad), isNull, reason: name);
      }
    });
  });

  group('connect request', () {
    Map<String, dynamic> desktop(Map<String, Object> capabilities) => {
      'id': 'pve-149',
      'name': 'TESTER',
      'kind': 'vm',
      'description': '',
      'os': 'Windows',
      'status': 'ready',
      'canConnect': true,
      'connectionModes': {'stream': true},
      'host': {'connected': true, 'capabilities': capabilities},
    };
    final ordinary = {
      'sessionId': 'udp-session',
      'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/udp-session',
      'mode': 'stream',
      'options': const StreamOptions().toJson(),
    };
    const advertised = {
      'service': true,
      'udpMediaProtocol': 'udp-media-v1',
      'udpTracksProtocol': 'udp-tracks-v1',
    };
    const absent = {'service': true};

    Future<(Map<dynamic, dynamic>, BrokerConnection)> connect(
      Map<String, Object> capabilities,
      String origin,
      Map<String, dynamic> reply,
      Future<BrokerConnection> Function(HttpBrokerApi) open, {
      bool requestUdpMedia = true,
    }) async {
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [desktop(capabilities)],
          },
        ))
        ..responses.add((200, reply));
      final api = HttpBrokerApi(
        origin: Uri.parse(origin),
        client: client,
        requestUdpMedia: requestUdpMedia,
      );
      await api.desktops('fixture');
      final connection = await open(api);
      return (
        jsonDecode(client.requests.last.body.toString()) as Map,
        connection,
      );
    }

    final paths =
        <
          String,
          (
            String,
            Map<String, dynamic> Function(),
            Future<BrokerConnection> Function(HttpBrokerApi),
          )
        >{
          'ordinary stream': (
            'https://fixture.invalid',
            () => ordinary,
            (api) => api.connect('fixture', 'pve-149'),
          ),
          'initial viewport': (
            'https://fixture.invalid',
            viewport.response,
            (api) => api.connectViewport(
              'fixture',
              'pve-149',
              viewport.target(),
              viewport.requestedOptions,
            ),
          ),
          'independent outputs': (
            'https://cloud.example.com',
            tracks.response,
            (api) => api.connectTracks(
              'fixture',
              'pve-149',
              tracks.plan(),
              tracks.options,
            ),
          ),
          'profile outputs': (
            'https://cloud.example.com',
            () => {
              ...tracks.response(),
              'streamProfileTracksProtocol': profileTracksProtocol,
            },
            (api) => api.connectProfileTracks(
              'fixture',
              'pve-149',
              tracks.plan(),
              tracks.options,
            ),
          ),
        };

    for (final MapEntry(key: name, value: (origin, reply, open))
        in paths.entries) {
      test('$name: the capability absent, the key is absent', () async {
        final (body, connection) = await connect(absent, origin, reply(), open);
        expect(body.containsKey('udpMediaProtocol'), isFalse);
        expect(connection.udpMedia, isFalse);
      });

      test(
        '$name: the capability present on Windows, the key is sent',
        () async {
          final (body, connection) = await connect(
            advertised,
            origin,
            reply(),
            open,
          );
          expect(body['udpMediaProtocol'], 'udp-media-v1');
          expect(connection.udpMedia, isTrue);
        },
      );

      test(
        '$name: a client that disables UDP never asks',
        () async {
          final (body, connection) = await connect(
            advertised,
            origin,
            reply(),
            open,
            requestUdpMedia: false,
          );
          expect(body.containsKey('udpMediaProtocol'), isFalse);
          expect(connection.udpMedia, isFalse);
        },
      );
      test('$name: only a broker-translated direct marker opts in', () async {
        for (final (marker, expected) in [
          ('udpDirectHostProtocol', false),
          ('udpDirectProtocol', true),
        ]) {
          final (body, _) = await connect(
            {...advertised, marker: udpDirectProtocol},
            origin,
            reply(),
            open,
          );
          expect(body.containsKey('udpDirectProtocol'), expected);
        }
      });
    }

    test('only the exact capability value counts', () {
      expect(udpMediaAdvertised({'udpMediaProtocol': 'udp-media-v1'}), isTrue);
      for (final value in <Object?>[
        null,
        'caps',
        const <String, Object>{},
        const {'udpMediaProtocol': 'udp-media-v2'},
        const {'udpMediaProtocol': true},
      ]) {
        expect(udpMediaAdvertised(value), isFalse, reason: '$value');
      }
      expect(BrokerDesktop.fromJson(desktop(advertised)).udpMedia, isTrue);
      expect(BrokerDesktop.fromJson(desktop(absent)).udpMedia, isFalse);
    });

    test('the capability is per desktop', () async {
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [desktop(advertised)],
          },
        ))
        ..responses.add((200, ordinary));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://fixture.invalid'),
        client: client,
        requestUdpMedia: true,
      );
      await api.desktops('fixture');
      await api.connect('fixture', 'pve-150');
      expect(
        (jsonDecode(client.requests.last.body.toString()) as Map).containsKey(
          'udpMediaProtocol',
        ),
        isFalse,
      );
    });
  });

  group('control', () {
    UdpMedia media(_FakeLeg leg) => UdpMedia(
      UdpGrant.parse(_event())!,
      leg,
      sessionId: 'udp-session',
      nowUs: () => 0,
      mediaAllowed: true,
      sendKey: (_) {},
      delivered: (_) {},
      keyed: () {},
    );

    test('one reading at a time, every 100 ms', () {
      fakeAsync((async) {
        final leg = _FakeLeg();
        final udp = media(leg)..start(0);
        async.elapse(const Duration(milliseconds: 350));
        expect(leg.polls, 3);
        leg.stuckPoll = Completer<UdpLegReading?>();
        async.elapse(const Duration(milliseconds: 500));
        expect(leg.polls, 4, reason: 'the stuck one, and no other');
        udp.close();
      });
    });

    test(
      'a failed reading shows in the report and the next one still runs',
      () {
        fakeAsync((async) {
          final leg = _FakeLeg()..failing = 'poll';
          final udp = media(leg)..start(0);
          async.elapse(const Duration(milliseconds: 150));
          expect(udp.report, {'udpLegFailed': 1});
          leg
            ..failing = null
            ..reading = const UdpLegReading(report: {'udpProbe': 1});
          async.elapse(const Duration(milliseconds: 100));
          expect(leg.polls, 2);
          expect(udp.report, {'udpProbe': 1});
          udp.close();
        });
      },
    );

    test('a native reading decodes through the channel', () async {
      TestWidgetsFlutterBinding.ensureInitialized();
      const channel = MethodChannel('multitap/udp');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => {
          'report': {'udpProbe': 1},
          'running': true,
          'arrivals': [7, 8, 9],
          'audioBytes': 0,
          'keyedStream': 0,
        },
      );
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final reading = await NativeUdpLeg().poll();
      expect(reading!.arrivals, [(7, 8, 9)]);
      expect(reading.report, {'udpProbe': 1});
    });

    test('close leaves no timer and stops the leg once', () {
      fakeAsync((async) {
        final leg = _FakeLeg();
        final udp = media(leg)..start(0);
        async.elapse(const Duration(milliseconds: 250));
        udp.close();
        udp.close();
        expect(leg.stops, 1);
        expect(async.pendingTimers, isEmpty);
      });
    });
  });

  group('session', () {
    late _LiveSocket socket;
    late _PathMedia media;
    late List<_FakeLeg> legs;

    Future<RemoteSession> open({
      bool udpMedia = true,
      bool adaptive = true,
      bool epochMedia = false,
      bool inputLatency = false,
      bool firstKey = true,
      UdpLegFactory? factory,
      StreamStatsSink? stats,
      Stopwatch? clock,
    }) async {
      socket = _LiveSocket();
      media = _PathMedia();
      legs = [];
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'resize-session',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/resize-session',
          ),
          options: adaptive
              ? StreamOptions.recommended.negotiated(100000000)
              : const StreamOptions(),
          bitrateAdaptive: adaptive,
          udpMedia: udpMedia,
          epochMedia: epochMedia,
          viewportProvisioning: epochMedia,
          inputLatency: inputLatency,
        ),
        token: 'not-persisted',
        media: media,
        dialer: (_, _) async => socket,
        statsSink: stats,
        clock: clock,
        udpLegFactory:
            factory ??
            () {
              final leg = _FakeLeg();
              legs.add(leg);
              return leg;
            },
      );
      unawaited(session.start());
      await flush();
      socket.incoming.add(jsonEncode(epochMedia ? startInfo : info));
      await flush();
      if (firstKey) {
        socket.incoming.add(packet());
        await flush();
        expect(session.frameReady, isTrue, reason: session.error);
      }
      return session;
    }

    Future<void> control(Map<String, dynamic> event) async {
      socket.incoming.add(jsonEncode(event));
      await flush();
    }

    Future<void> reading() =>
        Future<void>.delayed(const Duration(milliseconds: 130));

    void expectUntouched(RemoteSession session) {
      expect(session.closed, isFalse);
      expect(session.error, isNull);
      expect(session.diagnosticCode, isNull);
      expect(session.frameReady, isTrue);
    }

    Iterable<Map<String, dynamic>> keysSent() =>
        socket.sent.where((event) => event['type'] == 'udp.key');

    Future<void> finish(RemoteSession session) async {
      await session.close();
      await media.events.close();
    }

    test(
      'a grant starts the leg once; a repeat changes nothing; close ends it',
      () async {
        final session = await open();
        await control(_event());
        final leg = legs.single;
        expect(leg.calls, ['stream 0/1', 'start']);
        final (grant, sessionId, clockUs) = leg.started.single;
        expect(grant.id, base64.decode(_id));
        expect(leg.tickets.single, base64.decode(_ticket));
        expect(
          grant.ticket,
          everyElement(0),
          reason: 'the leg has it: nothing keeps it',
        );
        expect(sessionId, 'resize-session');
        expect(clockUs, greaterThan(0));
        await control(_event());
        expect(legs, hasLength(1), reason: 'the Broker grants once');
        expect(leg.stops, 0);
        expect(leg.started, hasLength(1));
        await session.close();
        expect(leg.stops, 1);
        await media.events.close();
      },
    );

    test('a malformed grant changes nothing', () async {
      final session = await open();
      await control({..._event(), 'ticket': 'short'});
      await control({..._event(), 'protocol': 'udp-media-v2'});
      await control(_event(relays: []));
      await control({'type': 'udp.grant'});
      expect(legs, isEmpty);
      await control(_event());
      await control({..._event(), 'ticket': 'short'});
      expect(legs.single.stops, 0, reason: 'the running leg stays');
      expectUntouched(session);
      await finish(session);
    });

    test('a session that did not offer it ignores a grant', () async {
      final session = await open(udpMedia: false);
      await control(_event());
      await reading();
      expect(legs, isEmpty);
      expectUntouched(session);
      await finish(session);
    });

    test('nothing waits for the leg', () async {
      final stuck = _FakeLeg()
        ..stuckStart = Completer<void>()
        ..stuckPoll = Completer<UdpLegReading?>()
        ..stuckStop = Completer<void>();
      final session = await open(factory: () => stuck);
      final before = media.packets.length;
      socket.incoming.add(jsonEncode(_event()));
      socket.incoming.add(packet());
      await reading();
      expect(media.packets.length, before + 1, reason: 'the next packet ran');
      expect(stuck.polls, 1, reason: 'one reading at a time');
      expectUntouched(session);
      final closing = session.close();
      await closing.timeout(const Duration(seconds: 3));
      expect(stuck.stops, 1);
      await media.events.close();
    });

    test('a leg that throws never reaches the session', () async {
      for (final failing in const [
        'factory',
        'start',
        'stream',
        'poll',
        'stop',
      ]) {
        final session = await open(
          factory: () => failing == 'factory'
              ? throw StateError('factory')
              : (_FakeLeg()..failing = failing),
        );
        socket.incoming.add(jsonEncode(_event()));
        await reading();
        socket.incoming.add(packet());
        await flush();
        expectUntouched(session);
        await finish(session);
      }
    });

    test('every leg outcome leaves the session alone', () async {
      for (final code in [
        udpProbeSocketError,
        udpProbeLookupFailed,
        udpProbeBindTimeout,
        udpProbeNoPong,
      ]) {
        final session = await open();
        await control(_event());
        legs.single.reading = UdpLegReading(
          report: {'udpProbe': code},
          running: code == udpProbeNoPong,
        );
        await reading();
        expectUntouched(session);
        await finish(session);
      }
    });

    test('the session says which path carries the video', () async {
      final session = await open();
      expect(session.onUdp, isFalse);
      await control(_event());
      legs.single.reading = const UdpLegReading(report: {'udpActivePath': 1});
      await reading();
      expect(session.onUdp, isTrue);
      legs.single.reading = const UdpLegReading(report: {'udpActivePath': 0});
      await reading();
      expect(session.onUdp, isFalse);
      await finish(session);
    });

    test('a leg that stopped is no longer read', () async {
      final session = await open();
      await control(_event());
      legs.single.reading = const UdpLegReading(
        report: {'udpProbe': udpProbeBindTimeout},
        running: false,
      );
      await reading();
      final polls = legs.single.polls;
      await reading();
      expect(legs.single.polls, polls);
      await finish(session);
    });

    test('close stops the reading and any later key', () async {
      final session = await open();
      await control(_event());
      await session.close();
      final polls = legs.single.polls;
      await reading();
      expect(legs.single.polls, polls);
      expect(session.closed, isTrue);
      await media.events.close();
    });

    test('without a grant nothing runs and no field appears', () async {
      final reports = <Map<String, Object>>[];
      final session = await open(stats: reports.add);
      await Future<void>.delayed(const Duration(milliseconds: 3300));
      await session.close();
      await media.events.close();
      expect(legs, isEmpty);
      expect(reports, isNotEmpty);
      expect(reports.last.keys.where((key) => key.startsWith('udp')), isEmpty);
    }, timeout: const Timeout(Duration(seconds: 15)));

    test(
      'the statistics report carries the leg\'s fixed numbers only',
      () async {
        final reports = <Map<String, Object>>[];
        final session = await open(stats: reports.add);
        final feed = Timer.periodic(
          const Duration(milliseconds: 500),
          (_) => socket.incoming.add(packet()),
        );
        await control(_event());
        legs.single.reading = const UdpLegReading(
          report: {
            'udpProbe': udpProbeReachable,
            'udpRelayIndex': 0,
            'udpRttP50Ms': 3,
            'udpRttP95Ms': 9,
            'udpLossPct': 0,
            'udpKeepSent': 0,
            'udpKeepLost': 0,
            'udpActivePath': 1,
            'udpDirectPath': 2,
            'udpDirectRttMs': 1,
            'udpRelayRttMs': 31,
            'udpFallbacks': 2,
            'udpGaps': 7,
            // The Android leg's stall watch (UdpReceiver.watch).
            'udpKeyRequests': 5,
            'udpStalls': 1,
            'udpStallCause': 1,
          },
        );
        // Three 1 s ticks: a short session's last window needs them.
        await Future<void>.delayed(const Duration(milliseconds: 3400));
        expect(session.videoPathLabel, 'P2P 직결');
        feed.cancel();
        await session.close();
        expect(session.videoPathLabel, '연결 끊김');
        await media.events.close();
        final report = reports.last;
        expect(report['udpProbe'], udpProbeReachable);
        expect(report['udpActivePath'], 1);
        expect(report['udpDirectPath'], 2);
        expect(report['udpDirectRttMs'], 1);
        expect(report['udpRelayRttMs'], 31);
        expect(report['udpFallbacks'], 2);
        expect(report['udpGaps'], 7);
        expect(report['udpKeyRequests'], 5);
        expect(report['udpStalls'], 1);
        expect(report['udpStallCause'], 1);
        expect(report.keys.every(streamStatsFields.contains), isTrue);
        final text = jsonEncode(report);
        for (final secret in [_ticket, _id, _hostA]) {
          expect(text, isNot(contains(secret)));
        }
      },
      timeout: const Timeout(Duration(seconds: 25)),
    );

    group('keys', () {
      final clientKey = Uint8List.fromList(List.filled(32, 7));
      final hostKey = {
        'type': 'udp.key',
        'protocol': 'udp-media-v1',
        'publicKey': base64.encode(List.filled(32, 9)),
      };

      /// A probe that passed, on a grant with this marker.
      Future<RemoteSession> granted(
        Object? marker, {
        bool adaptive = true,
      }) async {
        final session = await open(adaptive: adaptive);
        await control({..._event(), 'keys': ?marker});
        legs.single.reading = UdpLegReading(
          report: const {'udpProbe': udpProbeReachable},
          publicKey: clientKey,
        );
        await reading();
        return session;
      }

      test('the marker parses, and only the exact value counts', () {
        expect(UdpGrant.parse(_event())!.keys, isFalse);
        expect(UdpGrant.parse({..._event(), 'keys': true})!.keys, isTrue);
        expect(
          UdpGrant.parse({..._event(), 'keys': true, 'later': 1})!.keys,
          isTrue,
          reason: 'a field a later Broker adds is tolerated',
        );
        for (final value in const <Object?>[false, 1, 'true', null]) {
          expect(
            UdpGrant.parse({..._event(), 'keys': value})!.keys,
            isFalse,
            reason: '$value',
          );
        }
      });

      for (final (name, marker) in const <(String, Object?)>[
        ('without the marker', null),
        ('with a marker that is not true', 'true'),
      ]) {
        test('$name: no key, no path, same pushes', () async {
          // A released Broker ends the session on a client udp.key.
          final session = await granted(marker);
          for (var i = 0; i < 3; i++) {
            socket.incoming.add(packet());
          }
          await flush();
          await control(hostKey);
          await reading();
          expect(keysSent(), isEmpty);
          expect(legs.single.keys, isEmpty, reason: 'the Host only answers');
          expect(media.udpPathRule, isFalse);
          expect(media.ruled, everyElement(isFalse));
          expect(media.ruled, hasLength(greaterThanOrEqualTo(4)));
          expectUntouched(session);
          await finish(session);
        });
      }

      test('the marker: the client speaks first, once, exact fields', () async {
        final session = await granted(true);
        expect(keysSent().single, {
          'type': 'udp.key',
          'protocol': 'udp-media-v1',
          'publicKey': base64.encode(clientKey),
        });
        expect(legs.single.keys, isEmpty, reason: 'no answer yet');
        expect(media.udpPathRule, isFalse);
        await control(hostKey);
        expect(
          legs.single.keys.single.$1,
          base64.decode(hostKey['publicKey']!),
        );
        expect(
          legs.single.keys.single.$2,
          isTrue,
          reason: 'WSS fed a key frame',
        );
        await control(hostKey);
        await reading();
        expect(legs.single.keys, hasLength(1), reason: 'once per grant');
        expect(keysSent(), hasLength(1));
        await finish(session);
      });

      test('no key before the probe passed', () async {
        final session = await open();
        await control({..._event(), 'keys': true});
        legs.single.reading = const UdpLegReading(
          report: {'udpProbe': udpProbeNoPong, 'udpLossPct': 100},
        );
        await reading();
        await control(hostKey);
        expect(keysSent(), isEmpty);
        expect(legs.single.keys, isEmpty);
        expect(media.udpPathRule, isFalse);
        await finish(session);
      });

      test('a key frame not yet taken is passed on with the answer', () async {
        final session = await open(firstKey: false);
        await control({..._event(), 'keys': true});
        legs.single.reading = UdpLegReading(publicKey: clientKey);
        await reading();
        await control(hostKey);
        expect(legs.single.keys.single.$2, isFalse);
        await finish(session);
      });

      test('WSS pushes follow the rule only after the answer', () async {
        final session = await granted(true);
        socket.incoming.add(packet());
        await flush();
        expect(media.ruled.last, isFalse);
        await control(hostKey);
        socket.incoming.add(packet());
        await flush();
        expect(media.ruled.last, isTrue);
        await session.close();
        expect(media.udpPathRule, isFalse, reason: 'gone with the leg');
        await media.events.close();
      });

      test('a malformed answer changes nothing', () async {
        final session = await granted(true);
        for (final bad in <Map<String, dynamic>>[
          {...hostKey, 'protocol': 'udp-media-v2'},
          {'type': 'udp.key'},
          {...hostKey, 'publicKey': 'short'},
          {...hostKey, 'publicKey': 5},
          {...hostKey, 'publicKey': base64.encode(List.filled(31, 9))},
          {...hostKey, 'publicKey': base64.encode(List.filled(33, 9))},
        ]) {
          await control(bad);
        }
        expect(legs.single.keys, isEmpty);
        expect(media.udpPathRule, isFalse);
        expectUntouched(session);
        await finish(session);
      });

      test('a key without a grant is ignored', () async {
        final session = await open();
        await control(hostKey);
        expect(keysSent(), isEmpty);
        expect(media.udpPathRule, isFalse);
        expectUntouched(session);
        await finish(session);
      });

      test('a fixed bitrate stays on WebSocket, marker or not', () async {
        final session = await granted(true, adaptive: false);
        await control(hostKey);
        await reading();
        expect(legs.single.keys, isEmpty);
        expect(keysSent(), isEmpty);
        expect(media.udpPathRule, isFalse);
        await finish(session);
      });
    });
    group('delivered media', () {
      test(
        'keeps the video watchdog alive, and only delivered media',
        () async {
          final clock = _ManualClock()..now = 1000000;
          final session = await open(clock: clock);
          await control(_event());
          final leg = legs.single;
          // 24 s without a WebSocket frame, 6 s at a time.
          for (var step = 1; step <= 4; step++) {
            clock.now += 6000000;
            media.decoded++;
            leg.reading = UdpLegReading(
              arrivals: [(step * 1000, clock.now - 1000, 2000)],
            );
            await Future<void>.delayed(const Duration(milliseconds: 1150));
          }
          expect(session.closed, isFalse, reason: session.diagnosticCode);
          // Audio alone cannot hide it: the watchdog fires as on WebSocket.
          clock.now += 16000000;
          media.decoded++;
          leg.reading = const UdpLegReading(audioBytes: 1932);
          await Future<void>.delayed(const Duration(milliseconds: 1150));
          expect(session.diagnosticCode, 'video_timeout');
          await session.close();
          await media.events.close();
        },
        timeout: const Timeout(Duration(seconds: 20)),
      );

      test('feeds stream.feedback, bytes and the video gap', () async {
        final session = await open();
        await control(_event());
        final before = session.receivedBytes;
        legs.single.reading = const UdpLegReading(
          arrivals: [(1000, 500000, 3000), (2000, 700000, 4000)],
          audioBytes: 1932,
        );
        await reading();
        legs.single.reading = const UdpLegReading();
        await reading();
        expect(session.receivedBytes - before, greaterThanOrEqualTo(8932));
        final reports = socket.sent
            .where((event) => event['type'] == 'stream.feedback')
            .toList();
        final frames = [
          for (final report in reports) ...(report['frames'] as List),
        ];
        expect(
          frames,
          containsAll([
            [1000, 500000],
            [2000, 700000],
          ]),
        );
        final bytes = reports
            .map((report) => report['bytes'] as int)
            .reduce((a, b) => a + b);
        expect(bytes, greaterThanOrEqualTo(8932));
        await finish(session);
      });

      test('the longest gap between UDP frames is the reading', () async {
        final clock = _ManualClock()..now = 1000000;
        final session = await open(clock: clock);
        await control(_event());
        legs.single.reading = const UdpLegReading(
          arrivals: [
            (10, 1016000, 100),
            (20, 1032000, 100),
            (30, 1332000, 100),
          ],
        );
        await reading();
        clock.now = 1400000;
        for (var i = 0; i < 30 && session.videoGapMaxMsTick == null; i++) {
          await Future<void>.delayed(const Duration(milliseconds: 50));
        }
        expect(session.videoGapMaxMsTick, 300.0);
        await finish(session);
      });

      test('a key frame over UDP opens the WebSocket key gate', () async {
        final session = await open(firstKey: false);
        await control(_event());
        final leg = legs.single;
        final pFrame = Uint8List.fromList(packet())..[1] = 0;
        // Another stream's key frame opens nothing.
        leg.reading = UdpLegReading(keyedStream: leg.generations.last + 1);
        await reading();
        socket.incoming.add(pFrame);
        await flush();
        expect(media.packets, isEmpty);
        leg.reading = UdpLegReading(keyedStream: leg.generations.last);
        await reading();
        socket.incoming.add(pFrame);
        await flush();
        expect(media.packets, hasLength(1));
        await finish(session);
      });

      test('a new epoch reaches the leg before its decoder starts', () async {
        final session = await open(epochMedia: true, adaptive: false);
        await control(_event());
        final leg = legs.single;
        media.onStart = () => leg.calls.add('decoder');
        leg.calls.clear();
        unawaited(session.resizeViewport(1346, 812));
        await flush();
        final id =
            socket.sent.lastWhere(
                  (event) => event['type'] == 'stream.resize',
                )['requestId']
                as String;
        await control(resizeAck(id));
        await control(applied(id));
        expect(leg.calls, ['stream 1/2', 'decoder'], reason: session.error);
        await finish(session);
      });

      test('a Host run in the same epoch restarts PTS on both paths', () async {
        final session = await open(epochMedia: true, adaptive: false);
        await control(_event());
        final leg = legs.single;
        leg.calls.clear();
        await control(startInfo);
        expect(leg.calls, ['stream 0/2']);
        expect(media.starts, 1, reason: 'no new decoder');
        expectUntouched(session);
        await finish(session);
      });

      test('a stale announcement leaves the leg alone', () async {
        final session = await open(epochMedia: true, adaptive: false);
        await control(_event());
        final leg = legs.single;
        unawaited(session.resizeViewport(1346, 812));
        await flush();
        final id =
            socket.sent.lastWhere(
                  (event) => event['type'] == 'stream.resize',
                )['requestId']
                as String;
        await control(resizeAck(id));
        await control(applied(id));
        leg.calls.clear();
        await control(startInfo);
        expect(leg.calls, isEmpty);
        await finish(session);
      });

      test('a tagged frame over UDP still measures input to frame', () async {
        final clock = _ManualClock()..now = 1000000;
        final session = await open(clock: clock, inputLatency: true);
        await control(_event());
        await session.capture(true);
        media.events.add(const {
          'type': 'input',
          'event': 'button',
          'button': 'left',
          'down': true,
          'x': 0.25,
          'y': 0.75,
        });
        await flush();
        await control({
          'type': 'input.frame',
          'sessionId': 'resize-session',
          'epoch': 0,
          'sample': 1,
          'pts': 33334,
        });
        legs.single.reading = const UdpLegReading(
          arrivals: [(33334, 1004000, 2000)],
        );
        await reading();
        media.frames.add([33334, 2500]);
        await Future<void>.delayed(const Duration(milliseconds: 1150));
        expect(session.inputToFrameP50Ms, 6.5);
        await finish(session);
      });
    });
  });

  test('A window reports the latest probe numbers; none without them', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 1));
    const open = StreamStatsSample(width: 1280, height: 720);
    window.add(open, 0);
    expect(
      window.take(1000000)!.keys.where((key) => key.startsWith('udp')),
      isEmpty,
    );
    window.add(
      const StreamStatsSample(
        width: 1280,
        height: 720,
        udpProbe: {'udpProbe': 4},
      ),
      1000000,
    );
    window.add(
      const StreamStatsSample(
        width: 1280,
        height: 720,
        udpProbe: {'udpProbe': 1, 'udpKeepSent': 2},
      ),
      2000000,
    );
    final report = window.take(3000000)!;
    expect(report['udpProbe'], 1);
    expect(report['udpKeepSent'], 2);
    expect(
      streamStatsFields,
      containsAll([
        'udpProbe',
        'udpRttP50Ms',
        'udpRttP95Ms',
        'udpLossPct',
        'udpRelayIndex',
        'udpKeepSent',
        'udpKeepLost',
      ]),
    );
  });
}

class _ManualClock extends Fake implements Stopwatch {
  int now = 0;
  @override
  int get elapsedMicroseconds => now;
}

class _FakeLeg implements UdpLeg {
  final inputs = <(int, int, Uint8List)>[];
  @override
  Future<void> input(int sequence, int epoch, Uint8List bytes) async {
    inputs.add((sequence, epoch, bytes));
  }

  final peers = <List<(String, int)>>[];
  @override
  Future<void> peer(List<(String, int)> candidates) async {
    peers.add(candidates);
  }

  final calls = <String>[];
  final started = <(UdpGrant, String, int)>[];
  final tickets = <Uint8List>[];
  final keys = <(Uint8List, bool)>[];
  final generations = <int>[];
  int polls = 0, stops = 0;
  String? failing;
  Completer<void>? stuckStart, stuckStop;
  Completer<UdpLegReading?>? stuckPoll;
  UdpLegReading reading = const UdpLegReading();

  void _fail(String name) {
    if (failing == name) throw StateError('leg surprise');
  }

  @override
  Future<void> start(UdpGrant grant, String sessionId, int clockUs) async {
    _fail('start');
    calls.add('start');
    started.add((grant, sessionId, clockUs));
    tickets.add(Uint8List.fromList(grant.ticket));
    await stuckStart?.future;
  }

  @override
  Future<void> stream(int epoch, int generation) async {
    _fail('stream');
    calls.add('stream $epoch/$generation');
    generations.add(generation);
  }

  @override
  Future<UdpLegReading?> poll() async {
    polls++;
    _fail('poll');
    if (stuckPoll != null) return stuckPoll!.future;
    final value = reading;
    // A reading hands over what it holds, as the native leg does.
    reading = UdpLegReading(
      report: value.report,
      running: value.running,
      publicKey: value.publicKey,
      keyedStream: value.keyedStream,
    );
    return value;
  }

  @override
  Future<void> hostKey(Uint8List key, {required bool keyed}) async {
    calls.add('hostKey');
    keys.add((key, keyed));
  }

  @override
  Future<void> stop() async {
    stops++;
    _fail('stop');
    await stuckStop?.future;
  }
}

class _PathMedia extends SessionMedia
    implements UdpPathMedia, InputFrameWatchMedia {
  @override
  bool udpPathRule = false;
  final ruled = <bool>[];
  final frames = <List<int>>[];

  /// Frames the leg's engine decoded, which SessionMedia cannot see.
  int decoded = 0;
  void Function()? onStart;
  @override
  Future<int> start(int width, int height) async {
    onStart?.call();
    starts++;
    return 42;
  }

  @override
  Future<void> push(Uint8List packet) {
    ruled.add(udpPathRule);
    return super.push(packet);
  }

  @override
  Future<void> pushWatched(Uint8List packet) => push(packet);

  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'decodedFrames': packets.length + decoded,
    'inputFrames': frames,
  };
}

/// Answers the session's liveness pings, as a Host does.
class _LiveSocket extends SessionSocket {
  @override
  void add(dynamic data) {
    super.add(data);
    final value = sent.last;
    if (value['type'] == 'ping') {
      incoming.add(jsonEncode({'type': 'pong', 'id': value['id']}));
    }
  }
}
