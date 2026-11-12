import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/client_diagnostics.dart';
import 'package:multitap_client/data/client_build_version.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/stream_stats_report.dart';

import 'display_api_test.dart' show displayResponse;
import 'power_api_test.dart'
    show ApiClient, ApiRequest, ApiResponse, ApiHeaders, taskId;

class DiagnosticHeaders extends ApiHeaders {
  @override
  String? value(String name) => name == 'x-request-id' ? taskId : null;
}

class DiagnosticResponse extends ApiResponse {
  DiagnosticResponse(super.statusCode, super.value);
  @override
  final headers = DiagnosticHeaders();
}

class DiagnosticRequest extends ApiRequest {
  DiagnosticRequest(String method, Uri uri, this.owner)
    : super(method, uri, ApiResponse(200, {}));
  final DiagnosticClient owner;
  @override
  Future<HttpClientResponse> close() async {
    if (uri.path == '/api/v1/client/diagnostics') {
      final report = jsonDecode(body.toString()) as Map<String, dynamic>;
      owner.reports.add(report);
      if (owner.uploadStatus != 202) {
        return DiagnosticResponse(owner.uploadStatus, {
          'error': {'code': owner.uploadCode},
        });
      }
      return DiagnosticResponse(202, {
        'accepted': true,
        'protocol': report['protocol'],
        'runId': report['runId'],
        'sequence': report['sequence'],
        if (owner.ackStreamStats) 'streamStatsProtocol': 'stream-stats-v1',
        if (owner.ackStall) 'streamStallProtocol': 'stream-stall-v1',
      });
    }
    if (uri.path.endsWith('/display')) {
      final value = displayResponse(status: 'failed');
      (value['display'] as Map)['operation']['error'] =
          'display_configuration_failed';
      return DiagnosticResponse(202, value);
    }
    if (uri.path.endsWith('/logout')) {
      return DiagnosticResponse(200, {'ok': true});
    }
    return DiagnosticResponse(200, {'desktops': []});
  }
}

class DiagnosticClient extends ApiClient {
  final reports = <Map<String, dynamic>>[];
  int uploadStatus = 202;
  String uploadCode = 'not_found';
  bool ackStreamStats = false, ackStall = false;
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final request = DiagnosticRequest(method, uri, this);
    requests.add(request);
    return request;
  }
}

