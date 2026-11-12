// PRIVATE GREEN-only interaction controls. Public synthetic strings only.
// Compare the candidate widget with the actual pinned SDK SelectableText.
import 'dart:ui' show PointerDeviceKind;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/mfa_sensitive_text.dart';

const _leafKey = Key('mfa-interaction-leaf');
const _line = 'Alpha beta gamma delta.';
const _multiline = 'Alpha beta gamma delta.\n'
    '0123-4567-89AB-CDEF-0123-4567-89AB-CDEF\n'
    'Another selectable synthetic line for wrapping.';
const _style = TextStyle(fontSize: 16, height: 1.4);

class _PlatformFake {
  final writes = <String>[];
  int shareCalls = 0, processCalls = 0, processQueries = 0;
  Future<Object?> platform(MethodCall call) async {
    if (call.method == 'Clipboard.setData') {
      final args = call.arguments as Map;
      expectSync(args.keys, ['text']);
      writes.add(args['text'] as String);
    } else if (call.method == 'Clipboard.hasStrings') {
      return {'value': false};
    } else if (call.method == 'Share.invoke') {
      ++shareCalls;
    }
    return null;
  }
  Future<Object?> process(MethodCall call) async {
    if (call.method == 'ProcessText.queryTextActions') {
      ++processQueries;
      return {'synthetic-process': 'Synthetic process'};
    }
    if (call.method == 'ProcessText.processTextAction') { ++processCalls; }
    return null;
  }
}

Future<void> _mount(WidgetTester tester, {required bool guarded,
    required TargetPlatform platform, String data = _line, double width = 960,
    double scale = 1, ValueNotifier<int>? revision}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = Size(width, 1000);
  final binding = Object();
  Widget leaf() => guarded
      ? MfaSensitiveText(data, key: _leafKey, binding: binding,
          canCopy: () => true, style: _style)
      : SelectableText(data, key: _leafKey, style: _style);
  final content = revision == null ? leaf() : ValueListenableBuilder<int>(
    valueListenable: revision,
    builder: (_, value, _) => DecoratedBox(
      decoration: BoxDecoration(color: value.isEven ? Colors.transparent : Colors.amber.shade50),
      child: leaf()),
  );
  await tester.pumpWidget(MaterialApp(
    theme: buildAppTheme().copyWith(platform: platform),
    builder: (context, child) => MediaQuery(
      data: MediaQuery.of(context).copyWith(textScaler: TextScaler.linear(scale)),
      child: child!),
    home: Scaffold(body: Align(alignment: Alignment.topLeft,
      child: SizedBox(width: width, child: SingleChildScrollView(
        padding: const EdgeInsets.all(16), child: content)))),
  ));
  await tester.pumpAndSettle();
  expect(tester.takeException(), isNull);
}
EditableTextState _editable(WidgetTester tester) {
  final target = find.descendant(of: find.byKey(_leafKey),
    matching: find.byWidgetPredicate((widget) => widget is EditableText));
  expect(target, findsOneWidget);
  return tester.state<EditableTextState>(target);
}
String _selection(EditableTextState state) =>
    state.textEditingValue.selection.textInside(state.textEditingValue.text);
