// PRIVATE, UNCOMPILED/UNEXECUTED regression proposal; synthetic values only.
// C1/C2 retain actual widget callbacks intentionally. This is an adversarial
// callback model, not evidence that a physical Flutter event retains them.
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

const _dialogKey = Key('different-inbox-dialog');
const _savedKey = Key('mfa-completed-saved');
const _doneKey = Key('mfa-completed-done');

// A read-only observer of the real notifier; no behavior is overridden.
class _ObservedInbox extends MfaCompletionInbox {
  bool get observedHasListeners => hasListeners;
}

class _Target {
  const _Target(this.inbox, this.finished);
  final MfaCompletionInbox inbox;
  final VoidCallback finished;
}

class _PlatformCalls {
  final writes = <String>[];
  final processed = <Object?>[];
  Future<Object?> platform(MethodCall call) async {
    if (call.method == 'Clipboard.setData') {
      final fields = call.arguments as Map;
      expect(fields.keys, ['text']);
      writes.add(fields['text'] as String);
    }
    if (call.method == 'Clipboard.hasStrings') { return {'value': false}; }
    return null; // Intercept every platform call; never forward to the OS.
  }
  Future<Object?> processText(MethodCall call) async {
    if (call.method == 'ProcessText.queryTextActions') {
      return {'different.inbox.synthetic': 'Synthetic text action'};
    }
    if (call.method == 'ProcessText.processTextAction') {
      processed.add(call.arguments);
      return null;
    }
    throw StateError('Unexpected mocked ProcessText method');
  }
}

Future<void> _mount(WidgetTester tester, ValueNotifier<_Target> target) async {
  await tester.pumpWidget(MaterialApp(theme: buildAppTheme(),
    home: Scaffold(body: ValueListenableBuilder<_Target>(
      valueListenable: target,
      builder: (context, value, child) => MfaCompletedRecoveryDialog(
        key: _dialogKey, inbox: value.inbox, onFinished: value.finished),
    ))));
  await tester.pumpAndSettle();
}

Future<void> _remove(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pump();
}

EditableTextState _editable(WidgetTester tester) {
  final editable = find.descendant(
    of: find.byKey(const Key('mfa-completed-codes')),
    matching: find.byWidgetPredicate((widget) => widget is EditableText));
  expect(editable, findsOneWidget);
  return tester.state<EditableTextState>(editable);
}