void main() {
  test('ProductLog preparation failure never inherits the previously bound session', () async {
    const oldSession = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa';
    const newSession = 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb';
    final log = ProductLog(), sent = <Map<String, Object?>>[];
    final reporter = ClientDiagnostics(
      log: log,
      desktopId: 'pve-170',
      mode: 'stream',
      interval: const Duration(hours: 1),
      send: (report) async {
        sent.add(jsonDecode(jsonEncode(report)) as Map<String, Object?>);
        return DiagnosticDelivery.accepted;
      },
    );
    addTearDown(() {
      reporter.close();
      log.dispose();
    });
    reporter.bindSession(oldSession);
    log.recordFailure(LogEvent.streamEnd, 'capture_failed', {'ok': false, 'stage': 1});
    log.recordPreparationFailure(LogEvent.streamEnd, 'display_control_unavailable', {'ok': false, 'stage': 3});
    log.record(LogEvent.viewportResult, {'ok': true, 'stage': 4});
    reporter.bindSession(newSession);
    log.record(LogEvent.streamReady, {'ok': true, 'stage': 5});
    await reporter.flush();
    final rows = (sent.single['events'] as List).cast<Map>();
    Map stage(int value) => rows.singleWhere((row) => (row['values'] as Map)['stage'] == value);
    expect(stage(1)['sessionId'], oldSession, reason: 'Established-session failures retain their binding.');
    expect(stage(3).containsKey('sessionId'), isFalse, reason: 'A new failed preparation never acquired the old session.');
    expect(stage(3)['desktopId'], 'pve-170');
    expect(stage(3)['mode'], 'stream');
    expect(stage(3)['code'], 'display_control_unavailable');
    expect(stage(4)['sessionId'], oldSession, reason: 'Omitting one row does not erase an established binding.');
    expect(stage(5)['sessionId'], newSession);
    expect(rows.every((row) => !row.containsKey('inheritSession')), isTrue);
  });

  test('late acknowledgement after reporter close cannot enter its successor', () async {
    const oldSession = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa';
    const newSession = 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb';
    final log = ProductLog(), oldSent = <Map<String, Object?>>[], newSent = <Map<String, Object?>>[];
    final entered = Completer<void>(), acknowledgement = Completer<DiagnosticDelivery>();
    final old = ClientDiagnostics(
      log: log,
      desktopId: 'pve-170',
      mode: 'stream',
      runId: 'a' * 32,
      interval: const Duration(hours: 1),
      send: (report) async {
        oldSent.add(jsonDecode(jsonEncode(report)) as Map<String, Object?>);
        entered.complete();
        return acknowledgement.future;
      },
    );
    ClientDiagnostics? successor;
    addTearDown(() {
      if (!acknowledgement.isCompleted) acknowledgement.complete(DiagnosticDelivery.accepted);
      old.close();
      successor?.close();
      log.dispose();
    });
    old.bindSession(oldSession);
    log.recordFailure(LogEvent.streamEnd, 'capture_failed', {'stage': 1, 'ok': false});
    final sending = old.flush();
    await entered.future;
    log.recordFailure(LogEvent.streamEnd, 'capture_failed', {'stage': 2, 'ok': false});
    old.close();
    final current = ClientDiagnostics(
      log: log,
      desktopId: 'pve-170',
      mode: 'stream',
      runId: 'b' * 32,
      interval: const Duration(hours: 1),
      send: (report) async {
        newSent.add(jsonDecode(jsonEncode(report)) as Map<String, Object?>);
        return DiagnosticDelivery.accepted;
      },
    );
    successor = current;
    current.bindSession(newSession);
    acknowledgement.complete(DiagnosticDelivery.accepted);
    await sending;
    log.recordPreparationFailure(LogEvent.streamEnd, 'display_control_unavailable', {'stage': 3, 'ok': false});
    log.record(LogEvent.streamReady, {'stage': 4, 'ok': true});
    await current.flush();
    await old.flush();
    expect(oldSent, hasLength(1));
    expect(old.pending, isFalse);
    expect(newSent, hasLength(1));
    final rows = (newSent.single['events'] as List).cast<Map>();
    expect(rows.any((row) => (row['values'] as Map)['stage'] == 2), isFalse);
    expect(jsonEncode(newSent), isNot(contains(oldSession)));
    expect(rows.singleWhere((row) => row['event'] == 'streamReady')['sessionId'], newSession);
    expect(rows.singleWhere((row) => (row['values'] as Map)['stage'] == 3).containsKey('sessionId'), isFalse);
    current.close();
    log.record(LogEvent.streamReady, {'stage': 5, 'ok': true});
    await current.flush();
    expect(newSent, hasLength(1), reason: 'Closed subscriptions and late callbacks cannot restart uploads.');
  });

  test(
    'local native print stages never enter an unsupported broker batch',
    () async {
      final log = ProductLog();
      final reports = <Map<String, Object?>>[];
      final reporter = ClientDiagnostics(
        log: log,
        send: (report) async {
          reports.add(report);
          return DiagnosticDelivery.accepted;
        },
      );
      addTearDown(reporter.close);
      log.record(LogEvent.print, {'stage': 3, 'ok': true});
      log.record(LogEvent.streamReady, {'ok': true});
      await reporter.flush();
      final rows = reports.single['events'] as List;
      expect(rows.map((e) => (e as Map)['event']), [
        'appStarted',
        'streamReady',
      ]);
    },
  );
  for (final suppliedVersion in ['0.1.0+26', null]) {
    test('HTTP diagnostics use platform version $suppliedVersion without '
        'Windows fallback', () async {
      final previousBuild = runningClientBuild;
      runningClientBuild = ClientBuildVersion.parse('0.4.101+115');
      addTearDown(() => runningClientBuild = previousBuild);
      final client = DiagnosticClient();
      final api = HttpBrokerApi(
        origin: Uri.parse('https://configured.invalid'),
        client: client,
        enableDiagnostics: true,
        diagnosticsVersion: () => suppliedVersion,
      );
      addTearDown(api.close);
      await api.desktops('token');
      await api.flushDiagnostics();
      expect(client.reports, isNotEmpty);
      expect(
        client.reports.every((report) => report['version'] == suppliedVersion),
        isTrue,
      );
    });
  }
  test(
    'HTTP diagnostics retain the default running Windows EXE version',
    () async {
      final previousBuild = runningClientBuild;
      runningClientBuild = ClientBuildVersion.parse('0.4.101+115');
      addTearDown(() => runningClientBuild = previousBuild);
      final client = DiagnosticClient();
      final api = HttpBrokerApi(
        origin: Uri.parse('https://configured.invalid'),
        client: client,
        enableDiagnostics: true,
      );
      addTearDown(api.close);
      await api.desktops('token');
      await api.flushDiagnostics();
      expect(client.reports, isNotEmpty);
      expect(
        client.reports.every((report) => report['version'] == '0.4.101+115'),
        isTrue,
      );
    },
  );
  test('A rejected mixed batch isolates the unsupported row and preserves later evidence', () async {
    final reports = <String>[], accepted = <String>[];
    final reporter = ClientDiagnostics(
      interval: const Duration(hours: 1),
      send: (report) async {
        reports.add(jsonEncode(report));
        final rows = report['events'] as List;
        if (rows.any((dynamic row) => row['event'] == 'usb')) {
          return DiagnosticDelivery.rejected;
        }
        accepted.addAll(rows.map((dynamic row) => row['event'] as String));
        return DiagnosticDelivery.accepted;
      },
    );
    addTearDown(reporter.close);
    reporter.record('usb');
    reporter.record('clipboard', code: 'clipboard_write_failed');
    while (reporter.pending) {
      await reporter.flush();
    }
    expect(accepted, ['appStarted', 'clipboard']);
    expect(reporter.rejectedBatches, 2);
    expect(reporter.discardedRows, 1);
    reporter.record('clipboard', values: {'ok': true});
    await reporter.flush();
    expect(accepted.last, 'clipboard');
    expect(reports.map((r) => jsonDecode(r)['sequence']), [1, 2, 3, 4, 5]);
  });

  test(
    'A lost acknowledgement of an isolated row retries the exact ID and bytes',
    () async {
      final reports = <String>[];
      final reporter = ClientDiagnostics(
        interval: const Duration(hours: 1),
        send: (report) async {
          reports.add(jsonEncode(report));
          return reports.length == 1
              ? DiagnosticDelivery.rejected
              : reports.length == 2
              ? DiagnosticDelivery.retry
              : DiagnosticDelivery.accepted;
        },
      );
      addTearDown(reporter.close);
      reporter.record('clipboard');
      await reporter.flush();
      await reporter.flush();
      await reporter.flush();
      expect(reports[2], reports[1]);
      while (reporter.pending) {
        await reporter.flush();
      }
    },
  );

  test(
    'Dense statistics batches stay within the broker encoded byte limit',
    () async {
      final reports = <String>[];
      final reporter = ClientDiagnostics(
        interval: const Duration(hours: 1),
        send: (report) async {
          reports.add(jsonEncode(report));
          return DiagnosticDelivery.accepted;
        },
      );
      addTearDown(reporter.close);
      for (var i = 0; i < 32; i++) {
        reporter.record(
          'streamStats',
          values: {for (final field in streamStatsFields) field: 0xffffffff},
        );
      }
      while (reporter.pending) {
        await reporter.flush();
      }
      expect(
        reports.every(
          (r) => utf8.encode(r).length <= ClientDiagnostics.maxBatchBytes,
        ),
        isTrue,
      );
      expect(
        reports.fold<int>(
          0,
          (sum, r) => sum + (jsonDecode(r)['events'] as List).length,
        ),
        33,
      );
      expect(reporter.discardedRows, 0);
    },
  );

  for (final status in [400, 413]) {
    test(
      'Diagnostic HTTP $status schema rejection keeps future valid reports alive',
      () async {
        final client = DiagnosticClient()
          ..uploadStatus = status
          ..uploadCode = 'invalid_client_diagnostics';
        final api = HttpBrokerApi(
          origin: Uri.parse('https://configured.invalid'),
          client: client,
          enableDiagnostics: true,
        );
        addTearDown(api.close);
        await api.desktops('token');
        await api.flushDiagnostics();
        final before = client.reports.length;
        client.uploadStatus = 202;
        ProductLog.instance.record(LogEvent.clipboard, {'ok': true});
        for (var i = 0; i < 20; i++) {
          await api.flushDiagnostics();
        }
        expect(client.reports.length, greaterThan(before));
        expect(
          (client.reports.last['events'] as List).any(
            (dynamic e) => e['event'] == 'clipboard',
          ),
          isTrue,
        );
      },
    );
  }

  test(
    'USB records upload with session scope and bounded request details only',
    () async {
      final log = ProductLog(), sent = <Map<String, Object?>>[];
      final reporter = ClientDiagnostics(
        log: log,
        desktopId: 'pve-170',
        mode: 'stream',
        interval: const Duration(hours: 1),
        send: (report) async {
          sent.add(report);
          return DiagnosticDelivery.accepted;
        },
      );
      addTearDown(() {
        reporter.close();
        log.dispose();
      });
      reporter.bindSession(taskId);
      log.recordFailure(LogEvent.usb, 'usb_request_stuck', {
        'stage': 11,
        'ok': false,
        'usbReq': 0xa1fe,
        'usbStalls': 2,
        'usbFailed': 1,
        'usbTimeouts': 1,
        'usbSlow': 1,
        'usbBack': 2,
        'usbUsbd': 0x80000800,
        'name': 'PRIVATE_USB_NAME',
        'serial': 'PRIVATE_USB_SERIAL',
        'data': 'PRIVATE_USB_DATA',
        'vendor': 0x058f,
      });
      await reporter.flush();
      final rows = sent.single['events'] as List;
      final usb =
          rows.singleWhere((dynamic row) => row['event'] == 'usb') as Map;
      expect(usb['code'], 'usb_request_stuck');
      expect(usb['sessionId'], taskId);
      expect(usb['desktopId'], 'pve-170');
      expect(usb['values'], {
        'stage': 11,
        'ok': false,
        'usbReq': 0xa1fe,
        'usbStalls': 2,
        'usbFailed': 1,
        'usbTimeouts': 1,
        'usbSlow': 1,
        'usbBack': 2,
        'usbUsbd': 0x80000800,
      });
      expect(jsonEncode(sent), isNot(contains('PRIVATE_USB')));
    },
  );

  test('Reporter exports actual Dart bytes, scoped failures and only sanitized local records', () async {
    final log = ProductLog(), sent = <Map<String, Object?>>[];
    final reporter = ClientDiagnostics(
      log: log,
      desktopId: 'pve-149',
      mode: 'stream',
      interval: const Duration(hours: 1),
      runId: 'a' * 32,
      version: () => '0.4.38+52',
      send: (report) async {
        sent.add(jsonDecode(jsonEncode(report)) as Map<String, Object?>);
        return DiagnosticDelivery.accepted;
      },
    );
    addTearDown(() {
      reporter.close();
      log.dispose();
    });
    reporter.bindSession(taskId);
    log.recordFailure(LogEvent.streamEnd, 'capture_failed', {
      'ok': false,
      'width': 1920,
      'token': 'PRIVATE_SENTINEL',
      'error': 'PRIVATE_SENTINEL',
    });
    log.recordFailure(LogEvent.streamEnd, 'PRIVATE_SENTINEL');
    log.acceptForwarded({
      'time': DateTime.now().toUtc().toIso8601String(),
      'event': 'streamEnd',
      'values': {'error': 999},
    });
    await reporter.flush();
    final encoded = jsonEncode(sent.single);
    expect(encoded, isNot(contains('PRIVATE_SENTINEL')));
    final rows = sent.single['events'] as List;
    expect(rows.length, 3);
    expect(rows[1]['code'], 'capture_failed');
    expect(rows[1]['sessionId'], taskId);
    expect(rows[1]['desktopId'], 'pve-149');
    expect(rows[2]['code'], 'unknown_failure');
  });
  test('A connect refused while the Host restarts keeps the relay code', () async {
    // During a Host update: connects answered 409 host_not_registered /
    // stream_unavailable must not be sent as unknown_failure.
    final sent = <Map<String, Object?>>[];
    final reporter = ClientDiagnostics(
      log: ProductLog(),
      desktopId: 'pve-170',
      interval: const Duration(hours: 1),
      runId: 'b' * 32,
      version: () => '0.4.165+179',
      send: (report) async {
        sent.add(jsonDecode(jsonEncode(report)) as Map<String, Object?>);
        return DiagnosticDelivery.accepted;
      },
    );
    addTearDown(reporter.close);
    const refusals = [
      'host_not_registered',
      'stream_unavailable',
      'desktop_unavailable',
      'desktop_changed',
      'host_machine_mismatch',
      'monitor_unavailable',
      'vm_audio_busy',
      'proxmox_not_configured',
      'proxmox_settings_changed',
      'host_diagnostics_update_required',
    ];
    for (final code in refusals) {
      reporter.record(
        'brokerRequest',
        code: code,
        values: {'ok': false, 'stage': 3, 'error': 409},
      );
    }
    await reporter.flush();
    final rows = (sent.single['events'] as List).skip(1);
    expect([for (final row in rows) row['code']], refusals);
  });
  test('Lost acknowledgement retries identical batch and serializes concurrent flushes', () async {
    final log = ProductLog(),
        sent = <String>[],
        gate = Completer<DiagnosticDelivery>();
    final reporter = ClientDiagnostics(
      log: log,
      interval: const Duration(hours: 1),
      send: (report) async {
        sent.add(jsonEncode(report));
        return sent.length == 1 ? gate.future : DiagnosticDelivery.accepted;
      },
    );
    addTearDown(() {
      reporter.close();
      log.dispose();
    });
    final first = reporter.flush(), concurrent = reporter.flush();
    log.record(LogEvent.viewportRequest, {'width': 800, 'height': 600});
    expect(sent.length, 1);
    gate.complete(DiagnosticDelivery.retry);
    await Future.wait([first, concurrent]);
    await reporter.flush();
    expect(sent[1], sent[0]);
    await reporter.flush();
    expect(jsonDecode(sent.last)['sequence'], 2);
    expect(reporter.pending, isFalse);
  });
  test('Queue, batch and unavailable-server work remain bounded; closing discards the old session', () async {
    final log = ProductLog(), reports = <Map<String, Object?>>[];
    final reporter = ClientDiagnostics(
      log: log,
      interval: const Duration(hours: 1),
      send: (report) async {
        reports.add(report);
        return DiagnosticDelivery.accepted;
      },
    );
    for (var i = 0; i < 1000; i++) {
      log.record(LogEvent.viewportRequest, {'width': i});
    }
    while (reporter.pending) {
      await reporter.flush();
    }
    expect(reports.length, 8);
    expect(reports.every((r) => (r['events'] as List).length == 16), isTrue);
    reporter.close();
    log.record(LogEvent.streamEnd);
    await reporter.flush();
    expect(reports.length, 8);
    var attempts = 0;
    final unavailable = ClientDiagnostics(
      log: log,
      interval: const Duration(hours: 1),
      version: () => null,
      send: (report) async {
        attempts++;
        expect(report['version'], isNull);
        return DiagnosticDelivery.unavailable;
      },
    );
    await unavailable.flush();
    log.record(LogEvent.streamStart);
    await unavailable.flush();
    expect(attempts, 1);
    unavailable.close();
    log.dispose();
  });
  testWidgets('New local records upload automatically without a send button', (
    tester,
  ) async {
    final log = ProductLog();
    var count = 0;
    final reporter = ClientDiagnostics(
      log: log,
      send: (report) async {
        count++;
        return DiagnosticDelivery.accepted;
      },
    );
    log.record(LogEvent.streamReady, {'ok': true});
    await tester.pump(const Duration(seconds: 5));
    await tester.pump();
    expect(count, 1);
    reporter.close();
    log.dispose();
  });
  test('Production API retains failed host operation, HTTP request ID, origin, token and actual pending queue', () async {
    final client = DiagnosticClient();
    final api = HttpBrokerApi(
      origin: Uri.parse('https://configured.invalid'),
      client: client,
      enableDiagnostics: true,
      diagnosticsDesktopId: 'pve-149',
      diagnosticsMode: 'stream',
    );
    addTearDown(api.close);
    final result = await api.configureDisplays('in-memory-token', 'pve-149', 1);
    expect(result.operation!.error, 'display_configuration_failed');
    await api.flushDiagnostics();
    final events = client.reports.single['events'] as List;
    final event = events.last;
    expect(event['operationId'], taskId);
    expect(event['requestId'], taskId);
    expect(event['code'], 'display_configuration_failed');
    expect(event['values']['monitors'], 2);
    expect(jsonEncode(client.reports), isNot(contains('in-memory-token')));
    for (final request in client.requests) {
      expect(request.uri.origin, 'https://configured.invalid');
      expect(
        request.headers.values[HttpHeaders.authorizationHeader],
        'Bearer in-memory-token',
      );
      expect(request.followRedirects, isFalse);
    }
    await api.logout('in-memory-token');
    final before = client.requests.length;
    ProductLog.instance.record(LogEvent.streamEnd);
    await api.flushDiagnostics();
    expect(client.requests.length, before);
  });
  test('Stream statistics wait for the broker to acknowledge stream-stats-v1, then upload', () async {
    final client = DiagnosticClient();
    final api = HttpBrokerApi(
      origin: Uri.parse('https://configured.invalid'),
      client: client,
      enableDiagnostics: true,
      diagnosticsDesktopId: 'pve-149',
      diagnosticsMode: 'stream',
    );
    addTearDown(api.close);
    bool sentStats() => client.reports.any(
      (r) => (r['events'] as List).any((e) => e['event'] == 'streamStats'),
    );
    await api.desktops('token');
    await api.flushDiagnostics();
    api.recordStreamStats(
      {'decodeFps10': 600},
      sessionId: taskId,
      desktopId: 'pve-149',
    );
    await api.flushDiagnostics();
    expect(sentStats(), isFalse, reason: 'released broker acknowledgement');
    client.ackStreamStats = true;
    ProductLog.instance.record(LogEvent.streamReady, {'ok': true});
    await api.flushDiagnostics();
    api.recordStreamStats(
      {'decodeFps10': 600, 'auto': true},
      sessionId: taskId,
      desktopId: 'pve-149',
    );
    await api.flushDiagnostics();
    expect(sentStats(), isTrue);
    final row = (client.reports.last['events'] as List).last;
    expect(row['event'], 'streamStats');
    expect(row['values'], {'decodeFps10': 600, 'auto': true});
    expect(row['sessionId'], taskId);
    expect(row['mode'], 'stream');
  });
  test(
    'Windows produced before the first acknowledgement are held, then sent',
    () async {
      final client = DiagnosticClient()..ackStreamStats = true;
      final api = HttpBrokerApi(
        origin: Uri.parse('https://configured.invalid'),
        client: client,
        enableDiagnostics: true,
        diagnosticsDesktopId: 'pve-149',
        diagnosticsMode: 'stream',
      );
      addTearDown(api.close);
      await api.desktops('token');
      // Before any acknowledgement: held, not dropped.
      for (var i = 0; i < 6; i++) {
        api.recordStreamStats(
          {'decodeFps10': 600 + i},
          sessionId: taskId,
          desktopId: 'pve-149',
        );
      }
      await api.flushDiagnostics();
      ProductLog.instance.record(LogEvent.streamReady, {'ok': true});
      await api.flushDiagnostics();
      final stats = [
        for (final r in client.reports)
          for (final e in r['events'] as List)
            if (e['event'] == 'streamStats') e['values']['decodeFps10'],
      ];
      expect(stats, [600, 601, 602, 603], reason: 'bounded to 4 held windows');
    },
  );
  test('Old-server rejection disables uploads and a new login gets a fresh isolated reporter', () async {
    final client = DiagnosticClient()..uploadStatus = 404;
    final api = HttpBrokerApi(
      origin: Uri.parse('https://configured.invalid'),
      client: client,
      enableDiagnostics: true,
    );
    addTearDown(api.close);
    await api.desktops('first-token');
    await api.flushDiagnostics();
    expect(client.reports.length, 1);
    await api.desktops('first-token');
    await api.flushDiagnostics();
    expect(client.reports.length, 1);
    client.uploadStatus = 202;
    await api.desktops('new-token');
    await api.flushDiagnostics();
    expect(client.reports.length, 2);
    expect(client.reports.last['runId'], isNot(client.reports.first['runId']));
  });
}
