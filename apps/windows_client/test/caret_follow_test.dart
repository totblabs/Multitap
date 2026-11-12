// caret-v1 on the client: the report parser, the broker grant (only after a
// request, ordinary stream sessions only), the connect request only toward an
// advertising inventory (old Hosts and brokers: no field, no caret), the
// session's watch gate and caret updates, and the Android keyboard camera that
// keeps the caret (or, without one, the last tap) in view above the keyboard.
import 'dart:async';
import 'dart:convert';
import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_touch.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/caret_follow.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, flush, info, packet;

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
  'sessionId': 'caret-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/caret-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) 'caretProtocol': 'caret-v1',
};
Map<String, dynamic> _report({
  String session = 'caret-session',
  Map<String, dynamic>? fields,
}) => {
  'type': 'caret',
  'sessionId': session,
  'has': true,
  'x': 100,
  'y': 1200,
  'w': 2,
  'h': 30,
  'sw': 1080,
  'sh': 2400,
  ...?fields,
};

void main() {
  test('the report parser is exact', () {
    final caret = RemoteCaret.parse(_report())!;
    expect(caret.has, isTrue);
    expect(caret.fraction!.left, closeTo(100 / 1080, 1e-9));
    expect(caret.fraction!.top, closeTo(1200 / 2400, 1e-9));
    expect(caret.fraction!.bottom, closeTo(1230 / 2400, 1e-9));
    expect(
      RemoteCaret.parse({'type': 'caret', 'has': false})!.fraction,
      isNull,
    );
    expect(
      RemoteCaret.parse(_report(fields: {'w': 0}))!.fraction!.right,
      greaterThan(100 / 1080),
      reason: 'an empty caret is at least one pixel wide',
    );
    for (final bad in <Map<String, dynamic>>[
      {'type': 'caret'},
      {'type': 'caret', 'has': 1},
      _report(fields: {'x': -1}),
      _report(fields: {'x': 1.5}),
      _report(fields: {'sw': 1}),
      _report(fields: {'h': 70000}),
      {..._report()}..remove('sh'),
      {'type': 'other', 'has': false},
    ]) {
      expect(RemoteCaret.parse(bad), isNull, reason: '$bad');
    }
    expect(caretWatchMessage(true), {'type': 'caret.watch', 'on': true});
    expect(caretWatchMessage(false), {'type': 'caret.watch', 'on': false});
  });

  test('the broker grant is exact and only follows a request', () {
    final origin = Uri.parse(_origin);
    expect(
      BrokerConnection.fromJson(
        _connected(granted: true),
        origin,
        requestedCaret: true,
      ).caret,
      isTrue,
    );
    expect(BrokerConnection.fromJson(_connected(), origin).caret, isFalse);
    expect(
      BrokerConnection.fromJson(
        _connected(),
        origin,
        requestedCaret: true,
      ).caret,
      isFalse,
      reason: 'requested from an old Host: an ordinary session',
    );
    for (final (data, requested) in [
      (_connected(granted: true), false),
      ({..._connected(), 'caretProtocol': 'caret-v2'}, true),
      ({..._connected(), 'caretProtocol': true}, true),
    ]) {
      expect(
        () =>
            BrokerConnection.fromJson(data, origin, requestedCaret: requested),
        throwsA(isA<BrokerException>()),
      );
    }
    expect(
      BrokerDesktop.fromJson(_desktop({'caretProtocol': 'caret-v1'})).caret,
      isTrue,
    );
    expect(BrokerDesktop.fromJson(_desktop({})).caret, isFalse);
    expect(
      BrokerDesktop.fromJson(_desktop({'caretProtocol': 'caret-v2'})).caret,
      isFalse,
    );
  });

  test(
    'HTTP: requested only toward an advertising inventory (old Host)',
    () async {
      Future<(Map, BrokerConnection)> run(
        Map<String, Object> caps,
        Map<String, dynamic> reply, {
        bool requestCaret = true,
      }) async {
        final client = ApiClient()
          ..responses.add((
            200,
            {
              'desktops': [_desktop(caps)],
            },
          ))
          ..responses.add((200, reply));
        final api = HttpBrokerApi(
          origin: Uri.parse(_origin),
          client: client,
          requestCaret: requestCaret,
        );
        await api.desktops('fixture');
        final connection = await api.connect('fixture', 'pve-149');
        return (
          jsonDecode(client.requests.last.body.toString()) as Map,
          connection,
        );
      }

      const released = {'service': true, 'streaming': true};
      final legacy = await run(released, _connected());
      expect(legacy.$1.containsKey('caretProtocol'), isFalse);
      expect(legacy.$2.caret, isFalse);
      const capable = {...released, 'caretProtocol': 'caret-v1'};
      final caret = await run(capable, _connected(granted: true));
      expect(caret.$1['caretProtocol'], 'caret-v1');
      expect(caret.$2.caret, isTrue);
      final desktop = await run(capable, _connected(), requestCaret: false);
      expect(desktop.$1.containsKey('caretProtocol'), isFalse);
      // A released broker's preflight refusal: retried without the field.
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [_desktop(capable)],
          },
        ))
        ..responses.add((
          400,
          {
            'error': {'code': 'unexpected_field'},
          },
        ))
        ..responses.add((200, _connected()));
      final api = HttpBrokerApi(
        origin: Uri.parse(_origin),
        client: client,
        requestCaret: true,
      );
      await api.desktops('fixture');
      final retried = await api.connect('fixture', 'pve-149');
      final body = jsonDecode(client.requests.last.body.toString()) as Map;
      expect(body.containsKey('caretProtocol'), isFalse);
      expect(retried.caret, isFalse);
    },
  );

  group('session', () {
    Future<(RemoteSession, SessionSocket, SessionMedia)> open({
      bool granted = true,
    }) async {
      final socket = SessionSocket(), media = SessionMedia();
      final session = RemoteSession(
        connection: BrokerConnection.fromJson(
          _connected(granted: granted),
          Uri.parse(_origin),
          requestedCaret: granted,
        ),
        token: 'not-persisted',
        media: media,
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await flush();
      socket.incoming.add(jsonEncode(info));
      await flush();
      socket.incoming.add(packet());
      await flush();
      return (session, socket, media);
    }

    List<Map<String, dynamic>> watches(SessionSocket socket) =>
        socket.sent.where((m) => m['type'] == 'caret.watch').toList();

    test(
      'watch is sent once per change; reports arrive only while watching',
      () async {
        final (session, socket, media) = await open();
        expect(session.caretSupported, isTrue);
        socket.incoming.add(jsonEncode(_report()));
        await flush();
        expect(session.caretUpdates.value, isNull, reason: 'not watching yet');
        expect(session.watchCaret(true), isTrue);
        expect(session.watchCaret(true), isTrue);
        expect(watches(socket), [
          {'type': 'caret.watch', 'on': true},
        ]);
        socket.incoming.add(jsonEncode(_report()));
        await flush();
        final caret = session.caretUpdates.value!;
        expect(caret.x, 100);
        expect(caret.at, isNotNull);
        // A report of another session is ignored; a malformed one ends it.
        socket.incoming.add(
          jsonEncode(_report(session: 'other', fields: {'x': 5})),
        );
        await flush();
        expect(session.caretUpdates.value!.x, 100);
        socket.incoming.add(
          jsonEncode({
            'type': 'caret',
            'sessionId': 'caret-session',
            'has': false,
          }),
        );
        await flush();
        expect(session.caretUpdates.value!.has, isFalse);
        expect(session.watchCaret(false), isTrue);
        expect(session.caretUpdates.value, isNull);
        expect(watches(socket).last, {'type': 'caret.watch', 'on': false});
        await session.close();
        expect(session.watchCaret(true), isFalse);
        await media.events.close();
      },
    );

    test('an old Host or ungranted session has no caret', () async {
      final (session, socket, media) = await open(granted: false);
      expect(session.caretSupported, isFalse);
      expect(session.watchCaret(true), isFalse);
      expect(watches(socket), isEmpty);
      socket.incoming.add(jsonEncode(_report()));
      await flush();
      expect(session.caretUpdates.value, isNull);
      await session.close();
      await media.events.close();
    });
  });

  group('keyboard camera', () {
    // A 1080x2400 picture in a 1080x2400 area; the keyboard and key row cover
    // the bottom 1000 px, so 1400 px remain visible.
    const area = Rect.fromLTWH(0, 0, 1080, 2400);
    const visible = Rect.fromLTWH(0, 0, 1080, 1400);
    AndroidPictureView fresh() => AndroidPictureView(area: area, fitted: area);
    Rect caretAt(double y, {double x = .1, double h = .0125}) =>
        Rect.fromLTWH(x, y, .002, h);
    double screenBottom(AndroidPictureView v, Rect fractions) =>
        v.fractionRect(fractions).bottom;

    test('a caret below the keyboard is lifted into view', () {
      final camera = AndroidKeyboardCamera();
      camera.caret(caretAt(.9)); // y = 2160, hidden by the keyboard
      final view = camera.place(
        fresh(),
        visible,
        const Offset(.5, .1),
        fresh: true,
      );
      final bottom = screenBottom(view, caretAt(.9));
      expect(bottom, lessThanOrEqualTo(visible.bottom - 56 + .5));
      expect(view.fractionRect(caretAt(.9)).top, greaterThan(visible.top));
      expect(camera.open, isTrue);
    });

    test(
      'a caret moving inside the comfort band does not move the picture',
      () {
        final camera = AndroidKeyboardCamera();
        camera.caret(caretAt(.3));
        var view = camera.place(
          fresh(),
          visible,
          const Offset(.5, .3),
          fresh: true,
        );
        final first = view;
        for (final y in [.31, .32, .33, .34, .33, .31]) {
          camera.caret(caretAt(y));
          view = camera.place(view, visible, const Offset(.5, .3));
          expect(view.rect, first.rect, reason: 'moved for caret y=$y');
        }
      },
    );

    test('a caret leaving the band moves the picture once, then holds', () {
      final camera = AndroidKeyboardCamera();
      camera.caret(caretAt(.3));
      var view = camera.place(
        fresh(),
        visible,
        const Offset(.5, .3),
        fresh: true,
      );
      final before = view.rect;
      // Typing down: the caret passes the lower band edge.
      camera.caret(caretAt(.9)); // 2160 px, under the keyboard's lower band
      view = camera.place(view, visible, const Offset(.5, .3));
      expect(view.rect.top, lessThan(before.top), reason: 'followed it up');
      final moved = view.rect;
      final b = view.fractionRect(caretAt(.9));
      expect(b.bottom, lessThanOrEqualTo(visible.bottom - 56 + .5));
      // The next lines stay inside the band: no further movement.
      for (final y in [.9125, .925, .9375]) {
        camera.caret(caretAt(y));
        view = camera.place(view, visible, const Offset(.5, .3));
        expect(view.rect, moved, reason: 'jittered for y=$y');
      }
    });

    test('a caret off to the side is brought in horizontally', () {
      final camera = AndroidKeyboardCamera();
      // Zoomed 2x: the right half is off screen.
      var view = fresh().zoomTo(
        2,
        const Offset(0, 0),
        Offset.zero,
        visible: visible,
      );
      camera.caret(caretAt(.1, x: .1));
      view = camera.place(view, visible, const Offset(.1, .1), fresh: true);
      camera.caret(caretAt(.1, x: .8));
      view = camera.place(view, visible, const Offset(.1, .1));
      final r = view.fractionRect(caretAt(.1, x: .8));
      expect(r.left, greaterThanOrEqualTo(0));
      expect(r.right, lessThanOrEqualTo(visible.right));
    });

    test(
      'a move by hand suspends following until a clear jump out of view',
      () {
        final camera = AndroidKeyboardCamera();
        camera.caret(caretAt(.05, x: .1));
        var view = camera.place(
          fresh().zoomTo(2, Offset.zero, Offset.zero, visible: visible),
          visible,
          const Offset(.1, .05),
          fresh: true,
        );
        expect(camera.following, isTrue);
        // The user pans a little; the caret is still on screen.
        view = view.panned(const Offset(-100, 0), visible: visible);
        camera.moved(byHand: true);
        expect(camera.following, isFalse);
        final panned = view.rect;
        // Typing: the caret creeps along (no jump), then leaves the screen.
        camera.caret(caretAt(.05, x: .12));
        view = camera.place(view, visible, const Offset(.1, .05));
        expect(view.rect, panned);
        // A jump that stays on screen does not resume.
        camera.caret(caretAt(.1, x: .3));
        view = camera.place(view, visible, const Offset(.1, .05));
        expect(
          camera.following,
          isFalse,
          reason: 'still visible: no jump out of view',
        );
        expect(view.rect, panned);
        // A jump out of view resumes and brings the caret in.
        camera.caret(caretAt(.9, x: .95));
        view = camera.place(view, visible, const Offset(.1, .05));
        expect(camera.following, isTrue);
        expect(
          visible.overlaps(view.fractionRect(caretAt(.9, x: .95))),
          isTrue,
        );
      },
    );
    test('a tap resumes following and reveals the tapped field', () {
      final camera = AndroidKeyboardCamera();
      camera.caret(caretAt(.3));
      var view = camera.place(
        fresh(),
        visible,
        const Offset(.5, .3),
        fresh: true,
      );
      camera.moved(byHand: true);
      expect(camera.following, isFalse);
      camera.tapped(const Offset(.5, .9)); // a lower field, under the keyboard
      expect(camera.following, isTrue);
      expect(camera.caretFraction, isNull, reason: 'the old caret is stale');
      view = camera.place(view, visible, const Offset(.5, .3));
      final p = view.point(const Offset(.5, .9));
      expect(p.dy, lessThanOrEqualTo(visible.bottom - 56 + .5));
      expect(p.dy, greaterThan(visible.top));
    });

    test('without a caret (old Host) later taps are followed', () {
      final camera = AndroidKeyboardCamera();
      var view = camera.place(
        fresh(),
        visible,
        const Offset(.5, .2),
        fresh: true,
      );
      final first = view.rect;
      camera.tapped(const Offset(.5, .22)); // inside the band: no move
      view = camera.place(view, visible, const Offset(.5, .2));
      expect(view.rect, first);
      camera.tapped(const Offset(.5, .8));
      view = camera.place(view, visible, const Offset(.5, .2));
      expect(
        view.point(const Offset(.5, .8)).dy,
        lessThanOrEqualTo(visible.bottom - 56 + .5),
      );
      final settled = view.rect;
      // The reveal happens once: a rebuild does not drag the picture again.
      view = camera.place(view, visible, const Offset(.5, .2));
      expect(view.rect, settled);
    });

    test('a hand move without a caret keeps the cursor from pulling back', () {
      final camera = AndroidKeyboardCamera();
      var view = camera.place(
        fresh(),
        visible,
        const Offset(.5, .2),
        fresh: true,
      );
      view = view.panned(const Offset(0, 200), visible: visible);
      camera.moved(byHand: true);
      final panned = view.rect;
      view = camera.place(view, visible, const Offset(.5, .2));
      expect(view.rect, panned);
      expect(camera.manual, isTrue);
    });

    test('closing the keyboard returns the picture to where it was', () {
      final camera = AndroidKeyboardCamera();
      final rest = fresh();
      camera.caret(caretAt(.9));
      var view = camera.place(rest, visible, const Offset(.5, .1), fresh: true);
      expect(view.rect, isNot(rest.rect));
      camera.caret(caretAt(.95));
      view = camera.place(view, visible, const Offset(.5, .1));
      view = camera.place(view, area, const Offset(.5, .1));
      expect(view.rect, rest.rect);
      expect(camera.open, isFalse);
      // The next keyboard starts from scratch: following again.
      expect(camera.following, isTrue);
    });
  });
}
