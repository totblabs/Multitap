import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_onpaste_test.dart' as onpaste;
import 'clipboard_session_test.dart' as base;

/// The Windows adapter's clipboard thread while Explorer reads a pasted file:
/// a job queued for that thread (here the transfer's release) runs only once
/// the read it is inside got its bytes. Pull data never needs that thread.
class _ReadingClipboard extends onpaste.OnPasteClipboard {
  Completer<void>? reading;
  @override
  Future<void> release(String owner) async {
    await reading?.future;
    await super.release(owner);
  }

  @override
  Future<void> pullData(int rangeId, int offset, Uint8List bytes) async {
    await super.pullData(rangeId, offset, bytes);
    final waiting = reading;
    if (waiting != null && !waiting.isCompleted) waiting.complete();
  }
}

// A paste on this PC that starts right after the VM's new copy is published
// must not hang until the user cancels it. Explorer reads the new offer on the
// clipboard thread; the relay's clipboard.complete for that offer queues the
// transfer's release behind the read; the read waits for the pull's data,
// which the socket handler must take without awaiting that release.
void main() {
  late base.ClipboardSocket socket;
  late _ReadingClipboard bridge;
  late ClipboardSession session;

  setUp(() {
    socket = base.ClipboardSocket();
    bridge = _ReadingClipboard();
    session = ClipboardSession(
      policy: const ClipboardPolicy.all(),
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
      ),
      token: 'test-token',
      sessionId: base.sessionId,
      bridge: bridge,
      dialer: (url, token) async => socket,
      pullLinger: Duration.zero,
    );
  });
  tearDown(() async {
    final reading = bridge.reading;
    if (reading != null && !reading.isCompleted) reading.complete();
    await session.close();
  });

  test('a paste that reads a VM copy as it is published gets its data while the copy completes', () async {
    final starting = session.start();
    await base.settle();
    socket.emit(onpaste.onPasteReady());
    await starting;

    final receiving = session.receiveRemote();
    final read = await socket.nextMessage('clipboard.read');
    final id = read['requestId'] as String;
    socket.emit({
      'type': 'clipboard.offer',
      'requestId': id,
      'format': 'tree',
      'entries': 1,
      'files': 1,
      'bytes': 1000,
      'tops': 1,
      'skipped': 0,
    });
    await base.settle();
    socket.emit({
      'type': 'clipboard.entries',
      'requestId': id,
      'start': 0,
      'entries': [
        {
          'path': 'a.bin',
          'kind': 'file',
          'size': 1000,
          'attrs': 32,
          'mtime': 1761500000123,
        },
      ],
    });
    await socket.nextMessage('clipboard.applied');

    // Explorer reads the new copy at once: the clipboard thread waits inside
    // that read until its first bytes arrive.
    bridge.reading = Completer<void>();
    bridge.platform(ClipboardPullRequest(11, id, 0, 0));
    final pull = await socket.nextMessage('clipboard.pull');
    // The relay completes the offer, and the Host serves the pull.
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
    final bytes = Uint8List.fromList(List.generate(1000, (i) => i % 251));
    socket.emit({
      'type': 'clipboard.chunk',
      'requestId': pull['requestId'],
      'index': 0,
      'offset': 0,
      'data': base64Encode(bytes),
    });

    final completed = await receiving
        .then((_) => true)
        .timeout(const Duration(seconds: 2), onTimeout: () => false);
    final delivered = [for (final d in bridge.data) d.$1];
    expect(delivered, [
      11,
    ], reason: 'the pull data waited behind the transfer that waited for it');
    expect(completed, isTrue);
    expect(session.closed, isFalse);
    expect(session.error, isNull);

    socket.emit({
      'type': 'clipboard.item.end',
      'requestId': pull['requestId'],
      'index': 0,
      'sha256': 'c' * 64,
    });
    await base.settle();
    expect(bridge.ended, [(11, 'c' * 64)]);
  });

  test('pull data keeps flowing while a transfer message waits for the clipboard thread; order holds within each lane', () async {
    final starting = session.start();
    await base.settle();
    socket.emit(onpaste.onPasteReady());
    await starting;
    // A published VM copy that a paste is reading.
    final receiving = session.receiveRemote();
    final read = await socket.nextMessage('clipboard.read');
    final id = read['requestId'] as String;
    socket.emit({
      'type': 'clipboard.offer',
      'requestId': id,
      'format': 'tree',
      'entries': 1,
      'files': 1,
      'bytes': 100000,
      'tops': 1,
      'skipped': 0,
    });
    await base.settle();
    socket.emit({
      'type': 'clipboard.entries',
      'requestId': id,
      'start': 0,
      'entries': [
        {
          'path': 'a.bin',
          'kind': 'file',
          'size': 100000,
          'attrs': 32,
          'mtime': 1761500000123,
        },
      ],
    });
    await socket.nextMessage('clipboard.applied');
    bridge.reading = Completer<void>();
    bridge.platform(ClipboardPullRequest(21, id, 0, 0));
    final pull = await socket.nextMessage('clipboard.pull');
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
    final data = Uint8List.fromList(List.generate(100000, (i) => i % 200));
    for (var at = 0; at < data.length; at += 46080) {
      final end = at + 46080 > data.length ? data.length : at + 46080;
      socket.emit({
        'type': 'clipboard.chunk',
        'requestId': pull['requestId'],
        'index': 0,
        'offset': at,
        'data': base64Encode(data.sublist(at, end)),
      });
    }
    await receiving.timeout(const Duration(seconds: 2));
    expect([for (final d in bridge.data) d.$2], [0, 46080, 92160]);
  });
}
