// PRIVATE SOURCE ONLY. Synthetic values; every platform call is intercepted.
// Exercises public dialog/EditableText/menu/semantics, no helper private state.
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/semantics.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/mfa_account.dart';
import 'package:multitap_client/data/mfa_account_controller.dart';
import 'package:multitap_client/data/mfa_account_host.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/mfa_account_dialog.dart';
import 'package:multitap_client/ui/mfa_completion_overlay.dart';

import 'helpers/mfa_account_fake.dart';

class _Clipboard {
  final writes = <String>[];
  Future<Object?> call(MethodCall call) async {
    if (call.method == 'Clipboard.setData') {
      final fields = call.arguments as Map;
      expect(fields.keys, ['text']);
      writes.add(fields['text'] as String);
      return null;
    }
    if (call.method == 'Clipboard.hasStrings') return {'value': false};
    // Also intercept unrelated platform methods; never forward to the OS.
    return null;
  }
}

class _ProcessText {
  final processed = <Object?>[];
  Future<Object?> call(MethodCall call) async {
    if (call.method == 'ProcessText.queryTextActions') {
      return {'addon.synthetic': 'Synthetic text action'};
    }
    if (call.method == 'ProcessText.processTextAction') {
      processed.add(call.arguments);
      return null;
    }
    throw StateError('Unexpected mocked ProcessText method');
  }
}

Future<void> _mount(WidgetTester tester, Widget child) async {
  await tester.pumpWidget(MaterialApp(theme: buildAppTheme(),
    home: Scaffold(body: child)));
  await tester.pumpAndSettle();
}

EditableTextState _select(WidgetTester tester, String key) {
  final leaf = find.descendant(of: find.byKey(Key(key)),
    matching: find.byWidgetPredicate((widget) => widget is EditableText));
  expect(leaf, findsOneWidget);
  final state = tester.state<EditableTextState>(leaf);
  state.selectAll(SelectionChangedCause.keyboard);
  return state;
}

VoidCallback _menuCopy(EditableTextState state) {
  final builder = state.widget.contextMenuBuilder;
  expect(builder, isNotNull);
  final toolbar = builder!(state.context, state);
  expect(toolbar, isA<AdaptiveTextSelectionToolbar>());
  return (toolbar as AdaptiveTextSelectionToolbar).buttonItems!
      .singleWhere((item) => item.type == ContextMenuButtonType.copy).onPressed!;
}

VoidCallback _semanticsCopy(WidgetTester tester, EditableTextState state) {
  final nodes = find.semantics.byValue(state.textEditingValue.text)
      .evaluate().toList();
  expect(nodes, hasLength(1));
  expect(nodes.single.getSemanticsData().hasAction(SemanticsAction.copy), isTrue);
  final semanticsOwner = state.renderEditable.owner!.semanticsOwner!;
  final nodeId = nodes.single.id;
  return () => semanticsOwner.performAction(nodeId, SemanticsAction.copy);
}

MfaAccountChange _change(List<String> codes) => MfaAccountChange.parse({
  'enabled': true, 'requiresLogin': true, 'recoveryCodes': codes,
}, MfaAccountAction.enable);

String _display(List<String> codes) => codes.map((code) =>
  [for (var index = 0; index < 32; index += 4)
    code.substring(index, index + 4)].join('-')).join('\n');

MfaCompletionOwner _successor(MfaTestOwner fixture) => MfaCompletionOwner(
  origin: fixture.api.origin,
  accountId: '11111111-1111-4111-8111-111111111111',
  username: 'synthetic', generation: 2, current: () => true,
);

MfaEnrollment _shortEnrollment() {
  const secret = 'JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP';
  final received = DateTime.now();
  return MfaEnrollment.parse({
    'protocol': 'mfa-v1', 'setupToken': 's' * 43,
    'expiresAt': received.add(const Duration(seconds: 3)).toUtc().toIso8601String(),
    'secret': secret,
    'uri': 'otpauth://totp/Multitap:synthetic?secret=$secret&issuer=Multitap&algorithm=SHA1&digits=6&period=30',
  }, receivedAt: received);
}

Future<EditableTextState> _revealShortSetup(
  WidgetTester tester, MfaTestOwner fixture,
) async {
  fixture.api.setup = () async => _shortEnrollment();
  await _mount(tester,
    MfaAccountDialog(controller: fixture.controller, onClose: () {}));
  fixture.controller.choose(MfaAccountAction.enable);
  await fixture.controller.submitProof('synthetic-password');
  await tester.pump();
  await tester.ensureVisible(find.byKey(const Key('mfa-account-reveal')));
  await tester.tap(find.byKey(const Key('mfa-account-reveal')));
  await tester.pump();
  return _select(tester, 'mfa-account-secret');
}

