import 'dart:io';
import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_print.dart';
import 'package:multitap_client/data/print_session.dart';
import 'package:multitap_client/data/product_log.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/android/print');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final document = PrintDocument(
    jobId: 'fixture',
    title: 'fixture',
    size: 4,
    file: File('fixture-is-not-read-by-the-mocked-native-channel.pdf'),
  );
  const bridge = NativeAndroidPrint();
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test(
    'a native acceptance returns true and an explicit cancellation false',
    () async {
      messenger.setMockMethodCallHandler(channel, (call) async {
        expect(call.method, 'print');
        final arguments = call.arguments as Map;
        expect(arguments['path'], document.file.path);
        expect(arguments['title'], 'fixture');
        expect(arguments['requestId'], matches(RegExp(r'^[0-9a-f]{32}$')));
        return {'ok': true, 'code': ''};
      });
      expect(await bridge.print(document), isTrue);
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => {'ok': false, 'code': 'print_cancelled'},
      );
      expect(await bridge.print(document), isFalse);
    },
  );

  for (final code in [
    'print_document_invalid',
    'print_unavailable',
    'print_failed',
  ]) {
    test('$code remains a failure instead of a silent cancellation', () async {
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => {'ok': false, 'code': code},
      );
      await expectLater(
        bridge.print(document),
        throwsA(isA<AndroidPrintFailure>().having((e) => e.code, 'code', code)),
      );
    });
  }

  for (final result in [
    null,
    false,
    {'ok': false},
    {'ok': true, 'code': 'print_failed'},
    {'ok': false, 'code': 'untrusted exception text'},
  ]) {
    test(
      'an invalid native result reports only a fixed failure ($result)',
      () async {
        messenger.setMockMethodCallHandler(channel, (_) async => result);
        await expectLater(
          bridge.print(document),
          throwsA(
            isA<AndroidPrintFailure>().having(
              (e) => e.code,
              'code',
              'print_failed',
            ),
          ),
        );
      },
    );
  }

  test(
    'platform and missing-plugin errors are never treated as user cancel',
    () async {
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => throw PlatformException(code: 'arbitrary_native_error'),
      );
      await expectLater(
        bridge.print(document),
        throwsA(
          isA<AndroidPrintFailure>().having(
            (e) => e.code,
            'code',
            'print_failed',
          ),
        ),
      );
      messenger.setMockMethodCallHandler(channel, null);
      await expectLater(
        bridge.print(document),
        throwsA(
          isA<AndroidPrintFailure>().having(
            (e) => e.code,
            'code',
            'print_unavailable',
          ),
        ),
      );
    },
  );

  test(
    'only exact, increasing native stages for the live request are logged',
    () async {
      final answer = Completer<Object?>();
      String? id;
      messenger.setMockMethodCallHandler(channel, (call) {
        id = (call.arguments as Map)['requestId'] as String;
        return answer.future;
      });
      final pending = bridge.print(document);
      await Future<void>.delayed(Duration.zero);
      Future<void> event(Map<String, Object?> values) async {
        final reply = Completer<void>();
        await messenger.handlePlatformMessage(
          channel.name,
          const StandardMethodCodec().encodeMethodCall(
            MethodCall('print.event', values),
          ),
          (_) => reply.complete(),
        );
        await reply.future;
      }

      final before = ProductLog.instance.entries.length;
      await event({'requestId': id, 'sequence': 1, 'stage': 3});
      for (final invalid in [
        {'requestId': '0' * 32, 'sequence': 2, 'stage': 4},
        {'requestId': id, 'sequence': 1, 'stage': 4},
        {'requestId': id, 'sequence': 2, 'stage': 10},
        {'requestId': id, 'sequence': 65, 'stage': 4},
        {'requestId': id, 'sequence': 2, 'stage': 4, 'title': 'not accepted'},
      ]) {
        await event(invalid);
      }
      await event({'requestId': id, 'sequence': 2, 'stage': 4});
      expect(
        ProductLog.instance.entries.skip(before).map((e) => e.values['stage']),
        [3, 4],
      );
      answer.complete({'ok': true, 'code': ''});
      expect(await pending, isTrue);
      final after = ProductLog.instance.entries.length;
      await event({'requestId': id, 'sequence': 3, 'stage': 8});
      expect(ProductLog.instance.entries, hasLength(after));
    },
  );

  testWidgets('new native print requests wait for foreground; an already '
      'opened request survives pause and the next document waits', (
    tester,
  ) async {
    final binding = tester.binding;
    void background() {
      binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    }

    void foreground() {
      binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    }

    final printer = _ForegroundPrinter();
    final native = _ForegroundBridge();
    final offers = AndroidPrintOffers(bridge: native, foregroundOnly: true);
    addTearDown(() {
      offers.dispose();
      foreground();
    });
    printer.pending.addAll([document, document]);
    background();
    offers.watch(printer);
    await tester.pump();
    expect(native.calls, 0);
    foreground();
    await tester.pump();
    expect(native.calls, 1);
    background();
    await tester.pump();
    expect(printer.finished, isEmpty);
    native.answers.first.complete(true);
    await tester.pump();
    expect(printer.finished, [PrintOutcome.printed]);
    expect(native.calls, 1, reason: 'do not open the next job in background');
    foreground();
    await tester.pump();
    expect(native.calls, 2);
    native.answers.last.complete(false);
    await tester.pump();
    expect(printer.finished, [PrintOutcome.printed, PrintOutcome.cancelled]);
  });
}

class _ForegroundPrinter extends PrintSession {
  _ForegroundPrinter()
    : super(
        url: Uri.parse('wss://fixture.invalid/print'),
        sessionId: 'fixture',
        token: 'not-persisted',
      );
  final finished = <PrintOutcome>[];
  @override
  void hold(PrintDocument document) {}
  @override
  Future<void> finish(PrintDocument document, PrintOutcome outcome) async {
    pending.remove(document);
    finished.add(outcome);
  }
}

class _ForegroundBridge implements AndroidPrintBridge {
  int calls = 0;
  final answers = <Completer<bool>>[];
  @override
  Future<bool> print(PrintDocument document) {
    calls++;
    final answer = Completer<bool>();
    answers.add(answer);
    return answer.future;
  }
}
