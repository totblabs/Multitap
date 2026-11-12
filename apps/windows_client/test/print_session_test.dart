// print-v1 on the client: the broker grant (only after a request, stream
// sessions only, the session's own print URL), the connect request only toward
// an advertising inventory, the print socket over a real loopback WebSocket
// (in-order chunks, acknowledgements, digest, cancel, refusals, revocation and
// cleanup of every received file), and the print dialog (printer choice,
// remembered printer, copies, save, failure). No printer of this PC is used.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:crypto/crypto.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_print.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/native_print.dart';
import 'package:multitap_client/data/print_session.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/print_dialog.dart';
import 'package:multitap_client/ui/print_progress_notice.dart';

import 'power_api_test.dart' show ApiClient;

const _origin = 'https://fixture.invalid';
const _session = 'print-session';
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
  'sessionId': _session,
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/$_session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) ...{
    'printProtocol': 'print-v1',
    'printWebSocketUrl':
        'wss://fixture.invalid/api/v1/sessions/$_session/print',
  },
};

/// The relay's side of one print socket, on a real loopback WebSocket.
class FakeRelay {
  late final HttpServer server;
  WebSocket? peer;
  final received = <Map<String, dynamic>>[];
  final _arrivals = StreamController<Map<String, dynamic>>.broadcast();
  final _connected = Completer<WebSocket>();

  Future<void> start() async {
    server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    server.listen((request) async {
      expect(
        request.headers.value(HttpHeaders.authorizationHeader),
        'Bearer print-token',
      );
      final socket = await WebSocketTransformer.upgrade(request);
      peer = socket;
      _connected.complete(socket);
      socket.listen((message) {
        final value = jsonDecode(message as String) as Map<String, dynamic>;
        received.add(value);
        _arrivals.add(value);
      }, onDone: () {});
    });
  }

  Future<WebSocket> get connected => _connected.future;
  void send(Map<String, Object?> value) => peer!.add(jsonEncode(value));
  Future<Map<String, dynamic>> next(bool Function(Map<String, dynamic>) test) {
    final index = received.indexWhere(test);
    if (index >= 0) return Future.value(received.removeAt(index));
    return _arrivals.stream
        .firstWhere(test)
        .then((value) {
          received.remove(value);
          return value;
        })
        .timeout(const Duration(seconds: 5));
  }

  Future<void> close() async {
    await peer?.close();
    await server.close(force: true);
    await _arrivals.close();
  }
}

Map<String, Object?> _ready({
  int chunk = 1024,
  int window = 2,
  int max = 1 << 20,
}) => {
  'type': 'print.ready',
  'sessionId': _session,
  'protocol': 'print-v1',
  'limits': {'chunkBytes': chunk, 'windowChunks': window, 'maxJobBytes': max},
};

Future<void> _until(bool Function() done) async {
  for (var i = 0; i < 400 && !done(); i++) {
    await Future<void>.delayed(const Duration(milliseconds: 10));
  }
  expect(done(), isTrue);
}

List<int> _bytes(int length, int seed) {
  final random = Random(seed);
  return List.generate(length, (_) => random.nextInt(256));
}