Future<void> _expireWallClockWithoutPumping(
  WidgetTester tester, MfaEnrollment seed,
) async {
  expect(seed.remaining, greaterThan(Duration.zero));
  // Real DTO remaining uses DateTime.now AND Stopwatch, not tester fake time.
  // Let only those clocks advance. Controller/UI fake-zone timers stay unpumped.
  await tester.runAsync(() async {
    final elapsed = Stopwatch()..start();
    while (seed.remaining > Duration.zero &&
        elapsed.elapsed < const Duration(seconds: 5)) {
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }
  });
  expect(seed.remaining, Duration.zero);
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final platform in [TargetPlatform.windows, TargetPlatform.android]) {
    group(platform.name, () {
      late _Clipboard clipboard;
      late _ProcessText processText;
      void testOwner(String name, Future<void> Function(WidgetTester) body) {
        testWidgets(name, (tester) async {
          final previous = debugDefaultTargetPlatformOverride;
          debugDefaultTargetPlatformOverride = platform;
          try { await body(tester); }
          finally { debugDefaultTargetPlatformOverride = previous; }
        });
      }
      setUp(() {
        clipboard = _Clipboard();
        processText = _ProcessText();
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(SystemChannels.processText, processText.call);
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(SystemChannels.platform, clipboard.call);
      });
      tearDown(() {
        try { expect(processText.processed, isEmpty); }
        finally {
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
              .setMockMethodCallHandler(SystemChannels.processText, null);
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
              .setMockMethodCallHandler(SystemChannels.platform, null);
        }
      });

      testOwner('old public state and menu cannot copy successor with reused payload', (tester) async {
        final f = MfaTestOwner();
        try {
          f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
          final oldReceipt = f.inbox.completed!;
          await _mount(tester,
            MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
          final oldState = _select(tester, 'mfa-completed-codes');
          await tester.pump();
          final oldPublicCopy = oldState.copySelection;
          final oldMenuCopy = _menuCopy(oldState);
          final oldText = oldState.textEditingValue.text;
          f.inbox.retire(f.owner);
          final successor = _successor(f);
          expect(f.inbox.accept(f.inbox.reserve(successor), mfaChange()), isTrue);
          await tester.pump();
          final current = _select(tester, 'mfa-completed-codes');
          expect(successor.current, isTrue);
          expect(identical(f.inbox.completed, oldReceipt), isFalse);
          expect(current.textEditingValue.text, oldText); // Same bytes, new owner.
          oldPublicCopy(SelectionChangedCause.keyboard);
          oldMenuCopy();
          await tester.idle();
          expect(clipboard.writes, isEmpty);
          current.selectAll(SelectionChangedCause.keyboard);
          current.copySelection(SelectionChangedCause.keyboard);
          await tester.idle();
          expect(clipboard.writes, [_display(mfaCodes)]);
          expect(f.api.calls, isEmpty);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose();
        }
      });

      testOwner('retained real menu cannot copy a different successor payload', (tester) async {
        final f = MfaTestOwner();
        try {
          f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
          await _mount(tester,
            MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
          final oldState = _select(tester, 'mfa-completed-codes');
          await tester.pump();
          final oldMenuCopy = _menuCopy(oldState);
          final oldText = oldState.textEditingValue.text;
          f.inbox.retire(f.owner);
          final successor = _successor(f);
          final nextCodes = List.generate(10,
            (index) => (index + 100).toRadixString(16).padLeft(32, '0'));
          expect(f.inbox.accept(f.inbox.reserve(successor), _change(nextCodes)), isTrue);
          await tester.pump();
          final current = _select(tester, 'mfa-completed-codes');
          expect(current.textEditingValue.text, _display(nextCodes));
          expect(current.textEditingValue.text, isNot(oldText));
          oldMenuCopy();
          await tester.idle();
          expect(clipboard.writes, isEmpty);
          current.selectAll(SelectionChangedCause.keyboard);
          await tester.pump();
          _menuCopy(current)();
          await tester.idle();
          expect(clipboard.writes, [_display(nextCodes)]);
          expect(f.api.calls, isEmpty);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose();
        }
      });

      testOwner('setup public copy checks clock expiry before controller timer cleanup', (tester) async {
        final f = MfaTestOwner();
        try {
          final state = await _revealShortSetup(tester, f);
          final seed = f.controller.enrollment!;
          state.copySelection(SelectionChangedCause.keyboard);
          await tester.idle();
          expect(clipboard.writes, [seed.secret]);
          clipboard.writes.clear();
          state.selectAll(SelectionChangedCause.keyboard);
          await _expireWallClockWithoutPumping(tester, seed);
          // No expiry timer or UI frame has cleared the still-mounted seed.
          expect(identical(f.controller.enrollment, seed), isTrue);
          expect(f.controller.phase, MfaAccountPhase.enrollment);
          expect(seed.secret, isNotEmpty);
          expect(state.mounted, isTrue);
          state.copySelection(SelectionChangedCause.keyboard);
          await tester.idle();
          expect(clipboard.writes, isEmpty);
          expect(f.api.calls, ['status', 'setup']);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose();
        }
      });

      testOwner('setup native semantics Copy checks expiry before any timer frame', (tester) async {
        final semantics = tester.ensureSemantics(), f = MfaTestOwner();
        try {
          final state = await _revealShortSetup(tester, f);
          final seed = f.controller.enrollment!;
          state.widget.focusNode.requestFocus();
          await tester.pump();
          final copy = _semanticsCopy(tester, state);
          copy();
          await tester.idle();
          expect(clipboard.writes, [seed.secret]);
          clipboard.writes.clear();
          state.selectAll(SelectionChangedCause.keyboard);
          await tester.pump();
          final retainedCopy = _semanticsCopy(tester, state);
          await _expireWallClockWithoutPumping(tester, seed);
          expect(identical(f.controller.enrollment, seed), isTrue);
          expect(f.controller.phase, MfaAccountPhase.enrollment);
          expect(seed.secret, isNotEmpty);
          expect(state.mounted, isTrue);
          retainedCopy(); // Real SemanticsOwner action, not an Actions intent.
          await tester.idle();
          expect(clipboard.writes, isEmpty);
          expect(f.api.calls, ['status', 'setup']);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose(); semantics.dispose();
        }
      });

      testOwner('own ACK after authorizing401 permits state menu and semantics copy', (tester) async {
        final semantics = tester.ensureSemantics();
        var authenticated = true;
        final host = MfaAccountHost(clock: () => DateTime.utc(2026, 11, 7));
        final api = MfaTestApi();
        final controller = host.begin(api: api, token: 'synthetic-only',
          accountId: '11111111-1111-4111-8111-111111111111', username: 'synthetic',
          actor: 'a' * 32, currentPresentation: () => true,
          currentAuthentication: () => authenticated, onCommitted: (_) {},
          onAuthenticationRejected: (_) {})!;
        try {
          await controller.refresh(); controller.choose(MfaAccountAction.enable);
          await controller.submitProof('synthetic-password');
          final reply = Completer<MfaAccountChange>();
          api.change = (_) => reply.future;
          final work = controller.enable('123456');
          authenticated = false;
          host.sessionRetired('synthetic-only', 'session_expired');
          expect(host.inbox.pendingFor(controller.owner), isTrue);
          reply.complete(mfaChange()); await work;
          final receipt = host.inbox.completed!;
          expect(receipt.owner.current, isTrue);
          await _mount(tester,
            MfaCompletionOverlay(host: host, child: const SizedBox.expand()));
          final state = _select(tester, 'mfa-completed-codes');
          state.widget.focusNode.requestFocus(); await tester.pump();
          state.copySelection(SelectionChangedCause.keyboard);
          state.selectAll(SelectionChangedCause.keyboard); await tester.pump();
          _menuCopy(state)();
          state.selectAll(SelectionChangedCause.keyboard); await tester.pump();
          _semanticsCopy(tester, state)();
          await tester.idle();
          expect(authenticated, isFalse);
          expect(identical(host.inbox.completed, receipt), isTrue);
          expect(clipboard.writes, List.filled(3, _display(mfaCodes)));
          expect(api.calls, ['status', 'setup', 'enable']);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          host.finished(controller); host.dispose(); semantics.dispose();
        }
      });

      testOwner('completed semantics copy checks privacy clock before timer frame', (tester) async {
        final semantics = tester.ensureSemantics();
        var now = DateTime.utc(2026, 11, 7);
        final host = MfaAccountHost(clock: () => now), api = MfaTestApi();
        final controller = host.begin(api: api, token: 'synthetic-only',
          accountId: '11111111-1111-4111-8111-111111111111', username: 'synthetic',
          actor: 'a' * 32, currentPresentation: () => true,
          currentAuthentication: () => true, onCommitted: (_) {},
          onAuthenticationRejected: (_) {})!;
        try {
          await controller.refresh(); controller.choose(MfaAccountAction.enable);
          await controller.submitProof('synthetic-password');
          await controller.enable('123456');
          await _mount(tester,
            MfaCompletionOverlay(host: host, child: const SizedBox.expand()));
          final receipt = host.inbox.completed!;
          final state = _select(tester, 'mfa-completed-codes');
          state.widget.focusNode.requestFocus(); await tester.pump();
          _semanticsCopy(tester, state)();
          await tester.idle();
          expect(clipboard.writes, [_display(mfaCodes)]);
          clipboard.writes.clear();
          state.selectAll(SelectionChangedCause.keyboard); await tester.pump();
          final retainedCopy = _semanticsCopy(tester, state);
          expect(host.checkPrivacyIdle(), isTrue);
          now = now.add(const Duration(minutes: 30));
          // No pump, human event or timer callback before actual semantics Copy.
          expect(identical(host.inbox.completed, receipt), isTrue);
          expect(receipt.owner.current, isTrue);
          retainedCopy();
          await tester.idle();
          expect(clipboard.writes, isEmpty);
          expect(host.inbox.completed, isNull);
          expect(receipt.owner.current, isFalse);
          expect(api.calls, ['status', 'setup', 'enable']);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          host.finished(controller); host.dispose(); semantics.dispose();
        }
      });
    });
  }
}
