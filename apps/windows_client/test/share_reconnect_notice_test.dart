// Review 2026-11-12: after a client-side reconnect (다시 연결, a new quality,
// another screen mode) the USB devices came back with a line, while the
// shared folders were gone without a word. A new session never shares a
// folder by itself, so the window says once that their sharing ended; and two
// "ended" notices at the same moment are shown together, not only the last.
// RemoteView with memory sessions (as usb_reconnect_test); no file system,
// device or window operation.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'drive_session_test.dart' show FakeFiles;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;
import 'stream_display_entry_test.dart' show entryMachine;
import 'usb_redirection_test.dart' show FakeBackend;

const _id = '0b7d6f5c-3f9e-4f69-9d8c-2c1d6b8f0a12';
final _url = Uri.parse('wss://fixture.invalid/api/v1/sessions/$_id');
const _ended = '다시 연결하면서 폴더 공유를 끝냈어요. 폴더·USB에서 다시 추가하세요.';

/// Shared folders whose channel is a memory fake.
class _Drive extends DriveSession {
  _Drive({this.folders = 0})
    : super(url: _url, token: 'fixture', sessionId: _id, files: FakeFiles());
  final int folders;
  @override
  Future<void> start() async {
    ready = true;
    notifyListeners();
  }

  /// The administrator takes the permission away while the stream goes on
  /// (an end that is not dialled again).
  void revoke() {
    closed = true;
    error = 'drive_policy_changed';
    notifyListeners();
  }

  @override
  Future<void> close({String reason = 'drive_session_closed'}) async {
    sharesAtEnd = [
      for (var i = 0; i < folders; i++)
        DriveShare(id: 'share-$i', path: 'C:\\Fixture\\$i', label: 'f$i'),
    ];
    closed = true;
    error ??= reason;
    notifyListeners();
  }
}

class _Usb extends UsbSession {
  _Usb()
    : super(
        url: _url,
        token: 'fixture',
        sessionId: _id,
        backend: FakeBackend(),
      );
  bool ended = false;
  @override
  bool get closed => ended;
  @override
  Future<void> start() async => notifyListeners();
  void revoke() {
    ended = true;
    error = 'usb_policy_changed';
    notifyListeners();
  }

  /// The line about the devices offered again after a reconnect.
  void announce(String text) {
    resumeNotice = text;
    resumeFailed = false;
    resumeNotices++;
    notifyListeners();
  }

  @override
  Future<void> close({String reason = 'usb_session_closed'}) async {
    ended = true;
    notifyListeners();
  }
}

class _Session {
  _Session({int? folders, bool usb = false})
    : drive = folders == null ? null : _Drive(folders: folders),
      usb = usb ? _Usb() : null;
  final _Drive? drive;
  final _Usb? usb;
  final socket = SessionSocket(), media = SessionMedia();
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: _id,
      webSocketUrl: _url,
      driveWebSocketUrl: drive == null ? null : _url,
      usbWebSocketUrl: usb == null ? null : _url,
    ),
    token: 'fixture-only',
    media: media,
    dialer: (_, _) async => socket,
    driveFactory: drive == null ? null : (_, _) => drive!,
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

  Future<void> show(
    WidgetTester tester,
    _Session first,
    List<_Session> made,
    _Session Function() next,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1366, 768);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
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
  }

  Future<void> reconnect(
    WidgetTester tester,
    _Session first,
    List<_Session> made,
  ) async {
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
    await tester.pump();
    await tester.pump();
  }

  Future<void> end(
    WidgetTester tester,
    _Session first,
    List<_Session> made,
  ) async {
    // The view closes its session on unmount; its bounded steps run on the
    // test clock.
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

  testWidgets(
    'folders shared before a reconnect: the window says once that their sharing ended',
    (tester) async {
      final first = _Session(folders: 2);
      final made = <_Session>[];
      await show(tester, first, made, () => _Session(folders: 0));
      try {
        await reconnect(tester, first, made);
        expect(find.text(_ended), findsOneWidget);
        // Once: the next builds do not say it again.
        await tester.pump(const Duration(seconds: 7));
        await tester.pump();
        expect(find.text(_ended), findsNothing);
      } finally {
        await end(tester, first, made);
      }
    },
  );

  testWidgets(
    'after a reconnect the folders line is not replaced by the USB line: both are shown',
    (tester) async {
      final first = _Session(folders: 1, usb: true);
      final made = <_Session>[];
      await show(tester, first, made, () => _Session(folders: 0, usb: true));
      try {
        await reconnect(tester, first, made);
        expect(find.text(_ended), findsOneWidget);
        made.single.usb!.announce('USB 장치를 다시 연결했어요');
        await tester.pump();
        await tester.pump();
        expect(find.text('$_ended\nUSB 장치를 다시 연결했어요'), findsOneWidget);
      } finally {
        await end(tester, first, made);
      }
    },
  );

  testWidgets('no folder was shared: nothing is said', (tester) async {
    final first = _Session(folders: 0);
    final made = <_Session>[];
    await show(tester, first, made, () => _Session(folders: 0));
    try {
      await reconnect(tester, first, made);
      expect(find.text(_ended), findsNothing);
    } finally {
      await end(tester, first, made);
    }
  });

  testWidgets(
    'shared folders and USB ending at the same moment are both said, one line each',
    (tester) async {
      final first = _Session(folders: 1, usb: true);
      final made = <_Session>[];
      await show(tester, first, made, () => _Session(folders: 0));
      try {
        first.usb!.revoke();
        first.drive!.revoke();
        await tester.pump();
        await tester.pump();
        // In the order they ended, one line each.
        expect(
          find.text('관리자가 USB 권한을 해제했어요.\n관리자가 공유 폴더 권한을 해제했어요.'),
          findsOneWidget,
        );
        expect(find.byIcon(Icons.info_outline_rounded), findsOneWidget);
      } finally {
        await end(tester, first, made);
      }
    },
  );
}
