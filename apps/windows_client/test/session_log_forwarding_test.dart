import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/main.dart' show SessionBootstrapFailure;

class _Guard implements WindowCloseGuard {
  Future<void> Function()? request;
  int closes = 0;
  @override
  void attach(Future<void> Function() value) => request = value;
  @override
  Future<bool> update({required bool busy, required bool dirty}) async => true;
  @override
  Future<bool> close() async {
    closes++;
    return true;
  }

  @override
  void dispose() {}
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const codec = StandardMethodCodec();
  Future<void> receive(Object? entry) async {
    final reply = Completer<void>();
    await messenger.handlePlatformMessage(
      SessionWindows.channel.name,
      codec.encodeMethodCall(MethodCall('log', entry)),
      (_) => reply.complete(),
    );
    await reply.future;
  }

  tearDown(() {
    messenger.setMockMethodCallHandler(SessionWindows.channel, null);
    SessionWindows.channel.setMethodCallHandler(null);
    SessionWindows.bindRetirement(null);
  });

  test(
    'Child forwards safe records once, parent stores once and never echoes',
    () async {
      var echoes = 0, sends = 0;
      final parent = ProductLog(
        recordSink: (_) {
          echoes++;
        },
      );
      final child = ProductLog(recordSink: SessionWindows.forwardLog);
      SessionWindows.listen(log: parent);
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        expect(call.method, 'log');
        sends++;
        expect(jsonEncode(call.arguments), isNot(contains('PRIVATE_SENTINEL')));
        await receive(call.arguments);
        return null;
      });
      child.record(LogEvent.streamEnd, {
        'ok': false,
        'error': 7,
        'password': 'PRIVATE_SENTINEL',
        'token': 'PRIVATE_SENTINEL',
        'clipboard': 'PRIVATE_SENTINEL',
        'width': double.infinity,
        'action': 'PRIVATE_SENTINEL',
      });
      await child.flush();
      await parent.flush();
      expect(sends, 1);
      expect(echoes, 0);
      expect(parent.entries, hasLength(1));
      expect(parent.entries.single.event, LogEvent.streamEnd);
      expect(parent.entries.single.values, {'ok': false, 'error': 7});
      expect(parent.entries.single.time, child.entries.single.time);
      child.dispose();
      parent.dispose();
    },
  );

  test('Forwarding child does not create or rotate log storage', () async {
    final root = await Directory.systemTemp.createTemp(
      'Multitap-log-forward-qa-',
    );
    final untouched = Directory('${root.path}${Platform.pathSeparator}child');
    final sent = <ProductLogEntry>[];
    final child = ProductLog()..forwardTo(sent.add);
    await child.initialize(untouched);
    child.record(LogEvent.streamReady, {'ok': true});
    await child.flush();
    expect(await untouched.exists(), isFalse);
    expect(await root.list().toList(), isEmpty);
    expect(sent, hasLength(1));
    child.dispose();
    await root.delete(); // Empty test-owned directory only; never recursive.
  });

  test(
    'Forwarding boundary strips injected fields and rejects malformed events',
    () async {
      final parent = ProductLog();
      SessionWindows.listen(log: parent);
      await receive({
        'time': '2026-10-24T01:00:00.000Z',
        'event': 'streamEnd',
        'token': 'PRIVATE_SENTINEL',
        'values': {
          'ok': false,
          'error': 'PRIVATE_SENTINEL',
          'height': 4294967296,
          'clipboard': 'PRIVATE_SENTINEL',
          7: 'PRIVATE_SENTINEL',
        },
      });
      await receive({'time': 'x' * 5000, 'event': 'streamEnd', 'values': {}});
      await receive({
        'time': '2026-10-24',
        'event': 'PRIVATE_SENTINEL',
        'values': {},
      });
      await receive({'time': '2026-10-24', 'event': 'streamEnd', 'values': []});
      expect(parent.entries, hasLength(1));
      expect(parent.entries.single.values, {'ok': false});
      expect(
        jsonEncode(parent.entries.single.toJson()),
        isNot(contains('PRIVATE_SENTINEL')),
      );
      parent.dispose();
    },
  );

  test(
    'Failed forwarding stays local and a later record can still be delivered',
    () async {
      var attempts = 0;
      final child = ProductLog(
        recordSink: (_) async {
          if (++attempts == 1) throw StateError('PRIVATE_SENTINEL');
        },
      );
      child.record(LogEvent.streamStart);
      child.record(LogEvent.streamEnd);
      await child.flush();
      expect(attempts, 2);
      expect(child.storageAvailable, isFalse);
      expect(child.entries, hasLength(2));
      expect(
        jsonEncode(child.entries.map((entry) => entry.toJson()).toList()),
        isNot(contains('PRIVATE_SENTINEL')),
      );
      child.dispose();
    },
  );

  testWidgets(
    'Malformed child bootstrap shows a closeable failure and acknowledges ready',
    (tester) async {
      final methods = <String>[];
      final guard = _Guard();
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        methods.add(call.method);
        return null;
      });
      await tester.pumpWidget(
        MaterialApp(
          home: SessionBootstrapFailure(sessionWindow: true, guard: guard),
        ),
      );
      await tester.pump();
      expect(find.text('원격 창을 준비하지 못했어요'), findsOneWidget);
      expect(methods, ['ready']);
      await tester.tap(find.text('닫기'));
      await tester.pump();
      expect(guard.closes, 1);
      await tester.pumpWidget(const SizedBox());
      expect(tester.takeException(), isNull);
    },
  );
}