void main() {
  test('the broker grant is exact and only follows a request', () {
    final origin = Uri.parse(_origin);
    final granted = BrokerConnection.fromJson(
      _connected(granted: true),
      origin,
      requestedPrint: true,
    );
    expect(
      granted.printWebSocketUrl.toString(),
      'wss://fixture.invalid/api/v1/sessions/$_session/print',
    );
    expect(
      BrokerConnection.fromJson(_connected(), origin).printWebSocketUrl,
      isNull,
    );
    expect(
      BrokerConnection.fromJson(
        _connected(),
        origin,
        requestedPrint: true,
      ).printWebSocketUrl,
      isNull,
      reason: 'requested from a denied or released Host: an ordinary session',
    );
    for (final (data, requested) in [
      (_connected(granted: true), false),
      ({..._connected(granted: true), 'printProtocol': 'print-v2'}, true),
      ({..._connected(), 'printProtocol': 'print-v1'}, true),
      (
        {
          ..._connected(),
          'printWebSocketUrl':
              'wss://fixture.invalid/api/v1/sessions/$_session/print',
        },
        true,
      ),
      (
        {
          ..._connected(granted: true),
          'printWebSocketUrl':
              'wss://other.invalid/api/v1/sessions/$_session/print',
        },
        true,
      ),
      (
        {
          ..._connected(granted: true),
          'printWebSocketUrl':
              'wss://fixture.invalid/api/v1/sessions/other/print',
        },
        true,
      ),
      (
        {
          ..._connected(granted: true),
          'printWebSocketUrl':
              'wss://fixture.invalid/api/v1/sessions/$_session/print?x=1',
        },
        true,
      ),
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          data,
          origin,
          requestedPrint: requested,
        ),
        throwsA(isA<BrokerException>()),
        reason: '$data',
      );
    }
    expect(
      BrokerDesktop.fromJson(_desktop({'printProtocol': 'print-v1'})).print,
      isTrue,
    );
    expect(BrokerDesktop.fromJson(_desktop({})).print, isFalse);
    // Why the VM cannot print / share folders: only the two fixed reasons.
    for (final reason in [
      'print_spooler_unavailable',
      'print_driver_unavailable',
    ]) {
      expect(
        BrokerDesktop.fromJson(_desktop({'printUnavailableReason': reason}))
            .printUnavailable,
        reason,
      );
    }
    expect(
      BrokerDesktop.fromJson(_desktop({'printUnavailableReason': 'anything'}))
          .printUnavailable,
      isNull,
    );
    expect(BrokerDesktop.fromJson(_desktop({})).printUnavailable, isNull);
    expect(
      BrokerDesktop.fromJson(
        _desktop({'driveUnavailableReason': 'drive_webclient_unavailable'}),
      ).driveUnavailable,
      'drive_webclient_unavailable',
    );
    expect(
      BrokerDesktop.fromJson(_desktop({'driveUnavailableReason': 'other'}))
          .driveUnavailable,
      isNull,
    );
    expect(
      BrokerDesktop.fromJson(_desktop({'printProtocol': 'print-v2'})).print,
      isFalse,
    );
  });

  test(
    'HTTP: requested only by a printing client from an advertising inventory',
    () async {
      Future<(Map, BrokerConnection)> run(
        Map<String, Object> caps,
        Map<String, dynamic> reply, {
        bool requestPrint = true,
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
          requestPrint: requestPrint,
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
      expect(legacy.$1.containsKey('printProtocol'), isFalse);
      expect(legacy.$2.printWebSocketUrl, isNull);
      const capable = {...released, 'printProtocol': 'print-v1'};
      final printing = await run(capable, _connected(granted: true));
      expect(printing.$1['printProtocol'], 'print-v1');
      expect(printing.$2.printWebSocketUrl, isNotNull);
      final other = await run(capable, _connected(), requestPrint: false);
      expect(other.$1.containsKey('printProtocol'), isFalse);
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
        requestPrint: true,
      );
      await api.desktops('fixture');
      final retried = await api.connect('fixture', 'pve-149');
      expect(
        (jsonDecode(client.requests.last.body.toString()) as Map).containsKey(
          'printProtocol',
        ),
        isFalse,
      );
      expect(retried.printWebSocketUrl, isNull);
    },
  );

  group('print socket', () {
    late FakeRelay relay;
    late Directory storage;
    late PrintSession session;

    setUp(() async {
      relay = FakeRelay();
      await relay.start();
      storage = await Directory.systemTemp.createTemp('multitap-print-test-');
      session = PrintSession(
        url: Uri.parse('wss://fixture.invalid/api/v1/sessions/$_session/print'),
        token: 'print-token',
        sessionId: _session,
        storage: storage,
        dialer: (_, token) => WebSocket.connect(
          'ws://127.0.0.1:${relay.server.port}/print',
          headers: {HttpHeaders.authorizationHeader: 'Bearer $token'},
        ),
      );
    });
    tearDown(() async {
      await session.close();
      await relay.close();
      if (await storage.exists()) await storage.delete(recursive: true);
    });

    Future<void> open() async {
      final started = session.start();
      await relay.connected;
      relay.send(_ready());
      await started;
      expect(session.ready, isTrue);
    }

    Future<List<int>> transfer(
      String id,
      List<int> pdf, {
      String title = '보고서.docx',
      int chunk = 1024,
    }) async {
      relay.send({
        'type': 'print.job',
        'jobId': id,
        'format': 'pdf',
        'size': pdf.length,
        'title': title,
      });
      var offset = 0;
      while (offset < pdf.length) {
        final end = min(offset + chunk, pdf.length);
        relay.send({
          'type': 'print.chunk',
          'jobId': id,
          'offset': offset,
          'data': base64.encode(pdf.sublist(offset, end)),
        });
        final ack = await relay.next(
          (v) => v['type'] == 'print.ack' && v['jobId'] == id,
        );
        expect(ack, {'type': 'print.ack', 'jobId': id, 'offset': end});
        offset = end;
      }
      relay.send({
        'type': 'print.end',
        'jobId': id,
        'sha256': sha256.convert(pdf).toString(),
      });
      return pdf;
    }

    test('a document arrives in order, is verified, waits for the user and is deleted after the choice', () async {
      await open();
      relay.send({'type': 'print.queue', 'state': 'ready'});
      await _until(() => session.queueReady);
      final pdf = [...utf8.encode('%PDF-1.7\n'), ..._bytes(3000, 1)];
      await transfer('job1', pdf);
      await _until(() => session.pending.isNotEmpty);
      final document = session.pending.single;
      expect(document.title, '보고서.docx');
      expect(document.size, pdf.length);
      expect(await document.file.readAsBytes(), pdf);
      expect(document.file.path, startsWith(storage.path));
      expect(document.suggestedName, '보고서.docx.pdf');
      await session.finish(document, PrintOutcome.printed);
      expect(await relay.next((v) => v['type'] == 'print.result'), {
        'type': 'print.result',
        'jobId': 'job1',
        'result': 'printed',
      });
      expect(await document.file.exists(), isFalse);
      expect(session.pending, isEmpty);
      // The end of the session removes its folder.
      final second = [...utf8.encode('%PDF-1.4\n'), ..._bytes(10, 2)];
      await transfer('job2', second, title: '');
      await _until(() => session.pending.isNotEmpty);
      expect(session.pending.single.suggestedName, 'Multitap 인쇄.pdf');
      final kept = session.pending.single.file;
      await session.close();
      expect(await kept.exists(), isFalse);
      expect(
        await Directory('${storage.path}${Platform.pathSeparator}$_session')
            .exists(),
        isFalse,
      );
    });

    test('a digest mismatch is never offered; a cancel deletes the partial file; notices are fixed codes', () async {
      await open();
      relay.send({
        'type': 'print.queue',
        'state': 'unavailable',
        'reason': 'print_name_conflict',
      });
      await _until(() => session.queueReason == 'print_name_conflict');
      final pdf = _bytes(1500, 3);
      relay.send({
        'type': 'print.job',
        'jobId': 'bad',
        'format': 'pdf',
        'size': pdf.length,
        'title': '',
      });
      relay.send({
        'type': 'print.chunk',
        'jobId': 'bad',
        'offset': 0,
        'data': base64.encode(pdf.sublist(0, 1024)),
      });
      relay.send({
        'type': 'print.chunk',
        'jobId': 'bad',
        'offset': 1024,
        'data': base64.encode(pdf.sublist(1024)),
      });
      relay.send({'type': 'print.end', 'jobId': 'bad', 'sha256': '0' * 64});
      expect(await relay.next((v) => v['type'] == 'print.result'), {
        'type': 'print.result',
        'jobId': 'bad',
        'result': 'failed',
      });
      expect(session.pending, isEmpty);
      expect(session.skipped, 'print_job_failed');
      session.clearSkipped();
      relay.send({
        'type': 'print.job',
        'jobId': 'part',
        'format': 'pdf',
        'size': 5000,
        'title': '',
      });
      relay.send({
        'type': 'print.chunk',
        'jobId': 'part',
        'offset': 0,
        'data': base64.encode(_bytes(1024, 4)),
      });
      await relay.next((v) => v['type'] == 'print.ack' && v['jobId'] == 'part');
      relay.send({'type': 'print.cancel', 'jobId': 'part'});
      await _until(() => !session.receiving);
      final folder = Directory(
        '${storage.path}${Platform.pathSeparator}$_session',
      );
      expect(
        folder.listSync().whereType<File>(),
        isEmpty,
        reason: 'the partial file is gone',
      );
      // The Host gave the document up: the user is told it will not come.
      await _until(() => session.skipped == 'print_job_failed');
      relay.send({'type': 'print.skipped', 'reason': 'print_job_too_large'});
      await _until(() => session.skipped == 'print_job_too_large');
      // A document above the relay's limit is refused before any byte.
      relay.send({
        'type': 'print.job',
        'jobId': 'huge',
        'format': 'pdf',
        'size': (1 << 20) + 1,
        'title': '',
      });
      expect(await relay.next((v) => v['type'] == 'print.cancel'), {
        'type': 'print.cancel',
        'jobId': 'huge',
      });
      await transfer('after', [...utf8.encode('%PDF-'), 1, 2, 3]);
      await _until(() => session.pending.length == 1);
      expect(session.closed, isFalse);
    });

    test(
      'a broken order ends printing and deletes what was received',
      () async {
        await open();
        final pdf = _bytes(2048, 5);
        await transfer('kept', [...utf8.encode('%PDF-'), ...pdf]);
        await _until(() => session.pending.length == 1);
        final kept = session.pending.single.file;
        relay.send({
          'type': 'print.job',
          'jobId': 'x',
          'format': 'pdf',
          'size': 100,
          'title': '',
        });
        relay.send({
          'type': 'print.chunk',
          'jobId': 'x',
          'offset': 7,
          'data': base64.encode(_bytes(10, 6)),
        });
        await _until(() => session.closed);
        expect(session.error, 'print_out_of_order');
        expect(session.pending, isEmpty);
        await _until(() => !kept.existsSync());
      },
    );

    test(
      'revocation: the relay error closes printing with its reason',
      () async {
        await open();
        await transfer('waiting', [...utf8.encode('%PDF-'), 9, 9]);
        await _until(() => session.pending.length == 1);
        final waiting = session.pending.single.file;
        relay.send({'type': 'print.error', 'reason': 'print_policy_changed'});
        await _until(() => session.closed);
        expect(session.error, 'print_policy_changed');
        expect(session.pending, isEmpty);
        await _until(() => !waiting.existsSync());
        expect(printCodeMessage('print_policy_changed'), contains('관리자'));
      },
    );

    test('the channel ends while the person is choosing: that document stays until finished, the others are counted as lost', () async {
      await open();
      await transfer('shown', [...utf8.encode('%PDF-'), 1, 2]);
      await transfer('queued', [...utf8.encode('%PDF-'), 3, 4]);
      await _until(() => session.pending.length == 2);
      final shown = session.pending.first, queued = session.pending.last;
      session.hold(shown);
      relay.send({'type': 'print.error', 'reason': 'print_transport_closed'});
      await _until(() => session.closed);
      expect(session.pending, isEmpty);
      expect(session.lostDocuments, 1, reason: 'only the one not on screen');
      expect(await shown.file.exists(), isTrue, reason: 'still printable');
      expect(await queued.file.exists(), isFalse);
      // Printing it afterwards deletes the file and its folder; the Host
      // is gone, so nothing is sent.
      await session.finish(shown, PrintOutcome.printed);
      expect(await shown.file.exists(), isFalse);
      expect(
        await Directory('${storage.path}${Platform.pathSeparator}$_session')
            .exists(),
        isFalse,
      );
    });

    test(
      'a document still arriving when the channel ends counts as lost too',
      () async {
        await open();
        relay.send({
          'type': 'print.job',
          'jobId': 'half',
          'format': 'pdf',
          'size': 5000,
          'title': '',
        });
        relay.send({
          'type': 'print.chunk',
          'jobId': 'half',
          'offset': 0,
          'data': base64.encode(_bytes(1024, 4)),
        });
        await relay.next((v) => v['type'] == 'print.ack');
        relay.send({'type': 'print.error', 'reason': 'print_transport_closed'});
        await _until(() => session.closed);
        expect(session.lostDocuments, 1);
      },
    );

    test('no room to keep a document: it is declined with its own reason, the printer and the next documents stay', () async {
      await open();
      // A file where the session's folder must be: nothing can be stored.
      final blocker = File('${storage.path}${Platform.pathSeparator}$_session');
      await blocker.writeAsString('in the way');
      relay.send({
        'type': 'print.job',
        'jobId': 'nospace',
        'format': 'pdf',
        'size': 100,
        'title': '',
      });
      expect(await relay.next((v) => v['type'] == 'print.cancel'), {
        'type': 'print.cancel',
        'jobId': 'nospace',
      });
      expect(session.skipped, 'print_local_storage_failed');
      expect(session.closed, isFalse, reason: 'not a protocol error');
      expect(printCodeMessage('print_local_storage_failed'), contains('저장 공간'));
      session.clearSkipped();
      await blocker.delete();
      await transfer('next', [...utf8.encode('%PDF-'), 5]);
      await _until(() => session.pending.length == 1);
      expect(session.pending.single.jobId, 'next');
    });

    test('Android silently accepts a ready queue; skipped and lost documents stay actionable', () async {
      await open();
      final notices = <String>[];
      final offers = AndroidPrintOffers(
        bridge: FakeAndroidPrint(),
        onNotice: notices.add,
      )..watch(session);
      addTearDown(offers.dispose);
      relay.send({'type': 'print.queue', 'state': 'ready'});
      await _until(() => session.queueReady);
      expect(notices, isEmpty);
      relay.send({'type': 'print.skipped', 'reason': 'print_job_too_large'});
      await _until(() => notices.length == 1);
      expect(notices.last, printCodeMessage('print_job_too_large'));
      relay.send({'type': 'print.error', 'reason': 'print_transport_closed'});
      await _until(() => notices.length == 2);
      expect(notices.last, contains('연결이 끊겼어요'));
      // The same session never repeats a notice.
      await Future<void>.delayed(const Duration(milliseconds: 50));
      expect(notices, hasLength(2));
    });

    test(
      'Android: each document goes to the system print dialog, one at a time',
      () async {
        await open();
        final bridge = FakeAndroidPrint();
        final offers = AndroidPrintOffers(bridge: bridge)..watch(session);
        addTearDown(offers.dispose);
        await transfer('a1', [...utf8.encode('%PDF-'), 1]);
        await transfer('a2', [...utf8.encode('%PDF-'), 2]);
        await _until(() => bridge.offered.length == 1);
        expect(
          bridge.offered.single,
          'a1',
          reason: 'the second waits for the first',
        );
        bridge.answer.complete(true);
        expect(await relay.next((v) => v['type'] == 'print.result'), {
          'type': 'print.result',
          'jobId': 'a1',
          'result': 'printed',
        });
        await _until(() => bridge.offered.length == 2);
        bridge.answer.complete(false);
        expect(await relay.next((v) => v['type'] == 'print.result'), {
          'type': 'print.result',
          'jobId': 'a2',
          'result': 'cancelled',
        });
        await _until(() => session.pending.isEmpty);
      },
    );

    test(
      'Android native launch failures notify and report failed, not cancelled',
      () async {
        await open();
        final bridge = FakeAndroidPrint();
        final notices = <String>[];
        final offers = AndroidPrintOffers(bridge: bridge, onNotice: notices.add)
          ..watch(session);
        addTearDown(offers.dispose);
        await transfer('native-failure', [...utf8.encode('%PDF-'), 1]);
        await _until(() => bridge.offered.isNotEmpty);
        final file = session.pending.single.file;
        bridge.answer.completeError(
          const AndroidPrintFailure('print_document_invalid'),
        );
        expect(await relay.next((v) => v['type'] == 'print.result'), {
          'type': 'print.result',
          'jobId': 'native-failure',
          'result': 'failed',
        });
        await _until(() => session.pending.isEmpty);
        await _until(() => !file.existsSync());
        expect(notices, ['받은 인쇄 문서를 열지 못했어요. 데스크톱에서 다시 인쇄하세요.']);
        expect(bridge.offered, [
          'native-failure',
        ], reason: 'no silent retry loop');
      },
    );

    test(
      'a late native failure does not notify a different Android session',
      () async {
        await open();
        final bridge = FakeAndroidPrint();
        final notices = <String>[];
        final offers = AndroidPrintOffers(bridge: bridge, onNotice: notices.add)
          ..watch(session);
        addTearDown(offers.dispose);
        await transfer('retired', [...utf8.encode('%PDF-'), 1]);
        await _until(() => bridge.offered.isNotEmpty);
        offers.watch(null);
        bridge.answer.completeError(
          const AndroidPrintFailure('print_unavailable'),
        );
        expect(await relay.next((v) => v['type'] == 'print.result'), {
          'type': 'print.result',
          'jobId': 'retired',
          'result': 'failed',
        });
        expect(notices, isEmpty);
      },
    );

    test('malformed or foreign messages close the channel', () async {
      await open();
      relay.send({
        'type': 'print.job',
        'jobId': 'bad id',
        'format': 'pdf',
        'size': 10,
        'title': '',
      });
      await _until(() => session.closed);
      expect(session.error, 'print_invalid_message');
    });

    testWidgets('the receive notice shows bytes, percent and no guessed ETA', (
      tester,
    ) async {
      await tester.runAsync(() async {
        await open();
        relay.send({
          'type': 'print.job',
          'jobId': 'big',
          'format': 'pdf',
          'size': 512 * 1024,
          'title': '',
        });
        relay.send({
          'type': 'print.chunk',
          'jobId': 'big',
          'offset': 0,
          'data': base64.encode(_bytes(1024, 3)),
        });
        await _until(() => session.receivedBytes == 1024);
      });
      expect(session.receiving, isTrue);
      var now = Duration.zero;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: PrintReceiveNotice(printer: session, clock: () => now),
          ),
        ),
      );
      expect(find.text('인쇄 문서를 받는 중'), findsOneWidget);
      expect(find.text('1 KB / 512 KB · 0%'), findsOneWidget);
      expect(
        tester
            .widget<LinearProgressIndicator>(
              find.byKey(const Key('print-receive-bar')),
            )
            .value,
        closeTo(1024 / (512 * 1024), 1e-9),
      );
      // Too early for a trustworthy estimate.
      expect(find.byKey(const Key('print-receive-eta')), findsNothing);
      now = const Duration(seconds: 1);
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const Key('print-receive-eta')), findsNothing);
      await tester.pumpWidget(const SizedBox());
    });
  });

  group('dialog', () {
    Future<(Future<PrintOutcome>, FakeLocalPrint)> show(
      WidgetTester tester, {
      List<LocalPrinter> printers = const [
        LocalPrinter('Office'),
        LocalPrinter('Laser', isDefault: true),
      ],
      String? remembered,
      LocalPrintResult printResult = const LocalPrintResult(true, pages: 2),
      LocalPrintResult saveResult = const LocalPrintResult(true),
      Map<String, String> statuses = const {},
      int waiting = 0,
    }) async {
      final bridge = FakeLocalPrint(printers, printResult, saveResult)
        ..statuses.addAll(statuses);
      final memory = FakeMemory(remembered);
      late Future<PrintOutcome> outcome;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                onPressed: () => outcome = showPrintDialog(
                  context,
                  document: PrintDocument(
                    jobId: 'j',
                    title: '분기 보고서',
                    size: 123456,
                    file: File('C:/not-read/job.pdf'),
                  ),
                  machine: 'TESTER',
                  bridge: bridge,
                  memory: memory,
                  waiting: waiting,
                ),
                child: const Text('open'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      await tester.pumpAndSettle();
      return (outcome, bridge);
    }

    testWidgets(
      'an offline or paused printer is named, never blocks printing, and the warning follows the choice',
      (tester) async {
        final (outcome, bridge) = await show(
          tester,
          statuses: const {
            'Laser': 'print_printer_offline',
            'Office': 'print_printer_attention',
          },
        );
        expect(find.byKey(const Key('print-printer-status')), findsOneWidget);
        expect(
          find.text(printCodeMessage('print_printer_offline')),
          findsOneWidget,
        );
        expect(
          tester
              .widget<FilledButton>(find.byKey(const Key('print-confirm')))
              .onPressed,
          isNotNull,
          reason: 'the job waits in the printer queue; the person decides',
        );
        await tester.tap(find.byKey(const Key('print-printer')));
        await tester.pumpAndSettle();
        await tester.tap(find.text('Office').last);
        await tester.pumpAndSettle();
        expect(
          find.text(printCodeMessage('print_printer_offline')),
          findsNothing,
        );
        expect(
          find.text(printCodeMessage('print_printer_attention')),
          findsOneWidget,
        );
        await tester.tap(find.byKey(const Key('print-printer')));
        await tester.pumpAndSettle();
        await tester.tap(find.text('Laser (기본)').last);
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('print-confirm')));
        await tester.pumpAndSettle();
        expect(await outcome, PrintOutcome.printed);
        expect(bridge.printed.single.$1, 'Laser');
      },
    );

    testWidgets('a healthy printer shows no warning line', (tester) async {
      await show(tester);
      expect(find.byKey(const Key('print-printer-status')), findsNothing);
    });

    testWidgets(
      'closing the printer\'s own dialog (Print to PDF asks where to save) is a cancel, not an error',
      (tester) async {
        final (outcome, bridge) = await show(
          tester,
          printResult: const LocalPrintResult(false, code: 'print_cancelled'),
        );
        await tester.tap(find.byKey(const Key('print-confirm')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('print-dialog')), findsOneWidget);
        expect(find.byKey(const Key('print-error')), findsNothing);
        bridge.printResult = const LocalPrintResult(true, pages: 2);
        await tester.tap(find.byKey(const Key('print-confirm')));
        await tester.pumpAndSettle();
        expect(await outcome, PrintOutcome.printed);
      },
    );

    testWidgets(
      'documents waiting behind this one are counted; the defaults hint is shown',
      (tester) async {
        await show(tester, waiting: 2);
        expect(find.textContaining('대기 2개 더'), findsOneWidget);
        expect(find.textContaining('프린터의 기본 설정을 따라요'), findsOneWidget);
        await tester.tap(find.byKey(const Key('print-cancel')));
        await tester.pumpAndSettle();
      },
    );

    testWidgets('prints on the remembered printer with the chosen copies', (
      tester,
    ) async {
      final (outcome, bridge) = await show(tester, remembered: 'Office');
      expect(find.text('분기 보고서'), findsOneWidget);
      expect(find.textContaining('2페이지'), findsOneWidget);
      expect(
        find.text('Office'),
        findsOneWidget,
        reason: 'the remembered printer, not the default',
      );
      await tester.tap(find.byKey(const Key('print-copies-more')));
      await tester.tap(find.byKey(const Key('print-copies-more')));
      await tester.pump();
      expect(find.text('3'), findsOneWidget);
      await tester.tap(find.byKey(const Key('print-confirm')));
      await tester.pumpAndSettle();
      expect(await outcome, PrintOutcome.printed);
      expect(bridge.printed, [('Office', 3, '분기 보고서')]);
    });

    testWidgets(
      'defaults to the PC default printer; a failure keeps the dialog with its reason',
      (tester) async {
        final (_, bridge) = await show(
          tester,
          printResult: const LocalPrintResult(
            false,
            code: 'print_printer_unavailable',
          ),
        );
        expect(find.text('Laser (기본)'), findsOneWidget);
        await tester.tap(find.byKey(const Key('print-confirm')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('print-error')), findsOneWidget);
        expect(
          find.text(printCodeMessage('print_printer_unavailable')),
          findsOneWidget,
        );
        expect(bridge.printed.single.$1, 'Laser');
        await tester.tap(find.byKey(const Key('print-cancel')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('print-dialog')), findsNothing);
      },
    );

    testWidgets(
      'without printers: saving only; a closed save dialog keeps this one',
      (tester) async {
        final (outcome, bridge) = await show(
          tester,
          printers: const [],
          saveResult: const LocalPrintResult(false, code: 'print_cancelled'),
        );
        expect(find.byKey(const Key('print-no-printers')), findsOneWidget);
        expect(
          tester
              .widget<FilledButton>(find.byKey(const Key('print-confirm')))
              .onPressed,
          isNull,
        );
        await tester.tap(find.byKey(const Key('print-save')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('print-dialog')), findsOneWidget);
        expect(find.byKey(const Key('print-error')), findsNothing);
        bridge.saveResult = const LocalPrintResult(true);
        await tester.tap(find.byKey(const Key('print-save')));
        await tester.pumpAndSettle();
        expect(await outcome, PrintOutcome.saved);
        expect(bridge.saved, ['분기 보고서.pdf', '분기 보고서.pdf']);
      },
    );

    testWidgets('printing shows page progress only after 1.2 s, then done', (
      tester,
    ) async {
      final (outcome, bridge) = await show(tester);
      bridge.hold = Completer<void>();
      await tester.tap(find.byKey(const Key('print-confirm')));
      await tester.pump(const Duration(milliseconds: 300));
      // A short job never flashes a bar.
      expect(find.byKey(const Key('print-progress-bar')), findsNothing);
      expect(find.text('프린터로 보내는 중…'), findsOneWidget);
      bridge.shown = const LocalPrintProgress(3, 20);
      await tester.pump(const Duration(milliseconds: 1000));
      expect(find.text('인쇄 중 · 페이지 3/20'), findsOneWidget);
      final bar = tester.widget<LinearProgressIndicator>(
        find.byKey(const Key('print-progress-bar')),
      );
      expect(bar.value, closeTo(0.15, 1e-9));
      bridge.shown = const LocalPrintProgress(20, 20);
      await tester.pump(const Duration(milliseconds: 300));
      // Every page is with the print control: now the spooler takes over.
      expect(find.text('프린터로 보내는 중…'), findsOneWidget);
      expect(
        tester
            .widget<LinearProgressIndicator>(
              find.byKey(const Key('print-progress-bar')),
            )
            .value,
        isNull,
      );
      bridge.hold!.complete();
      await tester.pumpAndSettle();
      expect(await outcome, PrintOutcome.printed);
    });

    testWidgets('a short print finishes without ever showing a bar', (
      tester,
    ) async {
      final (outcome, bridge) = await show(tester);
      bridge.shown = const LocalPrintProgress(1, 2);
      await tester.tap(find.byKey(const Key('print-confirm')));
      await tester.pumpAndSettle();
      expect(await outcome, PrintOutcome.printed);
      expect(find.byKey(const Key('print-progress-bar')), findsNothing);
    });

    testWidgets('a failed print drops the bar and keeps the reason', (
      tester,
    ) async {
      final (_, bridge) = await show(
        tester,
        printResult: const LocalPrintResult(false, code: 'print_failed'),
      );
      bridge.hold = Completer<void>();
      bridge.shown = const LocalPrintProgress(2, 9);
      await tester.tap(find.byKey(const Key('print-confirm')));
      await tester.pump(const Duration(milliseconds: 1500));
      expect(find.byKey(const Key('print-progress-bar')), findsOneWidget);
      bridge.hold!.complete();
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('print-progress-bar')), findsNothing);
      expect(find.byKey(const Key('print-error')), findsOneWidget);
      expect(find.byKey(const Key('print-confirm')), findsOneWidget);
    });
  });
}

