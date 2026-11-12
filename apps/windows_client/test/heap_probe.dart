// Live objects of this test isolate after a full garbage collection, read from
// the Dart VM service over loopback. Only `flutter test --enable-vmservice`
// starts that service; without it [HeapProbe.connect] returns null and the
// memory tests skip. Measurement only: nothing in the code under test changes.
import 'dart:async';
import 'dart:convert';
import 'dart:developer';
import 'dart:io';
import 'dart:isolate';

class HeapProbe {
  HeapProbe._(this._socket, this._isolate) {
    _socket.listen((raw) {
      final reply = jsonDecode(raw as String) as Map<String, dynamic>;
      _pending.remove(reply['id'])?.complete(reply);
    });
  }
  final WebSocket _socket;
  final String _isolate;
  final _pending = <String, Completer<Map<String, dynamic>>>{};
  int _next = 0;

  static Future<HeapProbe?> connect() async {
    final uri = (await Service.getInfo()).serverWebSocketUri;
    final isolate = Service.getIsolateId(Isolate.current);
    if (uri == null || isolate == null) return null;
    return HeapProbe._(await WebSocket.connect(uri.toString()), isolate);
  }

  /// Live instances per class name after a full GC, plus `app`: every live
  /// object of a class in package:multitap_client (except [notApp]), and
  /// `heap`: the Dart heap in use (bytes; includes JIT code, so noisy).
  Future<Map<String, int>> live(
    Iterable<String> classes, {
    Set<String> notApp = const {},
  }) async {
    // Objects freed only by a finalizer the first collection queued go with
    // the second one.
    await _profile();
    final profile = await _profile();
    final counts = {for (final name in classes) name: 0, 'app': 0};
    for (final member in profile['members'] as List) {
      final type = member['class'] as Map;
      final name = type['name'] as String?,
          live = member['instancesCurrent'] as int;
      if (counts.containsKey(name)) counts[name!] = counts[name]! + live;
      final library = (type['library'] as Map?)?['uri'] as String?;
      if (library != null &&
          library.startsWith('package:multitap_client/') &&
          !notApp.contains(name)) {
        counts['app'] = counts['app']! + live;
      }
    }
    counts['heap'] = (profile['memoryUsage'] as Map)['heapUsage'] as int;
    return counts;
  }

  /// The length of collection [field] of the one live [className] instance.
  Future<int> fieldLength(String className, String field) async {
    final profile = await _profile();
    final member = (profile['members'] as List).firstWhere(
      (m) => (m['class'] as Map)['name'] == className,
    );
    final instances = await _call('getInstances', {
      'objectId': (member['class'] as Map)['id'],
      'limit': 2,
    });
    final list = instances['instances'] as List;
    if (list.length != 1) throw StateError('${list.length} $className');
    final object = await _call('getObject', {'objectId': list.single['id']});
    final value = (object['fields'] as List).firstWhere(
      (f) => (f['decl'] as Map)['name'] == field,
    )['value'];
    return (value as Map)['length'] as int;
  }

  Future<Map<String, dynamic>> _profile() =>
      _call('getAllocationProfile', {'gc': true});

  Future<Map<String, dynamic>> _call(
    String method,
    Map<String, Object?> params,
  ) async {
    final id = '${++_next}';
    final reply = _pending[id] = Completer<Map<String, dynamic>>();
    _socket.add(
      jsonEncode({
        'jsonrpc': '2.0',
        'id': id,
        'method': method,
        'params': {'isolateId': _isolate, ...params},
      }),
    );
    final value = await reply.future.timeout(const Duration(seconds: 60));
    if (value['error'] != null) throw StateError('${value['error']}');
    return value['result'] as Map<String, dynamic>;
  }

  Future<void> close() => _socket.close();
}
