// Support for the clipboard end-to-end harness (tools/clipboard-e2e-qa/README.md): the relay, the Host helper and the client
// helper as child processes, a MethodChannel mock that forwards the real NativeClipboardBridge to the client helper, and the
// content builders and comparisons of the scenarios.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

const _channel = MethodChannel('multitap/clipboard');

/// A child process that speaks JSON lines: requests carry an `id`, replies echo it, other lines are events.
class LineProcess {
  LineProcess._(this.name, this._process);
  final String name;
  final Process _process;
  final events = <Map<String, dynamic>>[];
  final stderrText = StringBuffer();
  final _pending = <int, Completer<Object?>>{};
  final _eventWaiters =
      <bool Function(Map<String, dynamic>), Completer<Map<String, dynamic>>>{};
  void Function(Map<String, dynamic>)? onEvent;
  int _nextId = 1;
  bool exited = false;

  static Future<LineProcess> start(
    String name,
    String executable,
    List<String> arguments,
  ) async {
    final process = await Process.start(
      executable,
      arguments,
      runInShell: false,
    );
    final line = LineProcess._(name, process);
    process.stdout
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(line._line);
    process.stderr.transform(utf8.decoder).listen(line.stderrText.write);
    unawaited(
      process.exitCode.then((_) {
        line.exited = true;
        for (final waiter in line._pending.values) {
          if (!waiter.isCompleted) {
            waiter.completeError(
              StateError('$name exited: ${line.stderrText}'),
            );
          }
        }
      }),
    );
    return line;
  }

  void _line(String text) {
    if (text.trim().isEmpty) return;
    final Map<String, dynamic> value;
    try {
      value = jsonDecode(text) as Map<String, dynamic>;
    } catch (_) {
      stderrText.writeln('unparsable line: $text');
      return;
    }
    final id = value['id'];
    if (id is int) {
      final waiter = _pending.remove(id);
      if (waiter == null) return;
      if (value.containsKey('err')) {
        waiter.completeError(
          value['err'] is Map
              ? PlatformException(
                  code: (value['err'] as Map)['code'] as String,
                  message: (value['err'] as Map)['message'] as String?,
                )
              : StateError('$name: ${value['err']}'),
        );
      } else {
        waiter.complete(value['ok']);
      }
      return;
    }
    events.add(value);
    onEvent?.call(value);
    for (final entry in _eventWaiters.entries.toList()) {
      if (entry.key(value)) {
        _eventWaiters.remove(entry.key);
        entry.value.complete(value);
      }
    }
  }

  Future<Object?> request(
    Map<String, Object?> body, {
    Duration timeout = const Duration(seconds: 120),
  }) {
    final id = _nextId++;
    final completer = Completer<Object?>();
    _pending[id] = completer;
    _process.stdin.writeln(jsonEncode({'id': id, ...body}));
    return completer.future.timeout(
      timeout,
      onTimeout: () {
        _pending.remove(id);
        throw TimeoutException(
          '$name did not answer ${body['cmd'] ?? body['method']}',
        );
      },
    );
  }

  Future<Map<String, dynamic>> waitEvent(
    bool Function(Map<String, dynamic>) test, {
    Duration timeout = const Duration(seconds: 30),
  }) {
    final existing = events.where(test);
    if (existing.isNotEmpty) return Future.value(existing.first);
    final completer = Completer<Map<String, dynamic>>();
    _eventWaiters[test] = completer;
    return completer.future.timeout(
      timeout,
      onTimeout: () {
        _eventWaiters.remove(test);
        throw TimeoutException('$name: no matching event');
      },
    );
  }

  Future<void> stop() async {
    if (!exited) {
      try {
        _process.stdin.writeln(jsonEncode({'id': _nextId++, 'cmd': 'quit'}));
        await _process.stdin.flush();
      } catch (_) {}
      await _process.exitCode.timeout(
        const Duration(seconds: 5),
        onTimeout: () {
          _process.kill();
          return -1;
        },
      );
    }
  }
}

