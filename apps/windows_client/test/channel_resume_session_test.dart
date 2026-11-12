// channel-resume-v1 in the session owner (remote_session.dart): printing,
// shared folders and USB devices come back by themselves after the stream
// resumed, for a client-side and a Host-side outage; a refusal, a revoked
// permission and an old broker or Host leave them ended as before; and the
// ended channel says "recovering" exactly while the app is bringing it back,
// which is what keeps the "connect again" notices away. Fake sockets, files
// and USB backend; no relay, Host, disk or device.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/channel_redial.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/print_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/ui/drive_panel.dart';

import 'drive_session_test.dart' show FakeFiles;
import 'remote_session_test.dart' show SessionSocket;
import 'session_recovery_test.dart' show RecoveryMedia, connected;
import 'usb_redirection_test.dart' show FakeBackend;

const _id = 'qa-session';
Uri _url(String leaf) =>
    Uri.parse('wss://fixture.invalid/api/v1/sessions/$_id$leaf');

class _Print extends PrintSession {
  _Print(this.folder, this.dialFailure, this.pairs)
    : super(url: _url('/print'), token: 'fixture-only', sessionId: _id);

  /// The other side dials too (false: this channel waits for a pair in vain).
  final bool pairs;

  /// Null for the session's first channel, its own folder for a later one.
  final String? folder;

  /// What the dial throws (null: it pairs).
  final Object? dialFailure;
  int starts = 0;
  @override
  Future<void> start() async {
    starts++;
    final failure = dialFailure;
    if (failure == null) {
      ready = pairs;
    } else {
      dialRefused = channelDialRefused(failure);
      closed = true;
      error = 'print_transport_closed';
    }
    notifyListeners();
  }

  @override
  Future<void> close() async {
    closed = true;
    ready = false;
  }

  void fail(String code, {int lost = 0}) {
    closed = true;
    ready = false;
    error = code;
    lostDocuments = lost;
    notifyListeners();
  }
}

class _Drive extends DriveSession {
  _Drive(this.pairs)
    : super(
        url: _url('/drive'),
        token: 'fixture-only',
        sessionId: _id,
        files: FakeFiles(),
      );
  final bool pairs;
  int starts = 0;
  DriveSession? inherited;
  @override
  void inherit(DriveSession previous) {
    inherited = previous;
    super.inherit(previous);
  }

  @override
  Future<void> start() async {
    starts++;
    ready = pairs;
    notifyListeners();
  }

  @override
  Future<void> close({String reason = 'drive_session_closed'}) async {
    closed = true;
    ready = false;
    error = reason;
  }

  void fail(String code, {bool refused = false}) {
    closed = true;
    ready = false;
    error = code;
    dialRefused = refused;
    notifyListeners();
  }
}

class _Usb extends UsbSession {
  _Usb(this.pairs)
    : super(
        url: _url('/usb'),
        token: 'fixture-only',
        sessionId: _id,
        backend: FakeBackend(),
      );
  final bool pairs;
  bool ended = false, up = false;
  int starts = 0;
  List<String>? offeredAgain;

  /// The ended channel still gives its devices back to the PC.
  Completer<void>? releasing;
  @override
  bool get closed => ended;
  @override
  bool get ready => up && !ended;
  @override
  Future<void> get settled => releasing?.future ?? Future<void>.value();
  @override
  Future<void> start() async {
    starts++;
    up = pairs;
    notifyListeners();
  }

  @override
  void resume(Iterable<String> ids) => offeredAgain = ids.toList();
  @override
  Future<void> close({String reason = 'usb_session_closed'}) async {
    ended = true;
  }

  void fail(
    String code, {
    List<String> shared = const [],
    bool refused = false,
  }) {
    ended = true;
    error = code;
    sharedAtEnd = shared;
    dialRefused = refused;
    notifyListeners();
  }
}

class _Fixture {
  _Fixture({this.granted = true});

  /// The broker answered with channel-resume-v1 (a new broker and Host).
  final bool granted;
  Completer<WebSocket>? redial;
  final socket = SessionSocket(), media = RecoveryMedia();
  final drives = <_Drive>[], usbs = <_Usb>[], prints = <_Print>[];

  /// What the next print dial throws (null: it pairs).
  Object? printDialFailure;

