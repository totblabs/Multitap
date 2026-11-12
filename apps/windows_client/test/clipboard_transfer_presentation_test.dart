import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/clipboard_transfer_card.dart';

class _Clipboard extends ClipboardSession {
  _Clipboard() : super(url: Uri.parse('wss://fixture.invalid/clipboard'),
      token: 'fixture-only', sessionId: 'fixture');
  bool running = false;
  ClipboardProgress? current;
  int cancels = 0;
  @override
  bool get transferring => running;
  @override
  bool get userWaiting => running;
  @override
  ClipboardProgress? get progress => current;
  @override
  Future<void> cancelTransfer() async { cancels++; }
  void begin({required bool known}) {
    current = known ? const ClipboardProgress(sending: false, format: 'files',
        fileCount: 1, fileIndex: 1, transferred: 1048576, total: 4194304,
        fileName: 'synthetic.bin') : null;
    running = true;
    notifyListeners();
  }
}

// Fixed local native-protocol seam. Legacy payloads deliberately reproduce
// published102's singleton last-writer behavior. Negotiated payloads enforce
// the agreed production lease protocol; no UI/progress algorithm is replaced.
class _NativeSink {
  _NativeSink(this.channel, this.messenger);
  final MethodChannel channel;
  final TestDefaultBinaryMessenger messenger;
  Map<String, Object?>? payload, identity;
  bool visible = false, holdGeneric = false;
  int acquisitions = 0, revision = 0;
  Completer<void>? _held;
  bool get hasHeldGeneric => _held != null;
  int? get percent => payload?['percent'] as int?;

  Map<String, Object?> _result(Map<String, Object?> requested, bool accepted) => {
    'protocol': 'presentation-lease-v1', 'accepted': accepted,
    'owner': requested['owner'], 'generation': requested['generation'],
    'lease': requested['lease'], 'revision': revision,
    'visible': accepted && visible,
  };
  bool _matches(Map<String, Object?> value) => identity != null &&
      value['owner'] == identity!['owner'] &&
      value['generation'] == identity!['generation'] &&
      value['lease'] == identity!['lease'];

  Future<Object?> call(MethodCall call) async {
    final value = Map<String, Object?>.from(call.arguments as Map? ?? {});
    if (call.method == 'acquire') {
      acquisitions++;
      revision = 1;
      identity = {'owner': value['owner'], 'generation': value['generation'],
        'lease': acquisitions.toRadixString(16).padLeft(32, '0')};
      visible = false;
      return _result(identity!, true);
    }
    if (call.method == 'update') {
      if (holdGeneric && value['title'] == '옮기는 중') {
        _held = Completer<void>();
        await _held!.future;
      }
      if (value['owner'] == null) {
        // Actual legacy102 bridge sends no owner/lease, so any wrapper wins.
        payload = value;
        identity = {'owner': '11111111111111111111111111111111',
          'generation': value['generation'],
          'lease': '11111111111111111111111111111111'};
        return null;
      }
      if (!_matches(value)) return _result(value, false);
      payload = value;
      return _result(value, true);
    }
    if (call.method == 'hide') {
      if (value['owner'] == null || _matches(value)) {
        visible = false;
        return value['owner'] == null ? null : _result(value, true);
      }
      return _result(value, false);
    }
    if (call.method == 'taskbar') {
      return value['owner'] == null ? null : _result(value, _matches(value));
    }
    throw StateError('unknown_fixed_notice_method');
  }
  void releaseGeneric() { _held!.complete(); }

  Future<Object?> send(String method, Map<String, Object?> value) async {
    final completed = Completer<Object?>();
    await messenger.handlePlatformMessage(channel.name,
      const StandardMethodCodec().encodeMethodCall(MethodCall(method, value)),
      (reply) {
        try { completed.complete(reply == null ? null
            : const StandardMethodCodec().decodeEnvelope(reply)); }
        catch (error, stack) { completed.completeError(error, stack); }
      },
    );
    return completed.future;
  }