class Relay {
  Relay._(this.process, this.info, this.closures);
  final Process process;
  final Map<String, dynamic> info;
  final List<String> closures;
  String get version => info['relayVersion'] as String;
  String get clientUrl => info['clientUrl'] as String;
  String get hostUrl => info['hostUrl'] as String;

  Future<void> setPolicy(Map<String, bool> policy) async {
    process.stdin.writeln(jsonEncode({'cmd': 'policy', 'policy': policy}));
    await process.stdin.flush();
  }

  static Future<Relay> start(
    String node,
    String fixture,
    String serverDir, {
    bool legacy = false,
    String? policy,
  }) async {
    final process = await Process.start(node, [
      fixture,
      '--server',
      serverDir,
      if (legacy) '--legacy',
      if (policy != null) ...['--policy', policy],
    ]);
    final closures = <String>[];
    final ready = Completer<Map<String, dynamic>>();
    process.stdout
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen((line) {
          if (line.trim().isEmpty) return;
          final value = jsonDecode(line) as Map<String, dynamic>;
          if (value['ready'] == true && !ready.isCompleted) {
            ready.complete(value);
          } else if (value['event'] == 'pairClosed') {
            closures.add(value['reason'] as String);
          }
        });
    final errors = StringBuffer();
    process.stderr.transform(utf8.decoder).listen(errors.write);
    unawaited(
      process.exitCode.then((code) {
        if (!ready.isCompleted) {
          ready.completeError(StateError('relay exited $code: $errors'));
        }
      }),
    );
    final info = await ready.future.timeout(const Duration(seconds: 30));
    return Relay._(process, info, closures);
  }

  Future<void> stop() async {
    try {
      await process.stdin.close();
    } catch (_) {}
    await process.exitCode.timeout(
      const Duration(seconds: 5),
      onTimeout: () {
        process.kill();
        return -1;
      },
    );
  }
}

dynamic _toJson(Object? value) {
  if (value is Uint8List) return {r'$b': base64Encode(value)};
  if (value is List) return value.map(_toJson).toList();
  if (value is Map) {
    return {
      for (final entry in value.entries)
        entry.key as String: _toJson(entry.value),
    };
  }
  return value;
}

dynamic _fromJson(Object? value) {
  if (value is Map) {
    if (value.length == 1 && value.containsKey(r'$b')) {
      return Uint8List.fromList(base64Decode(value[r'$b'] as String));
    }
    return <Object?, Object?>{
      for (final entry in value.entries) entry.key: _fromJson(entry.value),
    };
  }
  if (value is List) return value.map(_fromJson).toList();
  return value;
}

/// Forwards every call of the real NativeClipboardBridge to the client helper and delivers the helper's native-to-Dart
/// events (`changed`, `pull`) the way the Windows runner would.
void attachNativeChannel(LineProcess client) {
  final binding = TestDefaultBinaryMessengerBinding.instance;
  binding.defaultBinaryMessenger.setMockMethodCallHandler(_channel, (
    call,
  ) async {
    final result = await client.request({
      'method': call.method,
      'args': _toJson(call.arguments),
    });
    return _fromJson(result);
  });
  client.onEvent = (event) {
    final kind = event['event'];
    if (kind != 'changed' && kind != 'pull') return;
    final arguments = <String, Object?>{...event}..remove('event');
    final encoded = const StandardMethodCodec().encodeMethodCall(
      MethodCall(kind as String, arguments),
    );
    binding.defaultBinaryMessenger.handlePlatformMessage(
      'multitap/clipboard',
      encoded,
      (_) {},
    );
  };
}

void detachNativeChannel() {
  TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .setMockMethodCallHandler(_channel, null);
}

Future<void> eventually(
  FutureOr<bool> Function() condition,
  String what, {
  Duration timeout = const Duration(seconds: 40),
  Duration step = const Duration(milliseconds: 100),
}) async {
  final end = DateTime.now().add(timeout);
  while (DateTime.now().isBefore(end)) {
    if (await condition()) return;
    await Future<void>.delayed(step);
  }
  throw TimeoutException('Timed out waiting for $what');
}

// ---- content ------------------------------------------------------------------------------------------------

