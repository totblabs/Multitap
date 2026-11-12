// After a stream reconnect the USB devices shared before are offered again
// (2026-11-02), and the window says in one line how it went, never silently.
// UsbSession with the fakes of usb_redirection_test.dart; RemoteView with
// memory sessions. No device, driver or window operation.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;
import 'stream_display_entry_test.dart' show entryMachine;
import 'usb_redirection_test.dart'
    show
        FakeBackend,
        FakeSocket,
        ready,
        sessionId,
        settle,
        shared,
        started,
        stick;

const _id = '0b7d6f5c-3f9e-4f69-9d8c-2c1d6b8f0a12';
final _url = Uri.parse('wss://fixture.invalid/api/v1/sessions/$_id');

/// A UsbSession whose channel is a memory fake: what it was asked to resume.
class _Usb extends UsbSession {
  _Usb({this.endedWith = const []})
    : super(
        url: _url,
        token: 'fixture',
        sessionId: _id,
        backend: FakeBackend(),
      );
  final List<String> endedWith;
  final resumed = <List<String>>[];
  bool ended = false;
  @override
  bool get closed => ended;
  @override
  Future<void> start() async => notifyListeners();
  @override
  void resume(Iterable<String> ids) => resumed.add(ids.toList());
  void announce(String text, {bool failed = false}) {
    resumeNotice = text;
    resumeFailed = failed;
    resumeNotices++;
    notifyListeners();
  }

  @override
  Future<void> close({String reason = 'usb_session_closed'}) async {
    sharedAtEnd = endedWith;
    ended = true;
    notifyListeners();
  }
}

class _Session {
  _Session({required bool usb, List<String> endedWith = const []})
    : usb = usb ? _Usb(endedWith: endedWith) : null;
  final _Usb? usb;
  final socket = SessionSocket(), media = SessionMedia();
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: _id,
      webSocketUrl: _url,
      usbWebSocketUrl: usb == null ? null : _url,
    ),
    token: 'fixture-only',
    media: media,
    dialer: (_, _) async => socket,
    usbFactory: usb == null ? null : (_, _) => usb!,
  );
  Future<void> picture(WidgetTester tester) async {
    socket.incoming.add(jsonEncode(info));
    await tester.pump();
    await tester.pump();
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    expect(session.frameReady, isTrue);
  }
}

