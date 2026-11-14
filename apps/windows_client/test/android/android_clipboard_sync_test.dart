import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import '../clipboard_onpaste_test.dart' as onpaste;
import '../clipboard_session_test.dart' as base;

// Android's automatic clipboard, through the real ClipboardSession
// and NativeClipboardBridge(android: true). AndroidClipboard.kt is played by
// [AndroidClipboardFake] on `multitap/clipboard` (a plain-text phone clipboard,
// the foreground watch, the mark on a clip this app placed); the relay by
// base.ClipboardSocket, answering the headers the session dialled with.

const _channel = MethodChannel('multitap/clipboard');
TestDefaultBinaryMessenger get _messenger =>
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

class AndroidClipboardFake {
  String? text;

  /// The clip carries this app's mark (a VM copy this app placed).
  bool marked = false;
  int stamp = 0, seen = -1, sequence = 1;
  bool focused = true;
  String? watching;
  final calls = <String>[];
  Uint8List? _source;
  List<BytesBuilder>? _parts;

  void install() => _messenger.setMockMethodCallHandler(_channel, call);
  void uninstall() => _messenger.setMockMethodCallHandler(_channel, null);

  /// Copied in another app: Android's listener runs only while watched.
  void copyElsewhere(String value) {
    text = value;
    marked = false;
    stamp++;
    if (watching != null) _observe();
  }

  /// MainActivity.onWindowFocusChanged.
  void focus(bool value) {
    focused = value;
    if (value && watching != null) _observe();
  }

  void _observe() {
    if (!focused || stamp == seen) return;
    seen = stamp;
    if (marked) return;
    sequence = (sequence + 1) & 0xffffffff;
    final owner = watching;
    if (owner == null) return;
    // channel.invokeMethod: reaches Dart after the current reply.
    Timer.run(
      () => _messenger.handlePlatformMessage(
        _channel.name,
        _channel.codec.encodeMethodCall(
          MethodCall('changed', {'owner': owner, 'sequence': sequence}),
        ),
        (_) {},
      ),
    );
  }

  Future<Object?> call(MethodCall call) async {
    calls.add(call.method);
    final args = (call.arguments as Map).cast<String, Object?>();
    final owner = args['owner'] as String;
    switch (call.method) {
      case 'watch':
        if (args['enabled'] == true) {
          watching = owner;
          if (seen == -1) {
            seen = stamp;
          } else {
            _observe();
          }
        } else if (watching == owner) {
          watching = null;
        }
        return {'sequence': sequence};
      case 'capture':
        final automatic = args['skipOwn'] == true;
        if (automatic && marked) return {'own': true, 'sequence': sequence};
        final value = text;
        if (value == null) {
          if (automatic) return {'own': true, 'sequence': sequence};
          throw PlatformException(code: 'clipboard_empty');
        }
        _source = utf8.encode(value);
        return {
          'format': 'text',
          'items': [
            {'size': _source!.length, 'units': value.length},
          ],
          'sequence': sequence,
        };
      case 'sourceChunk':
        final offset = args['offset'] as int, length = args['length'] as int;
        expect(length, inInclusiveRange(1, 65536));
        return Uint8List.fromList(_source!.sublist(offset, offset + length));
      case 'sourceDigest':
        return sha256.convert(_source!).toString();
      case 'begin':
        if (args['format'] != 'text' && args['format'] != 'richtext') {
          throw PlatformException(code: 'unsupported_clipboard_format');
        }
        _parts = [for (final _ in args['items'] as List) BytesBuilder()];
        return null;
      case 'append':
        _parts![args['index'] as int].add(args['bytes'] as Uint8List);
        return null;
      case 'finish':
        final part = _parts![args['index'] as int].toBytes();
        if (sha256.convert(part).toString() != args['digest']) {
          throw PlatformException(code: 'clipboard_transfer_failed');
        }
        return null;
      case 'commit':
        final unless = args['unlessSequence'];
        if (unless == null || unless != sequence) {
          // Only the plain text reaches the phone's clipboard.
          text = utf8.decode(_parts!.first.toBytes());
          marked = true;
          stamp++;
          sequence = (sequence + 1) & 0xffffffff;
          if (watching != null) _observe();
        }
        _parts = null;
        return {'sequence': sequence};
      case 'published':
        return {'owned': marked, 'sequence': sequence};
      case 'cancel' || 'release':
        return null;
      case 'pullSupersede' ||
          'pullAbort' ||
          'pullRetire' ||
          'withdrawOffer' ||
          'pullShutdown' ||
          'pullClose' ||
          'closeOffers':
        return null;
      case 'dropOffers':
        return <String>[];
      case 'publishOffer' || 'publishPendingOffer' || 'pullOpen':
        throw PlatformException(code: 'unsupported_clipboard_format');
    }
    throw MissingPluginException();
  }
}