class FakeAndroidPrint implements AndroidPrintBridge {
  final offered = <String>[];
  var answer = Completer<bool>();
  @override
  Future<bool> print(PrintDocument document) async {
    expect(await document.file.exists(), isTrue);
    offered.add(document.jobId);
    final result = await answer.future;
    answer = Completer<bool>();
    return result;
  }
}

class FakeMemory extends PrintMemory {
  FakeMemory(this.printer) : super(File('unused-print-memory.json'));
  String? printer;
  @override
  Future<String?> load() async => printer;
  @override
  Future<void> save(String printer) async => this.printer = printer;
}

class FakeLocalPrint implements LocalPrintBridge {
  FakeLocalPrint(this.list, this.printResult, this.saveResult);
  final List<LocalPrinter> list;
  LocalPrintResult printResult, saveResult;
  final printed = <(String, int, String)>[];
  final saved = <String>[];

  /// What the spooler reports per printer name ('' = fine).
  final statuses = <String, String>{};
  @override
  Future<String> status(String printer) async => statuses[printer] ?? '';

  /// When set, print() waits for it; progress() answers [shown].
  Completer<void>? hold;
  LocalPrintProgress? shown;
  @override
  Future<LocalPrintProgress?> progress() async => shown;
  @override
  Future<List<LocalPrinter>> printers() async => list;
  @override
  Future<int?> pages(String path) async => 2;
  @override
  Future<LocalPrintResult> print(
    String path, {
    required String printer,
    required int copies,
    required String title,
  }) async {
    printed.add((printer, copies, title));
    await hold?.future;
    return printResult;
  }

  @override
  Future<LocalPrintResult> save(
    String path, {
    required String suggestedName,
  }) async {
    saved.add(suggestedName);
    return saveResult;
  }
}