final _crc = () {
  final table = List<int>.filled(256, 0);
  for (var n = 0; n < 256; n++) {
    var c = n;
    for (var k = 0; k < 8; k++) {
      c = (c & 1) != 0 ? 0xedb88320 ^ (c >> 1) : c >> 1;
    }
    table[n] = c;
  }
  return table;
}();

int _crc32(List<int> bytes) {
  var c = 0xffffffff;
  for (final b in bytes) {
    c = _crc[(c ^ b) & 0xff] ^ (c >> 8);
  }
  return c ^ 0xffffffff;
}

/// A valid RGBA PNG of noise (so it does not compress away and needs several chunks).
Uint8List makePng(int width, int height, {int seed = 1}) {
  final random = Random(seed);
  final raw = BytesBuilder();
  for (var y = 0; y < height; y++) {
    raw.addByte(0);
    raw.add(List<int>.generate(width * 4, (_) => random.nextInt(256)));
  }
  Uint8List chunk(String type, List<int> data) {
    final body = [...ascii.encode(type), ...data];
    final out = BytesBuilder()
      ..add((ByteData(4)..setUint32(0, data.length)).buffer.asUint8List())
      ..add(body)
      ..add((ByteData(4)..setUint32(0, _crc32(body))).buffer.asUint8List());
    return out.toBytes();
  }

  final header = ByteData(13)
    ..setUint32(0, width)
    ..setUint32(4, height)
    ..setUint8(8, 8)
    ..setUint8(9, 6);
  return (BytesBuilder()
        ..add([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])
        ..add(chunk('IHDR', header.buffer.asUint8List()))
        ..add(chunk('IDAT', ZLibCodec().encode(raw.toBytes())))
        ..add(chunk('IEND', const [])))
      .toBytes();
}

Uint8List noise(int size, int seed) {
  final random = Random(seed);
  final out = Uint8List(size);
  for (var i = 0; i < size; i++) {
    out[i] = random.nextInt(256);
  }
  return out;
}

Future<String> writeFile(
  Directory directory,
  String relative,
  List<int> bytes,
) async {
  final file = File(
    '${directory.path}${Platform.pathSeparator}${relative.replaceAll('/', Platform.pathSeparator)}',
  );
  await file.parent.create(recursive: true);
  await file.writeAsBytes(bytes);
  return file.path;
}

/// A file of [size] pseudo-random bytes written in pieces (no multi-megabyte list in memory).
Future<String> writeBigFile(
  Directory directory,
  String name,
  int size,
  int seed,
) async {
  final file = File('${directory.path}${Platform.pathSeparator}$name');
  await directory.create(recursive: true);
  final sink = file.openWrite();
  final random = Random(seed);
  var left = size;
  while (left > 0) {
    final part = min(left, 1 << 20);
    sink.add(
      Uint8List.fromList(List<int>.generate(part, (_) => random.nextInt(256))),
    );
    left -= part;
  }
  await sink.close();
  return file.path;
}

/// "HTML Format" clipboard payload around [fragment], with correct header offsets.
String cfHtml(String fragment) {
  const prefix = '<html><body><!--StartFragment-->';
  const suffix = '<!--EndFragment--></body></html>';
  String header(int a, int b, int c, int d) =>
      'Version:0.9\r\nStartHTML:${a.toString().padLeft(10, '0')}\r\nEndHTML:${b.toString().padLeft(10, '0')}\r\n'
      'StartFragment:${c.toString().padLeft(10, '0')}\r\nEndFragment:${d.toString().padLeft(10, '0')}\r\n';
  final size = header(0, 0, 0, 0).length;
  final start = size + prefix.length;
  final end = start + utf8.encode(fragment).length;
  return header(size, end + suffix.length, start, end) +
      prefix +
      fragment +
      suffix;
}

/// The entries (path, size, sha) of a described files board, folders marked, sorted.
List<String> entryKeys(Map<dynamic, dynamic> board) {
  final entries = (board['entries'] as List).cast<Map>();
  return [
    for (final entry in entries)
      entry['dir'] == true
          ? '${entry['path']}/'
          : '${entry['path']}:${entry['size']}:${entry['sha']}',
  ]..sort();
}