Map<String, bool> _policy({bool toRemote = true, bool toLocal = true}) => {
  'textToRemote': toRemote,
  'textToLocal': toLocal,
  // Files are granted: they still never move to or from a phone.
  'filesToRemote': true,
  'filesToLocal': true,
};

Future<void> _pause([int ms = 5]) =>
    Future<void>.delayed(Duration(milliseconds: ms));

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  late AndroidClipboardFake phone;
  late base.ClipboardSocket socket;
  late ClipboardSession session;

  ClipboardSession make() => ClipboardSession(
    url: Uri.parse(
      'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
    ),
    token: 'test-token',
    sessionId: base.sessionId,
    policy: const ClipboardPolicy.all(),
    syncSupported: true,
    bridge: NativeClipboardBridge(
      allowedText: true,
      allowedFiles: true,
      android: true,
    ),
    dialer: (_, _) async => socket,
  );

  /// The relay's ready for the headers this session dialled: the on-paste
  /// pair (and settled reads) only when it announced them, as server/src/clipboard.mjs.
  Future<void> connect(Map<String, bool> policy) async {
    final headers = session.dialHeaders;
    final onPaste = headers.containsKey(clipboardOnPasteHeader);
    final starting = session.start();
    await _pause();
    socket.emit({
      ...base.ready,
      'clipboardSync': true,
      'clipboardPolicy': policy,
      if (onPaste) 'limits': onpaste.onPasteLimits(),
      if (onPaste && headers.containsKey(clipboardSettledHeader))
        'clipboardSettled': clipboardSettledProtocol,
    });
    await starting;
  }

  /// Input captured (the session in front), then a settled moment.
  Future<void> activate() async {
    await session.setSharingActive(true);
    await _pause(200);
  }

  setUp(() {
    phone = AndroidClipboardFake()..install();
    socket = base.ClipboardSocket();
    session = make();
  });
  tearDown(() async {
    await session.close();
    phone.uninstall();
  });

  List<Map<String, dynamic>> sent(String type) => [
    for (final value in socket.sent)
      if (value['type'] == type) value,
  ];

  /// A VM copy the Host announced settled: the read it answers.
  Future<Map<String, dynamic>> vmCopied(int sequence) async {
    socket.emit({
      'type': 'clipboard.changed',
      'sessionId': base.sessionId,
      'sequence': sequence,
      'settled': clipboardSettledProtocol,
    });
    return socket.nextMessage('clipboard.read');
  }

  /// The Host pushes [items] ((kind, text) in manifest order) for [read].
  Future<void> push(
    Map<String, dynamic> read,
    List<(String, String)> items, {
    bool richtext = false,
  }) async {
    final id = read['requestId'] as String;
    final bytes = [for (final item in items) utf8.encode(item.$2)];
    socket.emit({
      'type': 'clipboard.begin',
      'requestId': id,
      'format': richtext ? 'richtext' : 'text',
      if (!richtext) 'size': bytes.single.length,
      if (richtext)
        'items': [
          for (var i = 0; i < items.length; i++)
            {'kind': items[i].$1, 'size': bytes[i].length},
        ],
    });
    for (var i = 0; i < bytes.length; i++) {
      socket.emit({
        'type': 'clipboard.chunk',
        'requestId': id,
        'index': i,
        'offset': 0,
        'data': base64Encode(bytes[i]),
      });
      socket.emit({
        'type': 'clipboard.item.end',
        'requestId': id,
        'index': i,
        'sha256': sha256.convert(bytes[i]).toString(),
      });
      await socket.nextMessage('clipboard.item.ack');
    }
    socket.emit({'type': 'clipboard.commit', 'requestId': id});
    await socket.nextMessage('clipboard.applied');
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
    await _pause(20);
  }

  /// Another app in front, then Multitap again: Android releases input on
  /// focus loss and the view captures it again on return.
  Future<void> awayAndBack({
    void Function()? meanwhile,
    bool focusFirst = true,
  }) async {
    phone.focus(false);
    await session.setSharingActive(false);
    meanwhile?.call();
    if (focusFirst) phone.focus(true);
    await session.setSharingActive(true);
    if (!focusFirst) phone.focus(true);
    await _pause(200);
  }

  test('the Android adapter takes part in the on-paste pair with settled '
      'reads; it has no pending placeholder or pull queue', () {
    expect(session.dialHeaders, {
      clipboardLimitsHeader: clipboardLimitsHeaderValue,
      clipboardFormatsHeader: clipboardFormatsHeaderValue,
      clipboardPullHeader: clipboardPullHeaderValue,
      clipboardScaleHeader: clipboardScaleHeaderValue,
      clipboardOnPasteHeader: clipboardOnPasteHeaderValue,
      clipboardSettledHeader: clipboardSettledHeaderValue,
      clipboardLivePolicyHeader: clipboardLivePolicyHeaderValue,
    });
  });

  group('VM → phone', () {
    test('a VM copy lands on the phone clipboard when text to this device '
        'is granted; the read is bound to a settled text/image copy, so a '
        'file copy is never read', () async {
      await connect(_policy());
      await activate();
      final read = await vmCopied(7);
      expect(read['expectedSequence'], 7);
      expect(read['textImageOnly'], isTrue);
      await push(read, [('text', 'VM 텍스트')]);
      expect(phone.text, 'VM 텍스트');
      expect(phone.marked, isTrue);
      expect(session.error, isNull);
    });

    test('rich text gives the phone its plain text', () async {
      await connect(_policy());
      await activate();
      final read = await vmCopied(8);
      await push(read, [
        ('text', 'plain'),
        ('html', '<b>plain</b>'),
      ], richtext: true);
      expect(phone.text, 'plain');
    });

    test('an image and a large offered copy are refused quietly; the channel '
        'stays', () async {
      await connect(_policy());
      await activate();
      final image = await vmCopied(9);
      socket.emit({
        'type': 'clipboard.begin',
        'requestId': image['requestId'],
        'format': 'image',
        'items': [
          {'kind': 'png', 'size': 10, 'width': 2, 'height': 2},
        ],
      });
      final refused = await socket.nextMessage('clipboard.error');
      expect(refused['reason'], 'unsupported_clipboard_format');
      await _pause(20);
      final offer = await vmCopied(10);
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': offer['requestId'],
        'format': 'text',
        'size': 2 * 1024 * 1024,
      });
      final declined = await socket.nextMessage('clipboard.error');
      expect(declined['reason'], 'unsupported_clipboard_format');
      expect(phone.text, isNull);
      expect(session.closed, isFalse);
      expect(session.failureQuiet, isTrue);
    });

    test('nothing is read without the text grant to this device (files '
        'alone do not count)', () async {
      await connect(_policy(toLocal: false));
      await activate();
      socket.emit({
        'type': 'clipboard.changed',
        'sessionId': base.sessionId,
        'sequence': 7,
        'settled': clipboardSettledProtocol,
      });
      await _pause(100);
      expect(sent('clipboard.read'), isEmpty);
      expect(phone.text, isNull);
      expect(phone.calls, isNot(contains('begin')));
    });
  });

  group('phone → VM', () {
    for (final focusFirst in [true, false]) {
      test('a copy made in another app goes to the VM when the session is '
          'back in front (${focusFirst ? 'focus, then input' : 'input, then '
                    'focus'})', () async {
        await connect(_policy());
        await activate();
        expect(sent('clipboard.begin'), isEmpty, reason: 'nothing copied');
        await awayAndBack(
          meanwhile: () => phone.copyElsewhere('휴대폰 텍스트'),
          focusFirst: focusFirst,
        );
        final begin = await socket.nextMessage('clipboard.begin');
        expect(begin['format'], 'text');
        expect(begin['size'], utf8.encode('휴대폰 텍스트').length);
        final chunk = await socket.nextMessage('clipboard.chunk');
        expect(utf8.decode(base64Decode(chunk['data'] as String)), '휴대폰 텍스트');
      });
    }

    test('the phone clipboard is not even read without the text grant to the '
        'VM (files alone do not count)', () async {
      await connect(_policy(toRemote: false));
      await activate();
      await awayAndBack(meanwhile: () => phone.copyElsewhere('비밀'));
      expect(sent('clipboard.begin'), isEmpty);
      expect(phone.calls, isNot(contains('capture')));
    });
  });

  group('no echo', () {
    test('a VM copy on the phone is never sent back: not on return, not '
        'when it was placed while another app had the focus, not by the next '
        'connection', () async {
      await connect(_policy());
      await activate();
      await push(await vmCopied(7), [('text', 'from VM')]);
      await awayAndBack();
      expect(sent('clipboard.begin'), isEmpty);

      // Left within the grace: the copy still arrives, without the focus.
      phone.focus(false);
      await session.setSharingActive(false);
      await push(await vmCopied(8), [('text', 'while away')]);
      expect(phone.text, 'while away');
      phone.focus(true);
      await session.setSharingActive(true);
      await _pause(200);
      expect(sent('clipboard.begin'), isEmpty);

      await session.close();
      socket = base.ClipboardSocket();
      session = make();
      await connect(_policy());
      await activate();
      expect(sent('clipboard.begin'), isEmpty);
      expect(phone.text, 'while away');
    });
  });

  test(
    'a permission withdrawn mid-session ends sharing at once: the copy '
    'still arriving is dropped, the watch stops, nothing is read again',
    () async {
      await connect(_policy());
      await activate();
      final read = await vmCopied(7);
      final bytes = utf8.encode('half');
      socket.emit({
        'type': 'clipboard.begin',
        'requestId': read['requestId'],
        'format': 'text',
        'size': bytes.length,
      });
      socket.emit({
        'type': 'clipboard.chunk',
        'requestId': read['requestId'],
        'index': 0,
        'offset': 0,
        'data': base64Encode(bytes),
      });
      await _pause(20);
      socket.emit({
        'type': 'clipboard.error',
        'reason': 'clipboard_policy_changed',
      });
      await _pause(50);
      expect(session.closed, isTrue);
      expect(session.canReconnect, isFalse);
      expect(session.error, 'clipboard_policy_changed');
      expect(session.status, '관리자가 클립보드 권한을 바꿨어요.');
      expect(phone.watching, isNull);
      expect(phone.calls, isNot(contains('commit')));
      expect(phone.text, isNull);
      final before = phone.calls.length;
      phone.copyElsewhere('after');
      phone.focus(false);
      phone.focus(true);
      await session.setSharingActive(true);
      await _pause(150);
      expect(phone.calls.skip(before), isEmpty);
    },
  );
}
