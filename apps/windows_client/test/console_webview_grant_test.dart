import 'dart:async';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/console_browser.dart';

class WebViewBroker implements ConsoleWebViewBrokerApi {
  final calls = <String>[];
  final closedOwners = <String?>[];
  Completer<BrokerConsoleWebViewGrant>? pending;
  Completer<void>? closeGate;
  String code = 'c' * 43;
  @override
  Future<BrokerConsoleWebViewGrant> createConsoleWebView(
    String token,
    String desktopId,
  ) async {
    calls.add('create:$desktopId');
    return pending?.future ??
        BrokerConsoleWebViewGrant(
          path: '/console/$desktopId/attach',
          code: code,
          expiresAt: DateTime.now().add(const Duration(seconds: 30)),
        );
  }

  @override
  Future<void> closeConsoleWebViews(
    String token,
    String desktopId, {
    String? ownerId,
  }) async {
    calls.add('close:$desktopId');
    closedOwners.add(ownerId);
    await closeGate?.future;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final origin = Uri.parse('https://fixture.invalid:8443');
  final now = DateTime.utc(2026, 10, 31);
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const console = ConsoleBrowser.windows();
  String ownerId(String code) => sha256.convert(code.codeUnits).toString();
  Map<String, dynamic> grant() => {
    'protocol': 'console-webview-v1',
    'path': '/console/pve-149/attach',
    'code': 'c' * 43,
    'expiresAt': now.add(const Duration(seconds: 30)).toIso8601String(),
  };
  tearDown(() async {
    await console.closeAll();
    messenger.setMockMethodCallHandler(console.channel, null);
  });
  test(
    'grant is exact VM/origin-bound, one-use code only and bounded expiry',
    () {
      final value = BrokerConsoleWebViewGrant.fromJson(
        grant(),
        origin,
        'pve-149',
        now: now,
      );
      expect(value.validFor(origin, 'pve-149', now: now), isTrue);
      expect(value.validFor(origin, 'pve-150', now: now), isFalse);
      for (final changes in <Map<String, dynamic>>[
        {'protocol': 'console-browser-v1'},
        {'path': 'https://outside.invalid/console/pve-149/attach'},
        {'path': '/console/pve-150/attach'},
        {'path': '/console/pve-149/attach?code=secret'},
        {'path': '/console/pve-149/%61ttach'},
        {'code': 'c' * 42},
        {'code': '!' * 43},
        {'expiresAt': now.toIso8601String()},
        {'expiresAt': now.add(const Duration(minutes: 3)).toIso8601String()},
        {'expiresAt': 'invalid'},
        {'token': 'parent-secret'},
      ]) {
        expect(
          () => BrokerConsoleWebViewGrant.fromJson(
            {...grant(), ...changes},
            origin,
            'pve-149',
            now: now,
          ),
          throwsA(isA<BrokerException>()),
        );
      }
      for (final bad in [
        'http://fixture.invalid',
        'https://user:secret@fixture.invalid',
        'https://fixture.invalid/path',
        'https://fixture.invalid?secret',
      ]) {
        expect(
          () => BrokerConsoleWebViewGrant.fromJson(
            grant(),
            Uri.parse(bad),
            'pve-149',
            now: now,
          ),
          throwsA(isA<BrokerException>()),
        );
      }
    },
  );
  test(
    'native gets only single-use code; close revokes only the exact parent VM',
    () async {
      final broker = WebViewBroker();
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(console.channel, (call) async {
        calls.add(call);
        return true;
      });
      await console.open(
        origin,
        'pve-149',
        api: broker,
        token: 'parent-must-stay-in-Dart',
        current: () => true,
      );
      expect(calls.single.method, 'openConsoleWebView');
      expect(calls.single.arguments, {
        'origin': origin.origin,
        'desktopId': 'pve-149',
        'attachPath': '/console/pve-149/attach',
        'code': 'c' * 43,
      });
      await ConsoleBrowser.handleClosed({
        'origin': 'https://outside.invalid',
        'desktopId': 'pve-149',
        'ownerId': ownerId('c' * 43),
      });
      expect(broker.calls, ['create:pve-149']);
      await ConsoleBrowser.handleClosed({
        'origin': origin.origin,
        'desktopId': 'pve-149',
        'ownerId': ownerId('c' * 43),
      });
      expect(broker.calls, ['create:pve-149', 'close:pve-149']);
      await ConsoleBrowser.handleClosed({
        'origin': origin.origin,
        'desktopId': 'pve-149',
        'ownerId': ownerId('c' * 43),
      });
      expect(
        broker.calls.where((call) => call.startsWith('close:')),
        hasLength(1),
      );
    },
  );
  test('late attachment after logout cannot open a native console', () async {
    final broker = WebViewBroker()
      ..pending = Completer<BrokerConsoleWebViewGrant>();
    var current = true;
    var opens = 0;
    messenger.setMockMethodCallHandler(console.channel, (call) async {
      if (call.method == 'openConsoleWebView') opens++;
      return true;
    });
    final future = console.open(
      origin,
      'pve-149',
      api: broker,
      token: 'parent',
      current: () => current,
    );
    current = false;
    broker.pending!.complete(
      BrokerConsoleWebViewGrant(
        path: '/console/pve-149/attach',
        code: 'c' * 43,
        expiresAt: DateTime.now().add(const Duration(seconds: 30)),
      ),
    );
    await expectLater(
      future,
      throwsA(
        isA<BrokerException>().having((e) => e.code, 'code', 'session_expired'),
      ),
    );
    expect(opens, 0);
  });
  test(
    'runtime absence is actionable and never opens external browser/native RFB',
    () async {
      final broker = WebViewBroker();
      final methods = <String>[];
      messenger.setMockMethodCallHandler(console.channel, (call) async {
        methods.add(call.method);
        throw PlatformException(code: 'console_webview_runtime_required');
      });
      await expectLater(
        console.open(
          origin,
          'pve-149',
          api: broker,
          token: 'parent',
          current: () => true,
        ),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'console_webview_runtime_required',
          ),
        ),
      );
      expect(methods, ['openConsoleWebView']);
      expect(broker.calls, ['create:pve-149', 'close:pve-149']);
    },
  );
  test('late old native completion and old close event preserve the new account console', () async {
    final oldBroker = WebViewBroker();
    final newBroker = WebViewBroker()..code = 'd' * 43;
    final firstNative = Completer<bool>();
    final calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(console.channel, (call) async {
      calls.add(call);
      if (call.method == 'openConsoleWebView' && calls.length == 1) {
        return firstNative.future;
      }
      return true;
    });
    var oldCurrent = true;
    final old = console.open(
      origin,
      'pve-149',
      api: oldBroker,
      token: 'old-parent',
      current: () => oldCurrent,
    );
    final oldRejected = expectLater(
      old,
      throwsA(
        isA<BrokerException>().having((e) => e.code, 'code', 'session_expired'),
      ),
    );
    await Future<void>.delayed(Duration.zero);
    oldCurrent = false;
    final newer = console.open(
      origin,
      'pve-149',
      api: newBroker,
      token: 'new-parent',
      current: () => true,
    );
    expect(newBroker.calls, isEmpty);
    firstNative.complete(true);
    await oldRejected;
    await newer;
    expect(calls.map((call) => call.method), [
      'openConsoleWebView',
      'closeConsoleWebViews',
      'openConsoleWebView',
    ]);
    expect(calls[1].arguments, {
      'origin': origin.origin,
      'desktopId': 'pve-149',
    });
    expect(newBroker.calls, ['create:pve-149']);
    await ConsoleBrowser.handleClosed({
      'origin': origin.origin,
      'desktopId': 'pve-149',
      'ownerId': ownerId('c' * 43),
    });
    expect(newBroker.calls, ['create:pve-149']);
    await ConsoleBrowser.handleClosed({
      'origin': origin.origin,
      'desktopId': 'pve-149',
      'ownerId': ownerId('d' * 43),
    });
    expect(newBroker.calls, ['create:pve-149', 'close:pve-149']);
  });
  test('a pending scoped revoke finishes before a same-parent new attachment is issued', () async {
    final broker = WebViewBroker();
    messenger.setMockMethodCallHandler(console.channel, (_) async => true);
    await console.open(
      origin,
      'pve-149',
      api: broker,
      token: 'parent',
      current: () => true,
    );
    broker.closeGate = Completer<void>();
    final closing = ConsoleBrowser.handleClosed({
      'origin': origin.origin,
      'desktopId': 'pve-149',
      'ownerId': ownerId('c' * 43),
    });
    broker.code = 'd' * 43;
    final next = console.open(
      origin,
      'pve-149',
      api: broker,
      token: 'parent',
      current: () => true,
    );
    await Future<void>.delayed(Duration.zero);
    expect(broker.calls, ['create:pve-149', 'close:pve-149']);
    expect(broker.closedOwners, [ownerId('c' * 43)]);
    broker.closeGate!.complete();
    await closing;
    await next;
    expect(broker.calls, ['create:pve-149', 'close:pve-149', 'create:pve-149']);
    await ConsoleBrowser.handleClosed({
      'origin': origin.origin,
      'desktopId': 'pve-149',
      'ownerId': ownerId('c' * 43),
    });
    expect(broker.calls.where((call) => call == 'close:pve-149'), hasLength(1));
  });
  test('same-parent duplicate focuses existing native owner and account-scoped logout skips a different owner', () async {
    final broker = WebViewBroker();
    messenger.setMockMethodCallHandler(console.channel, (_) async => true);
    await console.open(
      origin,
      'pve-149',
      api: broker,
      token: 'parent',
      current: () => true,
    );
    broker.code = 'd' * 43;
    await console.open(
      origin,
      'pve-149',
      api: broker,
      token: 'parent',
      current: () => true,
    );
    await console.closeAll(api: WebViewBroker(), token: 'other-parent');
    expect(broker.calls, ['create:pve-149', 'create:pve-149']);
    await ConsoleBrowser.handleClosed({
      'origin': origin.origin,
      'desktopId': 'pve-149',
      'ownerId': ownerId('c' * 43),
    });
    expect(broker.calls, ['create:pve-149', 'create:pve-149', 'close:pve-149']);
  });
  test(
    'failed native close cannot reuse a revoked same-parent child on reopen',
    () async {
      final broker = WebViewBroker();
      final methods = <String>[];
      var failClose = true;
      messenger.setMockMethodCallHandler(console.channel, (call) async {
        methods.add(call.method);
        if (call.method == 'closeConsoleWebViews' && failClose) {
          throw PlatformException(code: 'close_failed');
        }
        return true;
      });
      await console.open(
        origin,
        'pve-149',
        api: broker,
        token: 'parent',
        current: () => true,
      );
      await console.closeAll(api: broker, token: 'parent');
      failClose = false;
      broker.code = 'd' * 43;
      await console.open(
        origin,
        'pve-149',
        api: broker,
        token: 'parent',
        current: () => true,
      );
      expect(methods, [
        'openConsoleWebView',
        'closeConsoleWebViews',
        'closeConsoleWebViews',
        'openConsoleWebView',
      ]);
      expect(broker.calls, [
        'create:pve-149',
        'close:pve-149',
        'create:pve-149',
      ]);
      await ConsoleBrowser.handleClosed({
        'origin': origin.origin,
        'desktopId': 'pve-149',
        'ownerId': ownerId('c' * 43),
      });
      expect(
        broker.calls.where((value) => value == 'close:pve-149'),
        hasLength(1),
      );
    },
  );
}