  /// The Host dials its side too (false: a new channel never gets its pair).
  bool pairing = true;
  late final current = RemoteSession(
    connection: BrokerConnection(
      sessionId: _id,
      webSocketUrl: _url(''),
      printWebSocketUrl: _url('/print'),
      driveWebSocketUrl: _url('/drive'),
      usbWebSocketUrl: _url('/usb'),
      sessionResume: true,
      resumeGraceMs: 15000,
    ),
    token: 'fixture-only',
    media: media,
    dialer: (_, _) async => socket,
    resumeDialer: (_, _, _) => (redial = Completer<WebSocket>()).future,
    printFactory: (_, _, folder) {
      final fresh = _Print(folder, printDialFailure, pairing);
      prints.add(fresh);
      return fresh;
    },
    driveFactory: (_, _) {
      final fresh = _Drive(pairing);
      drives.add(fresh);
      return fresh;
    },
    usbFactory: (_, _) {
      final fresh = _Usb(pairing);
      usbs.add(fresh);
      return fresh;
    },
  );

  String message(String type, {String? token}) => jsonEncode({
    'type': type,
    'sessionId': _id,
    'token': ?token,
    if (token != null) 'graceMs': 15000,
    if (token != null && granted) 'channelResumeProtocol': 'channel-resume-v1',
  });

  Future<void> start(WidgetTester t) async {
    await connected(t, current, socket);
    socket.incoming.add(message('session.resume.token', token: 'A' * 43));
    await t.pump();
    await t.pump();
    expect(current.frameReady, isTrue);
    expect(current.channelResume, granted);
    expect(drives, hasLength(1));
    expect(usbs, hasLength(1));
    expect(prints, hasLength(1));
    expect(prints.single.folder, isNull);
    expect(current.printer!.ready, isTrue);
  }

  /// Every side channel ends with its own `<kind>_<suffix>` reason.
  Future<void> allFail(WidgetTester t, String suffix) async {
    drives.last.fail('drive_$suffix');
    usbs.last.fail('usb_$suffix', shared: ['stick-1']);
    prints.last.fail('print_$suffix');
    await t.pump();
  }

  int get created => drives.length + usbs.length + prints.length;
  bool get anyRecovering =>
      current.printer!.recovering ||
      current.drive!.recovering ||
      current.usb!.recovering;
  bool get allRecovering =>
      current.printer!.recovering &&
      current.drive!.recovering &&
      current.usb!.recovering;

  Future<void> close() async {
    await current.close();
    await media.events.close();
  }
}

