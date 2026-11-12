// Private reproduction against immutable Client24. Synthetic values only.
// SystemChannels.platform is intercepted; this file never touches OS clipboard.
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter/semantics.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/mfa_account.dart';
import 'package:multitap_client/data/mfa_account_host.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/mfa_account_dialog.dart';
import 'package:multitap_client/ui/mfa_completion_overlay.dart';

import 'helpers/mfa_account_fake.dart';

class _Clipboard {
  final writes = <String>[];
  Completer<void>? held;
  Future<Object?> call(MethodCall call) async {
    if (call.method == 'Clipboard.setData') {
      final args = call.arguments as Map;
      expect(args.keys, ['text']);
      writes.add(args['text'] as String);
      await held?.future;
      return null;
    }
    if (call.method == 'Clipboard.hasStrings') return {'value': false};
    if (call.method == 'Clipboard.getData') return null;
    return null;
  }
}

Future<void> _mount(WidgetTester tester, Widget child) async {
  await tester.pumpWidget(MaterialApp(theme: buildAppTheme(),
    home: Scaffold(body: child)));
  await tester.pumpAndSettle();
}

EditableTextState _selected(WidgetTester tester, String key) {
  final field = find.descendant(of: find.byKey(Key(key)),
    matching: find.byWidgetPredicate((widget) => widget is EditableText));
  expect(field, findsOneWidget);
  final state = tester.state<EditableTextState>(field);
  state.selectAll(SelectionChangedCause.keyboard);
  return state;
}

BuildContext _copyActionContext(WidgetTester tester, EditableTextState state) {
  final ownEditable = find.byElementPredicate((element) =>
      element is RenderObjectElement && identical(element.renderObject, state.renderEditable));
  expect(ownEditable, findsOneWidget);
  final context = tester.element(ownEditable);
  expect(Actions.maybeFind<CopySelectionTextIntent>(context), isNotNull);
  return context;
}

