// gamepad-v1: the state and rumble messages, the forwarder between local pads
// and a session (pads assigned in order, a state only on change, inactive =
// neutral, removal, rumble back to its pad), the exact connect gating and grant
// check, the session's sends and rumble, the counts on its statistics rows,
// and the platform channel. Injected HTTP, socket and channel: no network,
// Host or controller.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/gamepad.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_gamepads.dart';
import 'package:multitap_client/data/stream_stats_report.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'audio_stats_test.dart' show AudioMedia;
import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart'
    show SessionMedia, SessionSocket, flush, info, packet;
import 'session_recovery_test.dart' show ManualClock, RecoveryMedia;

const _origin = 'https://fixture.invalid';

Map<String, dynamic> _desktop(Map<String, Object> capabilities) => {
  'id': 'pve-149',
  'name': 'TESTER',
  'kind': 'vm',
  'description': '',
  'os': 'Windows',
  'status': 'ready',
  'canConnect': true,
  'host': {'connected': true, 'capabilities': capabilities},
};
Map<String, dynamic> _connected({bool granted = false}) => {
  'sessionId': 'pad-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/pad-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) 'gamepadProtocol': 'gamepad-v1',
};

Future<(List<Map>, BrokerConnection)> _connect(
  Map<String, Object> capabilities,
  List<(int, Map<String, dynamic>)> replies, {
  bool request = true,
}) async {
  final client = ApiClient()
    ..responses.add((
      200,
      {
        'desktops': [_desktop(capabilities)],
      },
    ))
    ..responses.addAll(replies);
  final api = HttpBrokerApi(
    origin: Uri.parse(_origin),
    client: client,
    requestGamepad: request,
  );
  await api.desktops('fixture');
  final connection = await api.connect('fixture', 'pve-149');
  return (
    [
      for (final item in client.requests.skip(1))
        jsonDecode(item.body.toString()) as Map,
    ],
    connection,
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const released = <String, Object>{
    'service': true,
    'interactiveSession': true,
    'streaming': true,
    'virtualDisplayReady': true,
  };
  const capable = <String, Object>{...released, 'gamepadProtocol': 'gamepad-v1'};
  const a = GamepadState(buttons: GamepadButtons.a, lt: 255, lx: -32768);

  test('States, removals and rumble are the protocol\'s, exactly', () {
    expect(a.message(2), {
      'type': 'gamepad.state',
      'pad': 2,
      'buttons': 0x1000,
      'lt': 255,
      'rt': 0,
      'lx': -32768,
      'ly': 0,
      'rx': 0,
      'ry': 0,
    });
    expect(gamepadRemoveMessage(1), {'type': 'gamepad.remove', 'pad': 1});
    final clamped = GamepadState.clamped(
      buttons: 0xFFFF,
      lt: 300,
      rt: -1,
      lx: 40000,
      ly: -40000,
      rx: 1,
      ry: 2,
    );
    expect(clamped.buttons, 0xF7FF, reason: 'the undefined bit is dropped');
    expect([clamped.lt, clamped.rt, clamped.lx, clamped.ly], [255, 0, 32767, -32768]);
    expect(
      gamepadStateFrom({'slot': 0, 'buttons': 16, 'lt': 0, 'rt': 0, 'lx': 0, 'ly': 0, 'rx': 0, 'ry': 0}),
      const GamepadState(buttons: 16),
    );
    expect(gamepadStateFrom({'buttons': 16}), isNull);
    expect(
      GamepadRumble.parse({'type': 'gamepad.rumble', 'pad': 1, 'low': 200, 'high': 30}),
      const GamepadRumble(1, 200, 30),
    );
    for (final bad in <Map<String, dynamic>>[
      {'type': 'gamepad.rumble', 'pad': 4, 'low': 0, 'high': 0},
      {'type': 'gamepad.rumble', 'pad': 0, 'low': 256, 'high': 0},
      {'type': 'gamepad.rumble', 'pad': 0, 'low': 0},
      {'type': 'gamepad.rumble', 'pad': 0, 'low': 0, 'high': 0, 'x': 1},
    ]) {
      expect(GamepadRumble.parse(bad), isNull, reason: '$bad');
    }
    expect(gamepadAdvertised(capable), isTrue);
    expect(gamepadAdvertised(released), isFalse);
    expect(gamepadAdvertised({...released, 'gamepadProtocol': 'gamepad-v2'}), isFalse);
  });

  test('The forwarder assigns pads in order, sends changes only, neutral while inactive, removes and rumbles back', () {
    final sent = <Map<String, Object?>>[];
    final rumbled = <(Object, int, int)>[];
    final forwarder = GamepadForwarder(
      send: (message) {
        sent.add(message);
        return true;
      },
      rumble: (local, low, high) => rumbled.add((local, low, high)),
    );
    forwarder.state(17, a);
    forwarder.state(17, a);
    forwarder.state(9, GamepadState.neutral);
    expect(sent, [a.message(0), GamepadState.neutral.message(1)], reason: 'a neutral first state still plugs its pad in');
    forwarder.setActive(false);
    expect(sent.last, GamepadState.neutral.message(0), reason: 'nothing stays pressed');
    final count = sent.length;
    forwarder.state(17, const GamepadState(buttons: GamepadButtons.b));
    expect(sent.length, count, reason: 'inactive: the pad stays neutral');
    forwarder.setActive(true);
    expect(sent.last, const GamepadState(buttons: GamepadButtons.b).message(0), reason: 'back: its newest state');
    forwarder.hostRumble(const GamepadRumble(1, 10, 20));
    forwarder.hostRumble(const GamepadRumble(3, 1, 1));
    expect(rumbled, [(9, 10, 20)]);
    forwarder.removed(17);
    expect(sent.last, gamepadRemoveMessage(0));
    forwarder.state(33, a);
    expect(sent.last, a.message(0), reason: 'a freed pad is given again');
    for (var local = 40; local < 45; local++) {
      forwarder.state(local, a);
    }
    expect(forwarder.pads.length, gamepadMaxPads, reason: 'at most four');
    forwarder.close();
    expect(rumbled.last, (9, 0, 0), reason: 'the motor this view set stops');
    expect(rumbled.where((r) => r.$2 == 0 && r.$3 == 0).length, 1, reason: 'no other pad is touched');
  });

  test('Resync sends every pad again and the removals the session could not take; a new pad overtakes a pending removal', () {
    var accept = true;
    final sent = <Map<String, Object?>>[];
    final forwarder = GamepadForwarder(
      send: (message) {
        if (accept) sent.add(message);
        return accept;
      },
      rumble: (_, _, _) {},
    );
    forwarder.state(1, a);
    forwarder.state(2, GamepadState.neutral);
    forwarder.state(3, a);
    accept = false;                       // the socket is being replaced
    forwarder.state(1, const GamepadState(buttons: GamepadButtons.x));
    forwarder.removed(2);
    forwarder.removed(3);
    accept = true;
    sent.clear();
    forwarder.state(4, const GamepadState(buttons: GamepadButtons.y));   // takes pad 1, freed by local 2
    forwarder.resync();
    expect(sent, [
      const GamepadState(buttons: GamepadButtons.y).message(1),
      gamepadRemoveMessage(2),
      const GamepadState(buttons: GamepadButtons.x).message(0),
      const GamepadState(buttons: GamepadButtons.y).message(1),
    ], reason: 'pad 1 needs no removal (its new state replaces it); pad 2 is removed; every state again');
    sent.clear();
    forwarder.resync();
    expect(sent, [const GamepadState(buttons: GamepadButtons.x).message(0), const GamepadState(buttons: GamepadButtons.y).message(1)],
        reason: 'a removal is sent once');
    forwarder.setActive(false);
    sent.clear();
    forwarder.resync();
    expect(sent, [GamepadState.neutral.message(0), GamepadState.neutral.message(1)], reason: 'inactive: neutral');
    // A removal the session took may have gone into a socket that was already dead: sent once more at the next resync.
    final again = <Map<String, Object?>>[];
    final other = GamepadForwarder(send: (message) { again.add(message); return true; }, rumble: (_, _, _) {});
    other.state(1, a);
    other.removed(1);
    again.clear();
    other.resync();
    expect(again, [gamepadRemoveMessage(0)]);
    again.clear();
    other.resync();
    expect(again, isEmpty, reason: 'once');
  });

  test('Only the view in front drives the motors, and it stops only what it set', () {
    final rumbled = <(Object, int, int)>[];
    final forwarder = GamepadForwarder(send: (_) => true, rumble: (local, low, high) => rumbled.add((local, low, high)));
    forwarder.state(7, a);
    forwarder.hostRumble(const GamepadRumble(0, 0, 0));
    expect(rumbled, isEmpty, reason: 'a stop this view never started leaves another window\'s motors alone');
    forwarder.hostRumble(const GamepadRumble(0, 120, 120));
    expect(rumbled, [(7, 120, 120)]);
    forwarder.setActive(false);
    expect(rumbled.last, (7, 0, 0), reason: 'leaving stops it');
    forwarder.hostRumble(const GamepadRumble(0, 200, 10));
    expect(rumbled.length, 2, reason: 'inactive: nothing is driven');
    forwarder.setActive(true);
    expect(rumbled.last, (7, 200, 10), reason: 'back: the Host\'s latest');
    forwarder.holdRumble();
    expect(rumbled.last, (7, 0, 0), reason: 'a frozen stream stops it');
    forwarder.setActive(false);
    forwarder.setActive(true);
    expect(rumbled.length, 4, reason: 'the level is unknown until the Host sends it again');
    forwarder.close();
    expect(rumbled.length, 4, reason: 'nothing to stop');
  });

  test('A state the session could not take is sent again with the next change', () {
    var accept = false;
    final sent = <Map<String, Object?>>[];
    final forwarder = GamepadForwarder(
      send: (message) {
        if (accept) sent.add(message);
        return accept;
      },
      rumble: (_, _, _) {},
    );
    forwarder.state(1, a);
    accept = true;
    forwarder.state(1, a);
    expect(sent, [a.message(0)]);
  });

  test('Asked where advertised; older peers and clients that do not read pads never see it', () async {
    final granted = await _connect(capable, [(200, _connected(granted: true))]);
    expect(granted.$1.single['gamepadProtocol'], 'gamepad-v1');
    expect(granted.$2.gamepad, isTrue);
    final older = await _connect(released, [(200, _connected())]);
    expect(older.$1.single.containsKey('gamepadProtocol'), isFalse);
    final quiet = await _connect(capable, [(200, _connected())], request: false);
    expect(quiet.$1.single.containsKey('gamepadProtocol'), isFalse);
    final retried = await _connect(capable, [
      (
        400,
        {
          'error': {'code': 'unexpected_field'},
        },
      ),
      (200, _connected()),
    ]);
    expect(retried.$1.first['gamepadProtocol'], 'gamepad-v1');
    expect(retried.$1.last.containsKey('gamepadProtocol'), isFalse);
    expect(retried.$2.gamepad, isFalse);
    final origin = Uri.parse(_origin);
    expect(
      () => BrokerConnection.fromJson(_connected(granted: true), origin),
      throwsA(isA<BrokerException>()),
      reason: 'never requested',
    );
    expect(
      () => BrokerConnection.fromJson(
        {..._connected(), 'gamepadProtocol': 'gamepad-v2'},
        origin,
        requestedGamepad: true,
      ),
      throwsA(isA<BrokerException>()),
    );
  });

  // Real time (the statistics poll that marks the first picture runs every second).
  for (final granted in [true, false]) {
    test('The session sends only with the grant and from its first picture, passes the Host\'s rumble on, '
        'and asks for every pad again at the first picture and after the Host is back (granted: $granted)', () async {
      final socket = SessionSocket(), media = RecoveryMedia();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'pad-session',
          webSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/pad-session'),
          gamepad: granted,
          sessionResume: true,
        ),
        token: 'fixture',
        media: media,
        dialer: (_, _) async => socket,
      );
      final rumbles = <GamepadRumble>[];
      final listening = session.gamepadRumble.listen(rumbles.add);
      var resyncs = 0;
      final resyncing = session.gamepadResync.listen((_) => resyncs++);
      expect(session.sendGamepad(a.message(0)), isFalse, reason: 'not connected yet');
      unawaited(session.start());
      await flush();
      socket.incoming.add(jsonEncode(info));
      await flush();
      expect(session.frameReady, isFalse);
      expect(session.sendGamepad(a.message(0)), isFalse, reason: 'connected, no picture yet: the Host may not take pads');
      expect(resyncs, 0);
      socket.incoming.add(packet());
      await _until(() => session.frameReady);
      expect(session.frameReady, isTrue);
      expect(resyncs, granted ? 1 : 0, reason: 'the first picture');
      expect(session.sendGamepad(a.message(0)), granted);
      socket.incoming.add(jsonEncode({'type': 'gamepad.rumble', 'pad': 0, 'low': 9, 'high': 8}));
      await flush();
      expect([for (final e in socket.sent) if (e['type'] == 'gamepad.state') e], granted ? [a.message(0)] : isEmpty);
      expect(rumbles, granted ? [const GamepadRumble(0, 9, 8)] : isEmpty);
      // The Host's channel is held: the broker drops what is sent then.
      socket.incoming.add(jsonEncode({'type': 'session.resuming', 'sessionId': 'pad-session'}));
      await flush();
      expect(session.sendGamepad(a.message(1)), isFalse);
      socket.incoming.add(jsonEncode({'type': 'session.resumed', 'sessionId': 'pad-session', 'token': 'B' * 43, 'graceMs': 15000}));
      await flush();
      expect(resyncs, granted ? 2 : 0, reason: 'the Host is back');
      expect(session.sendGamepad(a.message(1)), granted);
      await listening.cancel();
      await resyncing.cancel();
      await session.close();
      expect(session.sendGamepad(a.message(0)), isFalse, reason: 'closed');
      await media.events.close();
    });
  }

  test('The tally counts states sent and refused, pads held and rumbles; a window starts again from zero', () {
    final tally = GamepadTally();
    expect(tally.take(granted: false), {'gamepad': false}, reason: 'not granted: nothing else');
    tally.message(a.message(0), taken: true);
    tally.message(a.message(1), taken: true);
    tally.message(a.message(1), taken: true);
    tally.message(a.message(2), taken: false);
    tally.message(gamepadRemoveMessage(0), taken: true);
    tally.message(gamepadRemoveMessage(3), taken: false);
    tally.message({'type': 'gamepad.state'}, taken: true);
    tally.rumble();
    expect(tally.take(granted: true), {
      'gamepad': true,
      'gamepadPads': 1,
      'gamepadStates': 3,
      'gamepadRefused': 1,
      'gamepadRumbles': 1,
    });
    expect(tally.take(granted: true), {
      'gamepad': true,
      'gamepadPads': 1,
      'gamepadStates': 0,
      'gamepadRefused': 0,
      'gamepadRumbles': 0,
    }, reason: 'the pad is still held; the counts are per window');
    expect(gamepadStatsFields.every(streamStatsFields.contains), isTrue);
    final server = File('../../server/src/client-diagnostics.mjs').readAsStringSync();
    expect(server, contains("'gamepadPads', 'gamepadStates', 'gamepadRefused', 'gamepadRumbles'"));
    final boolean = RegExp(r'const boolean = new Set\(\[([^\]]*)\]\)').firstMatch(server)![1]!;
    expect(boolean, contains("'gamepad'"));
  });

  testWidgets('A granted session\'s statistics rows carry its pad counts; an ungranted one only says so', (t) async {
    for (final granted in [true, false]) {
      final socket = SessionSocket(), media = AudioMedia();
      final clock = ManualClock();
      final reports = <Map<String, Object>>[];
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'pad-session',
          webSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/pad-session'),
          options: const StreamOptions(audio: false),
          gamepad: granted,
        ),
        token: 'fixture',
        media: media,
        clock: clock,
        dialer: (_, _) async => socket,
        statsSink: reports.add,
      );
      unawaited(session.start());
      await t.pump();
      socket.incoming.add(jsonEncode(info));
      await t.pump();
      expect(session.sendGamepad(a.message(1)), isFalse, reason: 'before the first picture: refused');
      var answered = 0, sentPad = false;
      for (var second = 1; second <= 12; second++) {
        clock.now = second * 1000000;
        for (; answered < socket.sent.length; answered++) {
          final sent = socket.sent[answered];
          if (sent['type'] == 'ping') {
            socket.incoming.add(jsonEncode({'type': 'pong', 'id': sent['id']}));
          }
        }
        socket.incoming.add(packet());
        await t.pump(const Duration(seconds: 1));
        if (!sentPad && session.frameReady) {
          sentPad = true;
          expect(session.sendGamepad(a.message(0)), granted);
          socket.incoming.add(jsonEncode({'type': 'gamepad.rumble', 'pad': 0, 'low': 9, 'high': 8}));
        }
      }
      expect(sentPad, isTrue);
      expect(reports, isNotEmpty);
      expect(reports.every((report) => report['gamepad'] == granted), isTrue);
      int total(String key) => reports.fold(0, (sum, report) => sum + ((report[key] as int?) ?? 0));
      if (granted) {
        expect(reports.last['gamepadPads'], 1);
        expect(total('gamepadStates'), 1);
        expect(total('gamepadRefused'), 1);
        expect(total('gamepadRumbles'), 1);
      } else {
        expect(reports.first.keys.where((key) => key.startsWith('gamepad')), ['gamepad']);
      }
      expect(reports.every((report) => report.keys.every(streamStatsFields.contains)), isTrue);
      await session.close();
      await media.events.close();
    }
  });

  test('The platform reads pads only for a granted session; a pad read before the first picture is plugged in with it, '
      'rumble goes back and stops while the stream resumes, the end stops it', () async {
    const channel = MethodChannel('multitap/test-gamepad');
    final calls = <MethodCall>[];
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return call.method == 'start' ? true : null;
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    Future<void> platform(String method, Object? args) => messenger.handlePlatformMessage(
      channel.name,
      const StandardMethodCodec().encodeMethodCall(MethodCall(method, args)),
      (_) {},
    );
    List<Object?> padsSent(SessionSocket socket) => [for (final e in socket.sent) if (e['type'] == 'gamepad.state') e['pad']];
    for (final granted in [false, true]) {
      calls.clear();
      final socket = SessionSocket(), media = RecoveryMedia();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'pad-session',
          webSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/pad-session'),
          gamepad: granted,
          sessionResume: true,
        ),
        token: 'fixture',
        media: media,
        dialer: (_, _) async => socket,
      );
      // As remote_view.dart: the pads start with the session, before its socket is dialled.
      final pads = SessionGamepads(session, channel: channel);
      await pads.start();
      expect(pads.running, granted);
      expect(calls.map((c) => c.method), granted ? ['start'] : isEmpty);
      if (granted) {
        // An idle pad reports its state once, as the Windows runner does at 'start'.
        await platform('state', {'slot': 2, 'buttons': 0, 'lt': 0, 'rt': 0, 'lx': 0, 'ly': 0, 'rx': 0, 'ry': 0});
        await flush();
        expect(padsSent(socket), isEmpty, reason: 'not connected yet');
        unawaited(session.start());
        await flush();
        socket.incoming.add(jsonEncode(info));
        await flush();
        socket.incoming.add(packet());
        await _until(() => session.frameReady);
        await flush();
        expect(padsSent(socket), [0], reason: 'plugged in at the first picture without a touch');
        socket.incoming.add(jsonEncode({'type': 'gamepad.rumble', 'pad': 0, 'low': 50, 'high': 60}));
        await flush();
        expect(calls.last.method, 'rumble');
        expect(calls.last.arguments, {'slot': 2, 'low': 50, 'high': 60});
        socket.incoming.add(jsonEncode({'type': 'session.resuming', 'sessionId': 'pad-session'}));
        await flush();
        expect(calls.last.arguments, {'slot': 2, 'low': 0, 'high': 0}, reason: 'nothing vibrates while the stream is frozen');
        socket.incoming.add(jsonEncode({'type': 'session.resumed', 'sessionId': 'pad-session', 'token': 'C' * 43, 'graceMs': 15000}));
        await flush();
        expect(padsSent(socket), [0, 0], reason: 'sent again once the Host is back');
        await platform('removed', 2);
        await flush();
        expect(socket.sent.last, gamepadRemoveMessage(0));
        await session.close();
        await flush();
        expect(pads.running, isFalse, reason: 'the session\'s end stops the platform');
        expect(calls.last.method, 'stop');
      } else {
        await session.close();
      }
      await pads.stop();
      await media.events.close();
    }
  });
}

/// Real time: waits until [ok] holds, at most [within].
Future<void> _until(bool Function() ok, {Duration within = const Duration(seconds: 5)}) async {
  final end = DateTime.now().add(within);
  while (!ok() && DateTime.now().isBefore(end)) {
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
}