void main() {
  group('UsbSession', () {
    test('the devices shared when the channel ends are remembered, not the ones given back before', () async {
      final (session, socket, backend) = await started(
        devices: [
          stick(),
          stick(id: 'other'),
        ],
      );
      final (entry, _) = await shared(session, socket, backend);
      expect(entry.device.id, 'stick');
      await session.close();
      expect(session.sharedAtEnd, ['stick']);
    });

    test(
      'a device shared before is offered again and the line says so',
      () async {
        final (session, socket, backend) = await started();
        session.resume(['stick']);
        final attach = await socket.text('usb.attach');
        expect(session.resumeNotices, 0, reason: 'not before the VM took it');
        socket.emit({'type': 'usb.attached', 'device': attach['device']});
        for (var i = 0; i < 200 && session.resumeNotices == 0; i++) {
          await Future<void>.delayed(const Duration(milliseconds: 1));
        }
        expect(backend.captures, hasLength(1));
        expect(session.entries.single.state, UsbEntryState.shared);
        expect(session.resumeNotices, 1);
        expect(session.resumeFailed, isFalse);
        expect(session.resumeNotice, 'USB 장치를 다시 연결했어요');
        await session.close();
      },
    );

    test(
      'resume before the channel is up waits for it and its first listing',
      () async {
        final socket = FakeSocket();
        final backend = FakeBackend()..devices = [stick()];
        final session = UsbSession(
          url: _url,
          token: 'token',
          sessionId: sessionId,
          backend: backend,
          dialer: (url, token) async => socket,
        );
        await session.start();
        session.resume(['stick']);
        await settle();
        expect(socket.texts, isEmpty);
        socket.emit(ready);
        await settle();
        expect(
          socket.texts,
          isEmpty,
          reason: 'not before the VM can take devices',
        );
        socket.emit({'type': 'usb.status', 'state': 'ready'});
        final attach = await socket.text('usb.attach');
        expect(attach['device'], 1);
        await session.close();
      },
    );

    test('an unplugged device, a refusal or a VM that cannot take devices is said in one line', () async {
      // Unplugged meanwhile.
      var (session, socket, backend) = await started();
      session.resume(['gone']);
      expect(session.resumeNotices, 1);
      expect(session.resumeFailed, isTrue);
      expect(
        session.resumeNotice,
        'USB 장치를 다시 연결하지 못했어요. ${usbMessage('usb_device_removed')}',
      );
      expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
      await session.close();
      // No longer allowed: the Host or the broker refuses it.
      (session, socket, backend) = await started();
      session.resume(['stick']);
      final attach = await socket.text('usb.attach');
      socket.emit({
        'type': 'usb.rejected',
        'device': attach['device'],
        'reason': 'usb_class_not_granted',
      });
      await settle();
      expect(session.resumeNotices, 1);
      expect(
        session.resumeNotice,
        contains(usbMessage('usb_class_not_granted')),
      );
      expect(backend.captures, isEmpty);
      await session.close();
      // The VM cannot take devices now.
      (session, socket, backend) = await started(hostReady: false);
      session.resume(['stick']);
      socket.emit({
        'type': 'usb.status',
        'state': 'unavailable',
        'reason': 'usb_vhci_unavailable',
      });
      await settle();
      expect(session.resumeNotices, 1);
      expect(
        session.resumeNotice,
        contains(usbMessage('usb_vhci_unavailable')),
      );
      await session.close();
    });
  });

  group('RemoteView', () {
    setUp(() {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window'),
        (_) async => false,
      );
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window/events'),
        (_) async => null,
      );
    });
    tearDown(() {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window'),
        null,
      );
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window/events'),
        null,
      );
    });

    Future<void> run(
      WidgetTester tester,
      _Session first,
      _Session Function() next,
      Future<void> Function(_Session second) check,
    ) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1366, 768);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final made = <_Session>[];
      try {
        await tester.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: entryMachine(1),
              load: () async => first.session,
              onReconnect: (_) async => (made..add(next())).last.session,
              sessionGuideEnabled: false,
            ),
          ),
        );
        await tester.pump();
        await tester.pump();
        await first.picture(tester);
        // The stream ends; 다시 연결 replaces the session in this window.
        first.socket.incoming.add(
          jsonEncode({
            'type': 'stream.unavailable',
            'sessionId': _id,
            'reason': 'worker_failed',
          }),
        );
        await tester.pump();
        await tester.pump();
        await tester.tap(find.byKey(const Key('retry-remote-connection')));
        for (var i = 0; i < 6; i++) {
          await tester.pump();
        }
        expect(made, hasLength(1));
        await made.single.picture(tester);
        await check(made.single);
      } finally {
        // The view closes its session on unmount; its bounded steps run on
        // the test clock.
        await tester.pumpWidget(const SizedBox.shrink());
        for (var i = 0; i < 4; i++) {
          await tester.pump(const Duration(seconds: 3));
        }
        await tester.runAsync(() async {
          await first.session.close();
          for (final view in [first, ...made]) {
            await view.media.events.close();
          }
        });
      }
    }

    testWidgets(
      'the next session takes the devices again and says how it went',
      (tester) async {
        final first = _Session(usb: true, endedWith: ['stick']);
        await run(tester, first, () => _Session(usb: true), (second) async {
          expect(second.usb!.resumed, [
            ['stick'],
          ]);
          second.usb!.announce('USB 장치를 다시 연결했어요');
          await tester.pump();
          await tester.pump();
          expect(find.text('USB 장치를 다시 연결했어요'), findsOneWidget);
        });
      },
    );

    testWidgets(
      'a next session without USB says the devices did not come along',
      (tester) async {
        final first = _Session(usb: true, endedWith: ['stick']);
        await run(tester, first, () => _Session(usb: false), (_) async {
          await tester.pump();
          await tester.pump();
          expect(
            find.text('USB 장치를 다시 연결하지 못했어요. 이 연결에서는 USB를 쓸 수 없어요.'),
            findsOneWidget,
          );
        });
      },
    );

    testWidgets(
      'nothing shared before: nothing is offered and nothing is said',
      (tester) async {
        final first = _Session(usb: true);
        await run(tester, first, () => _Session(usb: true), (second) async {
          expect(second.usb!.resumed, isEmpty);
          expect(find.textContaining('USB 장치를 다시 연결'), findsNothing);
        });
      },
    );
  });
}