Offset _caret(EditableTextState state, int offset) {
  final render = state.renderEditable;
  final rect = render.getLocalRectForCaret(TextPosition(offset: offset));
  return render.localToGlobal(Offset(rect.left + .5, rect.center.dy));
}
Future<void> _mouseDrag(WidgetTester tester, EditableTextState state) async {
  final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
  final start = _caret(state, 0), end = _caret(state, 10);
  await mouse.addPointer(location: start);
  try {
    await mouse.down(start);
    await mouse.moveTo(end, timeStamp: const Duration(milliseconds: 100));
    await mouse.up();
    await tester.pumpAndSettle();
  } finally { await mouse.removePointer(); }
  expect(state.textEditingValue.selection.isCollapsed, isFalse);
  expect(_selection(state).length, greaterThanOrEqualTo(4));
}
Future<void> _mouseDoubleClick(WidgetTester tester, EditableTextState state) async {
  final point = _caret(state, 8);
  await tester.tapAt(point, kind: PointerDeviceKind.mouse);
  await tester.pump(const Duration(milliseconds: 50));
  await tester.tapAt(point, kind: PointerDeviceKind.mouse);
  await tester.pumpAndSettle();
  expect(_selection(state), 'beta');
}
Future<void> _longPress(WidgetTester tester, EditableTextState state) async {
  await tester.longPressAt(_caret(state, 8));
  await tester.pumpAndSettle();
  expect(state.textEditingValue.selection.isCollapsed, isFalse);
  expect(_selection(state), 'beta');
}
Future<void> _keyboardCopy(WidgetTester tester) async {
  try {
    await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
    await tester.sendKeyDownEvent(LogicalKeyboardKey.keyC);
  } finally {
    await tester.sendKeyUpEvent(LogicalKeyboardKey.keyC);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
  }
  await tester.idle();
}
List<ContextMenuButtonItem> _actualMenu(EditableTextState state) {
  final builder = state.widget.contextMenuBuilder;
  expect(builder, isNotNull);
  final toolbar = builder!(state.context, state);
  expect(toolbar, isA<AdaptiveTextSelectionToolbar>());
  return (toolbar as AdaptiveTextSelectionToolbar).buttonItems!;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  late _PlatformFake fake;
  setUp(() {
    fake = _PlatformFake();
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(SystemChannels.platform, fake.platform);
    messenger.setMockMethodCallHandler(SystemChannels.processText, fake.process);
  });
  tearDown(() {
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(SystemChannels.platform, null);
    messenger.setMockMethodCallHandler(SystemChannels.processText, null);
  });
  void interaction(String name, TargetPlatform platform,
      Future<void> Function(WidgetTester) body) {
    testWidgets(name, (tester) async {
      final previous = debugDefaultTargetPlatformOverride;
      debugDefaultTargetPlatformOverride = platform;
      try { await body(tester); }
      finally {
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
        tester.view.reset();
        debugDefaultTargetPlatformOverride = previous;
      }
    });
  }

  interaction('Windows mouse drag selects and keyboard-copies like SDK', TargetPlatform.windows, (tester) async {
    final selected = <String>[];
    for (final guarded in [false, true]) {
      fake.writes.clear();
      await _mount(tester, guarded: guarded, platform: TargetPlatform.windows);
      final state = _editable(tester);
      await _mouseDrag(tester, state);
      expect(state.widget.focusNode.hasFocus, isTrue);
      selected.add(_selection(state));
      await _keyboardCopy(tester);
      expect(fake.writes, [selected.last]);
      expect(tester.takeException(), isNull);
    }
    expect(selected[1], selected[0]);
    expect(fake.shareCalls, 0); expect(fake.processCalls, 0);
  });
  interaction('Windows mouse double-click selects the same whole word as SDK', TargetPlatform.windows, (tester) async {
    for (final guarded in [false, true]) {
      await _mount(tester, guarded: guarded, platform: TargetPlatform.windows);
      await _mouseDoubleClick(tester, _editable(tester));
      expect(tester.takeException(), isNull);
    }
    expect(fake.shareCalls, 0); expect(fake.processCalls, 0);
  });
  interaction('Android long-press reveals a hittable Copy menu and copies like SDK', TargetPlatform.android, (tester) async {
    for (final guarded in [false, true]) {
      fake.writes.clear();
      await _mount(tester, guarded: guarded, platform: TargetPlatform.android);
      final state = _editable(tester);
      await _longPress(tester, state);
      final text = _selection(state);
      final label = MaterialLocalizations.of(state.context).copyButtonLabel;
      final copy = find.text(label).hitTestable();
      expect(copy, findsOneWidget);
      await tester.tap(copy);
      await tester.idle(); await tester.pumpAndSettle();
      expect(fake.writes, [text]);
      expect(tester.takeException(), isNull);
    }
    expect(fake.shareCalls, 0); expect(fake.processCalls, 0);
  });

  for (final platform in [TargetPlatform.windows, TargetPlatform.android]) {
    final prefix = platform.name;
    interaction('$prefix same binding rebuild retains selection and focus like SDK', platform, (tester) async {
      for (final guarded in [false, true]) {
        final revision = ValueNotifier<int>(0);
        try {
          fake.writes.clear();
          await _mount(tester, guarded: guarded, platform: platform, revision: revision);
          final before = _editable(tester);
          if (platform == TargetPlatform.windows) { await _mouseDrag(tester, before); }
          else { await _longPress(tester, before); }
          final selection = before.textEditingValue.selection;
          expect(before.widget.focusNode.hasFocus, isTrue);
          revision.value++;
          await tester.pump();
          final after = _editable(tester);
          expect(identical(after, before), isTrue);
          expect(after.textEditingValue.selection, selection);
          expect(after.widget.focusNode.hasFocus, isTrue);
          _actualMenu(after).singleWhere((item) => item.type == ContextMenuButtonType.copy).onPressed!();
          await tester.idle();
          expect(fake.writes, [selection.textInside(_line)]);
          expect(tester.takeException(), isNull);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pump(); revision.dispose();
        }
      }
      expect(fake.shareCalls, 0); expect(fake.processCalls, 0);
    });
    interaction('$prefix multiline 320px and 200percent stays bounded like SDK', platform, (tester) async {
      final heights = <double>[];
      for (final guarded in [false, true]) {
        await _mount(tester, guarded: guarded, platform: platform,
          data: _multiline, width: 320, scale: 2);
        final state = _editable(tester), rect = tester.getRect(find.byKey(_leafKey));
        expect(state.renderEditable.textScaler.scale(16), 32);
        expect(rect.left, greaterThanOrEqualTo(0));
        expect(rect.right, lessThanOrEqualTo(320));
        expect(rect.width, lessThanOrEqualTo(288));
        expect(rect.height, greaterThan(64));
        heights.add(rect.height);
        if (platform == TargetPlatform.windows) { await _mouseDoubleClick(tester, state); }
        else { await _longPress(tester, state); }
        expect(tester.takeException(), isNull);
      }
      expect(heights[1], closeTo(heights[0], .01));
      expect(fake.shareCalls, 0); expect(fake.processCalls, 0);
    });
    interaction('$prefix sensitive menu permits only Copy and SelectAll with process action available', platform, (tester) async {
      await _mount(tester, guarded: false, platform: platform);
      var state = _editable(tester);
      if (platform == TargetPlatform.android) { await _longPress(tester, state); }
      else { await _mouseDoubleClick(tester, state); }
      final sdk = _actualMenu(state);
      expect(sdk.any((item) => item.type == ContextMenuButtonType.copy), isTrue);
      if (platform == TargetPlatform.android) {
        expect(fake.processQueries, greaterThan(0));
        expect(sdk.any((item) => item.type == ContextMenuButtonType.share || item.type == ContextMenuButtonType.custom), isTrue);
        expect(sdk.where((item) => item.type == ContextMenuButtonType.custom &&
            item.label == 'Synthetic process'), hasLength(1));
      }
      // Inspect baseline outbound actions, but NEVER invoke Share/ProcessText.
      await _mount(tester, guarded: true, platform: platform);
      state = _editable(tester);
      if (platform == TargetPlatform.android) { await _longPress(tester, state); }
      else { await _mouseDoubleClick(tester, state); }
      var items = _actualMenu(state);
      expect(items.every((item) => item.type == ContextMenuButtonType.copy || item.type == ContextMenuButtonType.selectAll), isTrue);
      expect(items.any((item) => item.type == ContextMenuButtonType.copy), isTrue);
      items.singleWhere((item) => item.type == ContextMenuButtonType.selectAll).onPressed!();
      await tester.pump();
      expect(_selection(state), _line);
      fake.writes.clear(); items = _actualMenu(state);
      items.singleWhere((item) => item.type == ContextMenuButtonType.copy).onPressed!();
      await tester.idle();
      expect(fake.writes, [_line]);
      expect(fake.shareCalls, 0); expect(fake.processCalls, 0);
      expect(tester.takeException(), isNull);
    });
  }
}