EditableTextState _select(WidgetTester tester) {
  final state = _editable(tester);
  state.widget.focusNode.requestFocus();
  state.selectAll(SelectionChangedCause.keyboard);
  expect(state.textEditingValue.selection.isCollapsed, isFalse);
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

void _semanticsCopy(WidgetTester tester, EditableTextState state) {
  final nodes = find.semantics.byValue(state.textEditingValue.text).evaluate().toList();
  expect(nodes, hasLength(1));
  expect(nodes.single.getSemanticsData().hasAction(SemanticsAction.copy), isTrue);
  state.renderEditable.owner!.semanticsOwner!
    .performAction(nodes.single.id, SemanticsAction.copy);
}

String _display() => mfaCodes.map((code) => [
  for (var index = 0; index < 32; index += 4) code.substring(index, index + 4),
].join('-')).join('\n');

Future<void> _saveCurrent(WidgetTester tester) async {
  expect(tester.widget<CheckboxListTile>(find.byKey(_savedKey)).value, isFalse);
  await tester.ensureVisible(find.byKey(_savedKey));
  await tester.tap(find.byKey(_savedKey));
  await tester.pump();
  expect(tester.widget<CheckboxListTile>(find.byKey(_savedKey)).value, isTrue);
  expect(tester.widget<FilledButton>(find.byKey(_doneKey)).onPressed, isNotNull);
}

Future<void> _finishCurrent(WidgetTester tester) async {
  await tester.ensureVisible(find.byKey(_doneKey));
  await tester.tap(find.byKey(_doneKey));
  await tester.pump();
}

void _intact(MfaTestOwner fixture, MfaCompletedOperation receipt) {
  expect(fixture.owner.current, isTrue);
  expect(identical(fixture.inbox.completed, receipt), isTrue);
  expect(receipt.codes, mfaCodes);
  expect(fixture.api.calls, isEmpty);
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final platform in [TargetPlatform.windows, TargetPlatform.android]) {
    group(platform.name, () {
      late _PlatformCalls calls;
      setUp(() {
        calls = _PlatformCalls();
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(SystemChannels.platform, calls.platform);
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(SystemChannels.processText, calls.processText);
      });
      tearDown(() {
        try { expect(calls.processed, isEmpty); }
        finally {
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(SystemChannels.platform, null);
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(SystemChannels.processText, null);
        }
      });
      void onPlatform(String name, Future<void> Function(WidgetTester) body) {
        testWidgets(name, (tester) async {
          final previous = debugDefaultTargetPlatformOverride;
          debugDefaultTargetPlatformOverride = platform;
          try { await body(tester); }
          finally { debugDefaultTargetPlatformOverride = previous; }
        });
      }

      onPlatform('G1 different inbox rebinds listener and copy with both owners live', (tester) async {
        final a = MfaTestOwner(), b = MfaTestOwner();
        final boxA = _ObservedInbox(), boxB = _ObservedInbox();
        final semantics = tester.ensureSemantics();
        var finishedA = 0, finishedB = 0;
        final target = ValueNotifier(_Target(boxA, () => finishedA++));
        try {
          // No controller or host subscribes to these two observed inboxes.
          expect(boxA.accept(boxA.reserve(a.owner), mfaChange()), isTrue);
          expect(boxB.accept(boxB.reserve(b.owner), mfaChange()), isTrue);
          final receiptA = boxA.completed!, receiptB = boxB.completed!;
          expect(identical(receiptA, receiptB), isFalse);
          expect(receiptA.codes, receiptB.codes); // Identity, not payload, changes.
          expect(boxA.observedHasListeners, isFalse);
          expect(boxB.observedHasListeners, isFalse);
          await _mount(tester, target);
          final dialogState = tester.state(find.byKey(_dialogKey));
          expect(boxA.observedHasListeners, isTrue);
          expect(boxB.observedHasListeners, isFalse);
          final old = _select(tester);
          await tester.pump();
          final oldValue = old.textEditingValue;
          // A genuine parent rebuild with the SAME inbox/receipt keeps selection.
          target.value = _Target(boxA, () => finishedA++);
          await tester.pump();
          expect(identical(tester.state(find.byKey(_dialogKey)), dialogState), isTrue);
          final same = _editable(tester); // Observe before any selection setter.
          expect(identical(same, old), isTrue);
          expect(same.textEditingValue, oldValue);
          expect(same.widget.focusNode.hasFocus, isTrue);
          final oldPublicCopy = old.copySelection;
          oldPublicCopy(SelectionChangedCause.keyboard);
          _menuCopy(old)();
          await tester.idle();
          expect(calls.writes, List.filled(2, _display()));
          calls.writes.clear();
          await _saveCurrent(tester);
          // Restore a real noncollapsed old selection after the consent tap.
          _select(tester);
          await tester.pump();
          final oldMenuCopy = _menuCopy(old);
          expect(old.mounted, isTrue);
          expect(old.textEditingValue.selection.isCollapsed, isFalse);
          target.value = _Target(boxB, () => finishedB++);
          await tester.pump();
          expect(identical(tester.state(find.byKey(_dialogKey)), dialogState), isTrue);
          expect(boxA.observedHasListeners, isFalse);
          expect(boxB.observedHasListeners, isTrue);
          expect(tester.widget<CheckboxListTile>(find.byKey(_savedKey)).value, isFalse);
          expect(tester.widget<FilledButton>(find.byKey(_doneKey)).onPressed, isNull);
          expect(a.owner.current && b.owner.current, isTrue);
          expect(identical(boxA.completed, receiptA), isTrue);
          expect(receiptA.codes, mfaCodes);
          final current = _select(tester);
          await tester.pump();
          expect(identical(current, old), isFalse);
          expect(current.textEditingValue.text, _display());
          oldPublicCopy(SelectionChangedCause.keyboard);
          oldMenuCopy();
          await tester.idle();
          expect(calls.writes, isEmpty);
          current.copySelection(SelectionChangedCause.keyboard);
          current.selectAll(SelectionChangedCause.keyboard);
          await tester.pump();
          _menuCopy(current)();
          current.selectAll(SelectionChangedCause.keyboard);
          await tester.pump();
          _semanticsCopy(tester, current);
          await tester.idle();
          expect(calls.writes, List.filled(3, _display()));
          await _saveCurrent(tester);
          await _finishCurrent(tester);
          expect(boxB.completed, isNull);
          expect(receiptB.codes, isEmpty);
          expect(identical(boxA.completed, receiptA), isTrue);
          expect(receiptA.codes, mfaCodes);
          expect([finishedA, finishedB], [0, 1]);
          expect(a.api.calls, isEmpty); expect(b.api.calls, isEmpty);
          await _remove(tester);
          expect(boxA.observedHasListeners, isFalse);
          expect(boxB.observedHasListeners, isFalse);
        } finally {
          await _remove(tester);
          target.dispose(); boxA.dispose(); boxB.dispose();
          a.dispose(); b.dispose(); semantics.dispose();
        }
      });

      onPlatform('C1 retained old checkbox and enabled Done cannot acknowledge replacement', (tester) async {
        final a = MfaTestOwner(), b = MfaTestOwner();
        var finishedA = 0, finishedB = 0;
        final target = ValueNotifier(_Target(a.inbox, () => finishedA++));
        try {
          expect(a.inbox.accept(a.inbox.reserve(a.owner), mfaChange()), isTrue);
          expect(b.inbox.accept(b.inbox.reserve(b.owner), mfaChange()), isTrue);
          final receiptA = a.inbox.completed!, receiptB = b.inbox.completed!;
          expect(identical(receiptA, receiptB), isFalse);
          expect(receiptA.codes, receiptB.codes);
          await _mount(tester, target);
          final state = tester.state(find.byKey(_dialogKey));
          await _saveCurrent(tester);
          final oldCheckbox = tester.widget<CheckboxListTile>(find.byKey(_savedKey)).onChanged!;
          final oldDone = tester.widget<FilledButton>(find.byKey(_doneKey)).onPressed!;
          target.value = _Target(b.inbox, () => finishedB++);
          await tester.pump();
          expect(identical(tester.state(find.byKey(_dialogKey)), state), isTrue);
          expect(tester.widget<CheckboxListTile>(find.byKey(_savedKey)).value, isFalse);
          expect(tester.widget<FilledButton>(find.byKey(_doneKey)).onPressed, isNull);
          _intact(a, receiptA); _intact(b, receiptB);
          oldDone(); // Reset alone is a useful control, but not the whole barrier.
          _intact(a, receiptA); _intact(b, receiptB);
          expect([finishedA, finishedB], [0, 0]);
          oldCheckbox(true);
          oldDone(); // Same turn: no new B consent and no intervening frame.
          _intact(a, receiptA); _intact(b, receiptB); // Planned RED on 097b.
          expect([finishedA, finishedB], [0, 0]);
          await tester.pump();
          expect(tester.widget<CheckboxListTile>(find.byKey(_savedKey)).value, isFalse);
          expect(tester.widget<FilledButton>(find.byKey(_doneKey)).onPressed, isNull);
          expect(calls.writes, isEmpty);
          await _saveCurrent(tester);
          await _finishCurrent(tester);
          expect(b.inbox.completed, isNull); expect(receiptB.codes, isEmpty);
          _intact(a, receiptA); expect([finishedA, finishedB], [0, 1]);
        } finally {
          await _remove(tester); target.dispose(); a.dispose(); b.dispose();
        }
      });

      onPlatform('C2 old enabled Done cannot consume even legitimately saved replacement', (tester) async {
        final a = MfaTestOwner(), b = MfaTestOwner();
        var finishedA = 0, finishedB = 0;
        final target = ValueNotifier(_Target(a.inbox, () => finishedA++));
        try {
          expect(a.inbox.accept(a.inbox.reserve(a.owner), mfaChange()), isTrue);
          expect(b.inbox.accept(b.inbox.reserve(b.owner), mfaChange()), isTrue);
          final receiptA = a.inbox.completed!, receiptB = b.inbox.completed!;
          expect(identical(receiptA, receiptB), isFalse);
          expect(receiptA.codes, receiptB.codes);
          await _mount(tester, target);
          final state = tester.state(find.byKey(_dialogKey));
          await _saveCurrent(tester);
          final oldDone = tester.widget<FilledButton>(find.byKey(_doneKey)).onPressed!;
          target.value = _Target(b.inbox, () => finishedB++);
          await tester.pump();
          expect(identical(tester.state(find.byKey(_dialogKey)), state), isTrue);
          _intact(a, receiptA); _intact(b, receiptB);
          await _saveCurrent(tester); // Consent B is real; old action A still is stale.
          oldDone();
          _intact(a, receiptA); _intact(b, receiptB); // Planned RED on 097b.
          expect([finishedA, finishedB], [0, 0]);
          expect(calls.writes, isEmpty);
          await _finishCurrent(tester);
          expect(b.inbox.completed, isNull); expect(receiptB.codes, isEmpty);
          _intact(a, receiptA); expect([finishedA, finishedB], [0, 1]);
        } finally {
          await _remove(tester); target.dispose(); a.dispose(); b.dispose();
        }
      });

      onPlatform('G2 own late ACK survives authorizing session retirement for copy and saved ACK', (tester) async {
        var authenticated = true, finished = 0;
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
          await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Scaffold(
            body: MfaCompletionOverlay(host: host, child: const SizedBox.expand()))));
          await tester.pumpAndSettle();
          final copy = _select(tester);
          copy.copySelection(SelectionChangedCause.keyboard);
          await tester.idle();
          expect(calls.writes, [_display()]);
          expect(authenticated, isFalse);
          expect(identical(host.inbox.completed, receipt), isTrue);
          await _saveCurrent(tester);
          // Use a local observer only; overlay keeps its real host callbacks.
          void changed() { if (host.inbox.completed == null) { finished++; } }
          host.inbox.addListener(changed);
          try { await _finishCurrent(tester); }
          finally { host.inbox.removeListener(changed); }
          expect(host.inbox.completed, isNull); expect(receipt.codes, isEmpty);
          expect(finished, 1); expect(authenticated, isFalse);
          expect(api.calls, ['status', 'setup', 'enable']);
          expect(calls.writes, [_display()]); // Saved ACK does not auto-copy.
        } finally {
          await _remove(tester); host.finished(controller); host.dispose();
        }
      });
    });
  }
}