void main() {
  testWidgets(
    'Host-side outage: the channels wait while the stream is away, say they are coming back, and each is dialled again one second after it resumed',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      f.socket.incoming.add(f.message('session.resuming'));
      await t.pump();
      await f.allFail(t, 'stream_away');
      expect(f.allRecovering, isTrue);
      // No "connect again" while the app is bringing them back.
      expect(driveEndedMessage(f.current.drive!), isNull);
      expect(usbEndedMessage(f.current.usb!), isNull);
      await t.pump(const Duration(seconds: 20));
      expect(f.created, 3, reason: 'A dial while the stream is away');
      expect(f.allRecovering, isTrue);
      f.socket.incoming.add(f.message('session.resumed'));
      await t.pump();
      expect(f.current.resuming, isFalse);
      await t.pump(const Duration(milliseconds: 999));
      expect(f.created, 3);
      await t.pump(const Duration(milliseconds: 1));
      await t.pump();
      expect(f.created, 6);
      // The same folders and the devices that were connected then.
      expect(f.drives.last.inherited, same(f.drives.first));
      expect(f.drives.last.starts, 1);
      expect(f.usbs.last.offeredAgain, ['stick-1']);
      expect(f.usbs.last.starts, 1);
      expect(f.current.drive, same(f.drives.last));
      expect(f.current.usb, same(f.usbs.last));
      // The new print channel keeps its documents in a folder of its own.
      expect(f.current.printer, same(f.prints.last));
      expect(f.prints.last.folder, '$_id-r1');
      expect(f.current.printer!.ready, isTrue);
      expect(f.anyRecovering, isFalse);
      expect(f.current.closed, isFalse);
      await f.close();
    },
  );

  testWidgets(
    'Client-side outage: the channels fall with the socket, wait for the new one, then come back',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      unawaited(f.socket.incoming.close()); // dropped without a close frame
      await t.pump();
      await t.pump();
      expect(f.current.resuming, isTrue, reason: f.current.error);
      await f.allFail(t, 'transport_closed');
      expect(f.allRecovering, isTrue);
      await t.pump(const Duration(seconds: 12));
      expect(f.created, 3, reason: 'A dial while the stream is away');
      final second = SessionSocket();
      f.redial!.complete(second);
      await t.pump();
      await t.pump();
      expect(f.current.resuming, isFalse);
      expect(f.current.closed, isFalse, reason: f.current.error);
      expect(f.allRecovering, isTrue);
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(f.created, 6);
      expect(f.drives.last.inherited, same(f.drives.first));
      expect(f.usbs.last.offeredAgain, ['stick-1']);
      // A second outage brings them back again: each resume is a fresh set.
      second.incoming.add(f.message('session.resuming'));
      await t.pump();
      await f.allFail(t, 'stream_away');
      second.incoming.add(f.message('session.resumed'));
      await t.pump();
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(f.created, 9);
      await f.close();
    },
  );

  testWidgets(
    'A channel that keeps failing after the resume gets three attempts (1, 2, 4 s) and then says so: nothing is recovering any more',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      f.socket.incoming.add(f.message('session.resuming'));
      await t.pump();
      await f.allFail(t, 'stream_away');
      f.socket.incoming.add(f.message('session.resumed'));
      await t.pump();
      // The Host never dials its side: each new channel waits for its pair
      // in vain (a pair that worked would start the count over).
      f.pairing = false;
      for (final seconds in [1, 2, 4]) {
        final before = f.created;
        await t.pump(Duration(seconds: seconds));
        await t.pump();
        expect(f.created, before + 3);
        await f.allFail(t, 'pair_timeout');
      }
      await t.pump(const Duration(seconds: 30));
      expect(f.created, 12);
      expect(f.anyRecovering, isFalse);
      // Now the true notices show.
      expect(
        driveEndedMessage(f.current.drive!),
        '공유 폴더 연결이 끊겼어요. 데스크톱에 다시 연결한 뒤 폴더를 다시 고르세요.',
      );
      expect(usbEndedMessage(f.current.usb!), 'USB 연결이 끊겼어요');
      expect(f.current.closed, isFalse);
      await f.close();
    },
  );

  testWidgets(
    'An outage longer than the hold ends the session: nothing is dialled again',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      unawaited(f.socket.incoming.close()); // dropped without a close frame
      await t.pump();
      await t.pump();
      expect(f.current.resuming, isTrue, reason: f.current.error);
      await f.allFail(t, 'transport_closed');
      // The broker no longer has the session: the resume dial is refused.
      f.redial!.completeError(const WebSocketException('gone', 401));
      await t.pump();
      await t.pump();
      expect(f.current.resuming, isFalse);
      expect(f.current.closed, isTrue);
      await t.pump(const Duration(seconds: 30));
      expect(f.created, 3);
      await f.close();
    },
  );

  testWidgets(
    'A permission revoked during the outage stays off: a refused dial and a policy end are never tried again',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      f.socket.incoming.add(f.message('session.resuming'));
      await t.pump();
      await f.allFail(t, 'stream_away');
      f.socket.incoming.add(f.message('session.resumed'));
      await t.pump();
      // The broker answers the print dial 409 (the permission is gone).
      f.printDialFailure = const WebSocketException('refused', 409);
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      await t.pump();
      expect(f.drives, hasLength(2));
      expect(f.usbs, hasLength(2));
      expect(f.prints, hasLength(2));
      expect(f.current.printer!.dialRefused, isTrue);
      expect(f.current.printer!.closed, isTrue);
      expect(f.current.printer!.recovering, isFalse);
      f.printDialFailure = null;
      // The folders' permission is taken away while they are shared again.
      f.drives.last.fail('drive_policy_changed');
      f.usbs.last.fail('usb_stream_away', refused: true);
      await t.pump();
      expect(f.anyRecovering, isFalse);
      expect(driveEndedMessage(f.current.drive!), '관리자가 공유 폴더 권한을 해제했어요.');
      await t.pump(const Duration(seconds: 30));
      expect(f.drives, hasLength(2));
      expect(f.usbs, hasLength(2));
      // Not even the next resume brings them back.
      f.socket.incoming.add(f.message('session.resuming'));
      await t.pump();
      f.socket.incoming.add(f.message('session.resumed'));
      await t.pump();
      await t.pump(const Duration(seconds: 30));
      expect(f.drives, hasLength(2));
      expect(f.usbs, hasLength(2));
      expect(f.prints, hasLength(2));
      await f.close();
    },
  );

  testWidgets(
    'A USB channel is not dialled while the ended one still gives its devices back to the PC',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      f.usbs.single.releasing = Completer<void>();
      f.usbs.single.fail('usb_transport_closed', shared: ['stick-1']);
      await t.pump(const Duration(seconds: 1));
      await t.pump(const Duration(seconds: 3));
      expect(f.usbs, hasLength(1), reason: 'Never two owners of one device');
      expect(f.current.usb!.recovering, isTrue);
      f.usbs.single.releasing!.complete();
      await t.pump();
      await t.pump();
      expect(f.usbs, hasLength(2));
      expect(f.usbs.last.offeredAgain, ['stick-1']);
      await f.close();
    },
  );

  testWidgets(
    'A channel lost while the stream goes on is dialled again too, and a session that closes cancels what was scheduled',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      f.drives.single.fail('drive_transport_closed');
      expect(f.current.drive!.recovering, isTrue);
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(f.drives, hasLength(2));
      expect(f.current.drive!.recovering, isFalse);
      f.drives.last.fail('drive_transport_closed');
      unawaited(f.current.close());
      await t.pump(const Duration(seconds: 30));
      expect(f.drives, hasLength(2));
      await f.close();
    },
  );

  testWidgets(
    'An old broker or Host (no grant): nothing is dialled; the channel is quiet only while the stream is away and ended after it',
    (t) async {
      final f = _Fixture(granted: false);
      await f.start(t);
      f.socket.incoming.add(f.message('session.resuming'));
      await t.pump();
      await f.allFail(t, 'transport_closed');
      expect(f.allRecovering, isTrue, reason: 'No notice during the outage');
      await t.pump(const Duration(seconds: 20));
      expect(f.allRecovering, isTrue);
      f.socket.incoming.add(f.message('session.resumed'));
      await t.pump();
      expect(f.anyRecovering, isFalse);
      await t.pump(const Duration(seconds: 30));
      expect(f.created, 3);
      expect(
        driveEndedMessage(f.current.drive!),
        '공유 폴더 연결이 끊겼어요. 데스크톱에 다시 연결한 뒤 폴더를 다시 고르세요.',
      );
      await f.close();
    },
  );

  testWidgets(
    'An old broker or Host: a channel that ends a moment before the stream says it is away is not announced as lost',
    (t) async {
      final f = _Fixture(granted: false);
      await f.start(t);
      await f.allFail(t, 'transport_closed');
      expect(f.allRecovering, isTrue);
      await t.pump(const Duration(seconds: 1));
      f.socket.incoming.add(f.message('session.resuming'));
      await t.pump();
      await t.pump(const Duration(seconds: 10));
      expect(f.allRecovering, isTrue);
      expect(f.created, 3);
      await f.close();
    },
  );

  testWidgets(
    'An old broker or Host: a channel lost while the stream goes on is announced after a moment, as before',
    (t) async {
      final f = _Fixture(granted: false);
      await f.start(t);
      await f.allFail(t, 'transport_closed');
      await t.pump(const Duration(seconds: 2));
      await t.pump();
      expect(f.anyRecovering, isFalse);
      expect(f.created, 3);
      expect(usbEndedMessage(f.current.usb!), 'USB 연결이 끊겼어요');
      await f.close();
    },
  );

  testWidgets(
    'A document in flight when the printer fell is counted on the ended channel, which says it is coming back',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      final first = f.prints.single;
      first.fail('print_transport_closed', lost: 2);
      // What the views read: lost documents on a channel that is recovering.
      expect(f.current.printer, same(first));
      expect(first.recovering, isTrue);
      expect(first.lostDocuments, 2);
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(f.prints, hasLength(2));
      expect(f.current.printer, same(f.prints.last));
      expect(first.recovering, isFalse);
      await f.close();
    },
  );

  test('the documents lost with a printer that is coming back are named '
      'without asking to connect again', () {
    final notice = printLostNotice(2);
    expect(notice, '인쇄하던 문서 2개를 받지 못했어요. 데스크톱에서 다시 인쇄해 주세요.');
    expect(notice.contains('다시 연결'), isFalse);
  });
}