VoidCallback _copyMenu(EditableTextState state) {
  // Use the ACTUAL widget-supplied builder, including any app-owned wrappers.
  // Reading state.contextMenuButtonItems directly would bypass such wrappers.
  final builder = state.widget.contextMenuBuilder;
  expect(builder, isNotNull);
  final toolbar = builder!(state.context, state);
  expect(toolbar, isA<AdaptiveTextSelectionToolbar>());
  return (toolbar as AdaptiveTextSelectionToolbar).buttonItems!
      .singleWhere((item) => item.type == ContextMenuButtonType.copy).onPressed!;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final platform in [TargetPlatform.windows, TargetPlatform.android]) {
    group(platform.name, () {
      late _Clipboard clipboard;
      void testOwner(String name, Future<void> Function(WidgetTester) body) {
        testWidgets(name, (tester) async {
          final before = debugDefaultTargetPlatformOverride;
          debugDefaultTargetPlatformOverride = platform;
          try { await body(tester); }
          finally { debugDefaultTargetPlatformOverride = before; }
        });
      }
      setUp(() {
        clipboard = _Clipboard();
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(SystemChannels.platform, clipboard.call);
      });
      tearDown(() {
        if (clipboard.held?.isCompleted == false) clipboard.held!.complete();
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(SystemChannels.platform, null);
      });

      testOwner('current recovery owner permits keyboard and menu copying', (tester) async {
        final f = MfaTestOwner();
        try {
          f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
          await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
          final selected = _selected(tester, 'mfa-completed-codes');
          Actions.invoke(_copyActionContext(tester, selected), CopySelectionTextIntent.copy);
          selected.selectAll(SelectionChangedCause.keyboard);
          await tester.pump();
          _copyMenu(selected)();
          await tester.idle();
          expect(clipboard.writes.length, 2);
          expect(clipboard.writes.every((text) => text.contains('-')), isTrue);
          expect(f.api.calls, isEmpty);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose();
        }
      });

      testOwner('retired recovery owner blocks current-frame keyboard admission', (tester) async {
        final f = MfaTestOwner();
        try {
          f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
          await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
          final selected = _selected(tester, 'mfa-completed-codes');
          await tester.pump();
          final copyContext = _copyActionContext(tester, selected);
          f.presentation = false;
          f.inbox.retire(f.owner);
          expect(f.inbox.completed, isNull);
          // Deliberately no frame: the real action still has a mounted selection.
          Actions.invoke(copyContext, CopySelectionTextIntent.copy);
          await tester.idle();
          expect(clipboard.writes, isEmpty);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose();
        }
      });

      testOwner('retired recovery owner blocks an already-created menu callback', (tester) async {
        final f = MfaTestOwner();
        try {
          f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
          await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
          final selected = _selected(tester, 'mfa-completed-codes');
          await tester.pump();
          final copy = _copyMenu(selected);
          f.inbox.retire(f.owner);
          expect(f.owner.current, isFalse);
          copy(); // Actual default menu callback, before the retirement frame.
          await tester.idle();
          expect(clipboard.writes, isEmpty);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose();
        }
      });

      testOwner('cleared setup seed blocks a retained copy callback before next frame', (tester) async {
        final f = MfaTestOwner();
        try {
          await _mount(tester, MfaAccountDialog(controller: f.controller, onClose: () {}));
          f.controller.choose(MfaAccountAction.enable);
          await f.controller.submitProof('synthetic-password');
          await tester.pump();
          await tester.ensureVisible(find.byKey(const Key('mfa-account-reveal')));
          await tester.tap(find.byKey(const Key('mfa-account-reveal')));
          await tester.pump();
          final seed = f.controller.enrollment!;
          final selected = _selected(tester, 'mfa-account-secret');
          await tester.pump();
          _copyMenu(selected)(); // Current owner control keeps copying supported.
          await tester.idle();
          expect(clipboard.writes.length, 1);
          clipboard.writes.clear();
          selected.selectAll(SelectionChangedCause.keyboard);
          final staleCopy = _copyMenu(selected);
          f.presentation = false;
          f.inbox.retire(f.owner);
          f.controller.authenticationRetired();
          expect(seed.secret, isEmpty);
          staleCopy();
          await tester.idle();
          expect(clipboard.writes, isEmpty);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose();
        }
      });

      testOwner('expired30min human callback cannot propagate a stale keyboard copy', (tester) async {
        var now = DateTime.utc(2026, 11, 7);
        final host = MfaAccountHost(clock: () => now), api = MfaTestApi();
        final controller = host.begin(api: api, token: 'synthetic-only',
          accountId: '11111111-1111-4111-8111-111111111111', username: 'synthetic',
          actor: 'a' * 32, currentPresentation: () => true,
          currentAuthentication: () => true, onCommitted: (_) {},
          onAuthenticationRejected: (_) {})!;
        try {
          await controller.refresh(); controller.choose(MfaAccountAction.enable);
          await controller.submitProof('synthetic-password'); await controller.enable('123456');
          await _mount(tester, MfaCompletionOverlay(host: host, child: const SizedBox.expand()));
          final selected = _selected(tester, 'mfa-completed-codes');
          selected.widget.focusNode.requestFocus();
          await tester.pump();
          expect(selected.widget.focusNode.hasFocus, isTrue);
          now = now.add(const Duration(minutes: 30));
          // No clock/timer pump: model expiry is checked by the ACTUAL local key
          // observer, while the selectable widget still belongs to this frame.
          try {
            await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
            await tester.sendKeyDownEvent(LogicalKeyboardKey.keyC);
          } finally {
            await tester.sendKeyUpEvent(LogicalKeyboardKey.keyC);
            await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
          }
          await tester.idle();
          expect(host.inbox.completed, isNull);
          expect(clipboard.writes, isEmpty);
          expect(api.calls, ['status', 'setup', 'enable']);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          host.finished(controller); host.dispose();
        }
      });

      testOwner('write admitted while current may finish after retirement without replay', (tester) async {
        final f = MfaTestOwner();
        try {
          f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
          await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
          final selected = _selected(tester, 'mfa-completed-codes');
          clipboard.held = Completer<void>();
          Actions.invoke(_copyActionContext(tester, selected), CopySelectionTextIntent.copy);
          await tester.idle();
          expect(clipboard.writes.length, 1);
          f.inbox.retire(f.owner);
          clipboard.held!.complete(); clipboard.held = null;
          await tester.idle(); await tester.pump();
          expect(clipboard.writes.length, 1); // No wipe, replay or successor write.
          expect(f.inbox.completed, isNull);
          expect(find.byKey(const Key('mfa-completed-codes')), findsNothing);
          expect(f.api.calls, isEmpty);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose();
        }
      });

      testOwner('current completed overlay permits actual focused keyboard copy', (tester) async {
        final now = DateTime.utc(2026, 11, 7);
        final host = MfaAccountHost(clock: () => now), api = MfaTestApi();
        final controller = host.begin(api: api, token: 'synthetic-only',
          accountId: '11111111-1111-4111-8111-111111111111', username: 'synthetic',
          actor: 'a' * 32, currentPresentation: () => true,
          currentAuthentication: () => true, onCommitted: (_) {},
          onAuthenticationRejected: (_) {})!;
        try {
          await controller.refresh(); controller.choose(MfaAccountAction.enable);
          await controller.submitProof('synthetic-password'); await controller.enable('123456');
          await _mount(tester, MfaCompletionOverlay(host: host, child: const SizedBox.expand()));
          final selected = _selected(tester, 'mfa-completed-codes');
          selected.widget.focusNode.requestFocus(); await tester.pump();
          expect(selected.widget.focusNode.hasFocus, isTrue);
          expect(host.checkPrivacyIdle(), isTrue);
          try {
            await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
            await tester.sendKeyDownEvent(LogicalKeyboardKey.keyC);
          } finally {
            await tester.sendKeyUpEvent(LogicalKeyboardKey.keyC);
            await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
          }
          await tester.idle();
          expect(clipboard.writes.length, 1);
          expect(host.inbox.completed!.codes, mfaCodes);
          expect(api.calls, ['status', 'setup', 'enable']);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          host.finished(controller); host.dispose();
        }
      });

      testOwner('own acknowledged completion remains copyable after authorizing auth retires', (tester) async {
        final now = DateTime.utc(2026, 11, 7);
        final host = MfaAccountHost(clock: () => now), api = MfaTestApi();
        var authenticated = true;
        final controller = host.begin(api: api, token: 'synthetic-only',
          accountId: '11111111-1111-4111-8111-111111111111', username: 'synthetic',
          actor: 'a' * 32, currentPresentation: () => true,
          currentAuthentication: () => authenticated, onCommitted: (_) {},
          onAuthenticationRejected: (_) {})!;
        try {
          await controller.refresh(); controller.choose(MfaAccountAction.enable);
          await controller.submitProof('synthetic-password');
          final reply = Completer<MfaAccountChange>(); api.change = (_) => reply.future;
          final work = controller.enable('123456');
          authenticated = false; host.sessionRetired('synthetic-only', 'session_expired');
          expect(host.inbox.pendingFor(controller.owner), isTrue);
          reply.complete(mfaChange()); await work;
          expect(authenticated, isFalse);
          expect(controller.owner.current, isTrue);
          expect(host.inbox.completed!.codes, mfaCodes);
          await _mount(tester, MfaCompletionOverlay(host: host, child: const SizedBox.expand()));
          final selected = _selected(tester, 'mfa-completed-codes');
          selected.widget.focusNode.requestFocus(); await tester.pump();
          expect(selected.widget.focusNode.hasFocus, isTrue);
          try {
            await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
            await tester.sendKeyDownEvent(LogicalKeyboardKey.keyC);
          } finally {
            await tester.sendKeyUpEvent(LogicalKeyboardKey.keyC);
            await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
          }
          await tester.idle();
          expect(clipboard.writes.length, 1);
          expect(authenticated, isFalse);
          expect(host.inbox.completed!.codes, mfaCodes);
          expect(api.calls, ['status', 'setup', 'enable']);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          host.finished(controller); host.dispose();
        }
      });

      testOwner('current recovery semantics Copy remains accessible and admitted', (tester) async {
        final semantics = tester.ensureSemantics(), f = MfaTestOwner();
        try {
          f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
          await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
          final selected = _selected(tester, 'mfa-completed-codes');
          selected.widget.focusNode.requestFocus(); await tester.pump();
          final nodes = find.semantics.byValue(selected.textEditingValue.text).evaluate().toList();
          expect(nodes, hasLength(1));
          expect(nodes.single.getSemanticsData().hasAction(SemanticsAction.copy), isTrue);
          final owner = selected.renderEditable.owner!.semanticsOwner!;
          owner.performAction(nodes.single.id, SemanticsAction.copy);
          await tester.idle();
          expect(clipboard.writes.length, 1);
          expect(f.inbox.completed!.codes, mfaCodes);
          expect(f.api.calls, isEmpty);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose(); semantics.dispose();
        }
      });

      testOwner('retired recovery owner blocks retained semantics Copy before frame', (tester) async {
        final semantics = tester.ensureSemantics(), f = MfaTestOwner();
        try {
          f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
          await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
          final selected = _selected(tester, 'mfa-completed-codes');
          selected.widget.focusNode.requestFocus(); await tester.pump();
          final nodes = find.semantics.byValue(selected.textEditingValue.text).evaluate().toList();
          expect(nodes, hasLength(1));
          expect(nodes.single.getSemanticsData().hasAction(SemanticsAction.copy), isTrue);
          final owner = selected.renderEditable.owner!.semanticsOwner!;
          final nodeId = nodes.single.id;
          f.inbox.retire(f.owner);
          expect(f.inbox.completed, isNull);
          // Actual native-semantics admission, with the old node still mounted.
          owner.performAction(nodeId, SemanticsAction.copy);
          await tester.idle();
          expect(clipboard.writes, isEmpty);
          expect(f.api.calls, isEmpty);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          f.dispose(); semantics.dispose();
        }
      });
    });
  }
}