  Future<void> background() async {
    final current = Map<String, Object?>.from(identity!);
    if (payload!['owner'] == null) {
      // Legacy native shows immediately and has no prepare handshake.
      visible = true;
      await send('presentation', {...current, 'revision': ++revision, 'visible': true});
      return;
    }
    final preparingRevision = ++revision;
    final reply = await send('prepare', {...current,
      'revision': preparingRevision, 'requiresFrame': true});
    final ack = reply is Map ? Map<String, Object?>.from(reply) : null;
    if (_matches(current) && revision == preparingRevision &&
        ack?['prepared'] == true && ack?['revision'] == preparingRevision &&
        ack?['owner'] == current['owner'] && ack?['lease'] == current['lease'] &&
        ack?['generation'] == current['generation']) {
      visible = true;
      await send('presentation', {...current,
        'revision': revision, 'visible': true});
    } else {
      visible = false;
      await send('presentation', {...current,
        'revision': ++revision, 'visible': false});
    }
  }
  Future<void> foreground() async {
    visible = false; // Native hides first, before notifying Flutter.
    await send('presentation', {...identity!,
      'revision': ++revision, 'visible': false});
  }
}

Future<void> _until(WidgetTester tester, bool Function() ready) async {
  for (var frame = 0; frame < 24 && !ready(); frame++) {
    await tester.pump(const Duration(milliseconds: 16));
  }
  expect(ready(), isTrue, reason: 'bounded fake channel/frame completion');
}

void main() {
  testWidgets('native external presenter replaces Flutter paint but retains barrier and Escape', (tester) async {
    const channel = MethodChannel('fixture/clipboard-presentation-one-owner');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final sink = _NativeSink(channel, messenger);
    messenger.setMockMethodCallHandler(channel, sink.call);
    final window = NativeTransferNoticeWindow(channel);
    final clipboard = _Clipboard();
    final controller = ClipboardTransferController(clipboard: clipboard, window: window);
    try {
      await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Scaffold(
        body: ClipboardTransferOverlay(controller: controller))));
      clipboard.begin(known: true);
      await tester.pump(const Duration(milliseconds: 701));
      await _until(tester, () => sink.percent == 25);
      final card = find.byKey(const Key('clipboard-transfer')).hitTestable();
      expect(card, findsOneWidget);
      var handedOff = false;
      final handoff = sink.background().then((_) => handedOff = true);
      await _until(tester, () => handedOff);
      await handoff;
      await tester.pump();
      expect(sink.visible, isTrue);
      final presenters = card.evaluate().length + (sink.visible ? 1 : 0);
      expect(presenters, 1, reason: 'one transfer must not paint two progress cards');
      expect(find.byKey(const Key('clipboard-transfer-barrier')), findsOneWidget);
      expect(controller.blocksInteraction, isTrue);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pump();
      expect(clipboard.cancels, 1);
      await sink.foreground();
      await tester.pump();
      expect(sink.visible, isFalse);
      expect(card, findsOneWidget);
      expect(find.byKey(const Key('clipboard-transfer-barrier')), findsOneWidget);
    } finally {
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
      channel.setMethodCallHandler(null);
      messenger.setMockMethodCallHandler(channel, null);
    }
  });

  testWidgets('held old-wrapper generic update cannot replace current known 25 percent', (tester) async {
    const channel = MethodChannel('fixture/clipboard-presentation-two-owners');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final sink = _NativeSink(channel, messenger)..holdGeneric = true;
    messenger.setMockMethodCallHandler(channel, sink.call);
    final oldWindow = NativeTransferNoticeWindow(channel);
    final oldClipboard = _Clipboard();
    final old = ClipboardTransferController(clipboard: oldClipboard, window: oldWindow);
    final currentWindow = NativeTransferNoticeWindow(channel);
    final currentClipboard = _Clipboard();
    final current = ClipboardTransferController(clipboard: currentClipboard, window: currentWindow);
    try {
      await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Scaffold(
        body: ClipboardTransferOverlay(controller: current))));
      oldClipboard.begin(known: false);
      await tester.pump(const Duration(milliseconds: 701));
      await _until(tester, () => sink.hasHeldGeneric);
      currentClipboard.begin(known: true);
      await tester.pump(const Duration(milliseconds: 701));
      await _until(tester, () => sink.percent == 25);
      sink.releaseGeneric();
      await tester.pump();
      await tester.pump();
      expect(sink.percent, 25,
          reason: 'a stale wrapper cannot replace the current owner with generic/-1');
      expect(current.view.percent, 25);
      expect(current.blocksInteraction, isTrue);
      old.dispose();
      await tester.pump();
      expect(sink.percent, 25, reason: 'old hide/taskbar cleanup keeps the current lease');
      expect(sink.acquisitions, 2, reason: 'one acquire per displayed generation, no reacquire');
    } finally {
      await tester.pumpWidget(const SizedBox.shrink());
      old.dispose();
      current.dispose();
      channel.setMethodCallHandler(null);
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
}
