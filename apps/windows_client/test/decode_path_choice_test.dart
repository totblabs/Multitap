import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/decode_path_choice.dart';
import 'package:multitap_client/data/decode_settings.dart';

DecodePathRecord _record(
  double seconds,
  double delayMs,
  double frameMs, {
  bool busy = false,
}) {
  final record = DecodePathRecord();
  // One reading a second, as the session takes them.
  for (var i = 0; i < seconds; i++) {
    record.add(1, delayMs, frameMs, busy);
  }
  return record;
}

void main() {
  group('decode-path-v1 rule', () {
    // Field 2026-11-25. Integrated graphics, a game at 129 fps: a picture was
    // ready 13 ms after its packet on the hardware path, 4.8 ms on the CPU path.
    // A PC with a graphics card at 96 fps: 2.5 ms and 10 ms (decoder busy).
    const game = 1000 / 129, card = 1000 / 96;

    test('nothing measured: the hardware path, as before', () {
      expect(
        DecodePathRule.decide(DecodePathRecord(), DecodePathRecord()),
        DecodePathReason.hardwareUnmeasured,
      );
      expect(
        DecodePathRule.decide(_record(59, 13, game), _record(600, 4.8, game)),
        DecodePathReason.hardwareUnmeasured,
        reason: 'under a minute of fast stream decides nothing',
      );
    });

    test('a hardware path that keeps up is kept, whatever the CPU would do', () {
      expect(
        DecodePathRule.decide(_record(120, 2.5, card), DecodePathRecord()),
        DecodePathReason.hardwareKeepsUp,
      );
      expect(
        DecodePathRule.decide(_record(120, 2.5, card), _record(120, 1, card)),
        DecodePathReason.hardwareKeepsUp,
      );
      expect(DecodePathReason.hardwareKeepsUp.hardware, isTrue);
    });

    test('a hardware path that falls behind has the CPU path measured', () {
      final hardware = _record(120, 13, game);
      expect(
        DecodePathRule.decide(hardware, DecodePathRecord()),
        DecodePathReason.cpuMeasuring,
      );
      expect(
        DecodePathRule.decide(hardware, _record(59, 4.8, game)),
        DecodePathReason.cpuMeasuring,
      );
      expect(DecodePathReason.cpuMeasuring.hardware, isFalse);
    });

    test('the CPU path is taken where it is clearly sooner and has room', () {
      final hardware = _record(120, 13, game);
      expect(
        DecodePathRule.decide(hardware, _record(120, 4.8, game)),
        DecodePathReason.cpuSooner,
      );
      expect(DecodePathReason.cpuSooner.hardware, isFalse);
    });

    test('a CPU path that is no better gives the hardware path back', () {
      final hardware = _record(120, 13, game);
      expect(
        DecodePathRule.decide(hardware, _record(120, 10, game)),
        DecodePathReason.hardwareStill,
        reason: 'not sooner by a quarter',
      );
      expect(
        DecodePathRule.decide(hardware, _record(120, 4.8, game, busy: true)),
        DecodePathReason.hardwareStill,
        reason: 'the decoder had no room',
      );
      final mostlyFree = _record(100, 4.8, game)..add(20, 4.8, game, true);
      expect(
        DecodePathRule.decide(hardware, mostlyFree),
        DecodePathReason.hardwareStill,
        reason: 'busy in a sixth of the time',
      );
      expect(DecodePathReason.hardwareStill.hardware, isTrue);
    });

    test('a slower stream that keeps up does not hide a fast one that does not', () {
      // Five minutes of 60 fps video within its picture time, then a game.
      final hardware = _record(300, 7, 1000 / 60);
      expect(
        DecodePathRule.decide(hardware, DecodePathRecord()),
        DecodePathReason.hardwareKeepsUp,
      );
      for (var i = 0; i < 100; i++) {
        hardware.add(1, 13, game, false);
      }
      expect(hardware.behindShare, closeTo(0.25, 0.001));
      expect(
        DecodePathRule.decide(hardware, DecodePathRecord()),
        DecodePathReason.cpuMeasuring,
      );
    });

    test('diagnostics codes are fixed', () {
      expect(
        [for (final reason in DecodePathReason.values) reason.code],
        [0, 1, 2, 3, 4, 5, 6],
      );
    });
  });

  group('decode-path-v1 record', () {
    test('sums by time; readings that are no measure are ignored', () {
      final record = DecodePathRecord()
        ..add(2, 6, 8, false)
        ..add(1, 12, 8, true);
      expect(record.seconds, 3);
      expect(record.delayMs, closeTo(8, 1e-9));
      expect(record.behindShare, closeTo(1 / 3, 1e-9));
      expect(record.busyShare, closeTo(1 / 3, 1e-9));
      for (final bad in const [0.0, -1.0, double.nan, double.infinity]) {
        record.add(bad, 6, 8, false);
      }
      record
        ..add(1, double.nan, 8, false)
        ..add(1, -1, 8, false)
        ..add(1, 6, 0, false);
      expect(record.seconds, 3);
    });

    test('older streams count half once the record is full', () {
      final record = _record(DecodePathRule.keepSeconds, 13, 8);
      expect(record.seconds, DecodePathRule.keepSeconds);
      record.add(1, 4, 8, false);
      expect(record.seconds, closeTo((DecodePathRule.keepSeconds + 1) / 2, 1e-6));
      expect(record.delayMs, closeTo(13, 0.01));
      // The same time again of a faster path moves the mean to between them.
      for (var i = 0; i < DecodePathRule.keepSeconds / 2 - 1; i++) {
        record.add(1, 4, 8, false);
      }
      expect(record.delayMs, closeTo(8.5, 0.05));
    });

    test('a stored record is read back; a malformed one is empty', () {
      final record = _record(90, 13, 8, busy: true);
      final back = DecodePathRecord.parse(
        jsonDecode(jsonEncode(record.toJson())),
      );
      expect(back.seconds, 90);
      expect(back.delayMs, closeTo(13, 1e-6));
      expect(back.behindShare, 1);
      expect(back.busyShare, 1);
      for (final bad in <Object?>[
        null,
        'x',
        <String, Object?>{},
        {'seconds': 90, 'delay': 1, 'behind': 1},
        {'seconds': '90', 'delay': 1, 'behind': 1, 'busy': 1},
        {'seconds': 90, 'delay': -1, 'behind': 1, 'busy': 1},
        {'seconds': 90, 'delay': 1, 'behind': 91, 'busy': 1},
        {'seconds': 90, 'delay': 1, 'behind': 1, 'busy': 91},
        {'seconds': double.infinity, 'delay': 1, 'behind': 1, 'busy': 1},
      ]) {
        expect(DecodePathRecord.parse(bad).seconds, 0, reason: '$bad');
      }
    });
  });

  group('decode-path-v1 memory of this PC', () {
    const amd = '1002:164e', nvidia = '10de:2484';
    late Directory dir;
    late DecodeSettings store;
    setUp(() async {
      dir = Directory.systemTemp.createTempSync('decode-path');
      store = DecodeSettings(File('${dir.path}/decoding.json'));
      await DecodeSettings.init(store, adapter: amd);
    });
    tearDown(() async {
      await DecodeSettings.remember();
      await DecodeSettings.init(
        DecodeSettings(File('${dir.path}/unused.json')),
      );
      DecodeSettings.hardwareDecode = true;
      dir.deleteSync(recursive: true);
    });

    void watch(bool hardwarePath, int seconds, double delayMs, {double fps = 129}) {
      for (var i = 0; i < seconds; i++) {
        DecodeSettings.observe(
          hardwarePath: hardwarePath,
          seconds: 1,
          delayMs: delayMs,
          frameMs: 1000 / fps,
          decoderBusy: false,
        );
      }
    }

    test('an adapter seen for the first time is noted in the file', () async {
      final paths = await store.loadPaths();
      expect(paths.keys, [amd]);
      expect(paths[amd]!.hardware.seconds, 0);
      expect(await store.load(), isTrue);
      await DecodeSettings.choose(false);
      await DecodeSettings.init(store, adapter: nvidia);
      expect((await store.loadPaths()).keys, [amd, nvidia]);
      expect(await store.load(), isFalse, reason: 'the switch stays');
      await DecodeSettings.choose(true);
    });

    test('adapter names are vendor:device in hex, nothing else', () {
      expect(DecodeSettings.adapterKey('1002:164e'), '1002:164e');
      for (final bad in <Object?>[
        null,
        1,
        '',
        '1002:164E',
        '1002-164e',
        '1002:164e ',
        'AMD Radeon(TM) Graphics',
        '1002:164e:1',
      ]) {
        expect(DecodeSettings.adapterKey(bad), isNull, reason: '$bad');
      }
    });

    test('the switch off is the CPU path whatever was measured', () async {
      watch(true, 120, 2.5);
      expect(DecodeSettings.reason, DecodePathReason.hardwareKeepsUp);
      await DecodeSettings.choose(false);
      expect(DecodeSettings.reason, DecodePathReason.cpuOnly);
      expect(DecodeSettings.useHardware, isFalse);
      await DecodeSettings.choose(true);
      expect(DecodeSettings.useHardware, isTrue);
    });

    test('an unknown adapter learns nothing and asks for the hardware path', () async {
      store.file.deleteSync();
      await DecodeSettings.init(store);
      watch(true, 120, 13);
      expect(DecodeSettings.reason, DecodePathReason.hardwareNoAdapter);
      expect(DecodeSettings.useHardware, isTrue);
      await DecodeSettings.remember();
      expect(store.file.existsSync(), isFalse);
    });

    test('one PC from its first game to the CPU path, across restarts', () async {
      expect(DecodeSettings.reason, DecodePathReason.hardwareUnmeasured);
      // The first game runs on the hardware path and falls behind.
      watch(true, 90, 13);
      expect(DecodeSettings.reason, DecodePathReason.cpuMeasuring,
          reason: 'what this process measured counts before it is written');
      await DecodeSettings.remember();
      // The next connection is made by another process.
      await DecodeSettings.init(store, adapter: amd);
      expect(DecodeSettings.useHardware, isFalse);
      watch(false, 90, 4.8);
      await DecodeSettings.remember();
      await DecodeSettings.init(store, adapter: amd);
      expect(DecodeSettings.reason, DecodePathReason.cpuSooner);
      // Another graphics adapter in the same PC starts over.
      await DecodeSettings.init(store, adapter: nvidia);
      expect(DecodeSettings.reason, DecodePathReason.hardwareUnmeasured);
      watch(true, 90, 2.5, fps: 96);
      await DecodeSettings.remember();
      await DecodeSettings.init(store, adapter: nvidia);
      expect(DecodeSettings.reason, DecodePathReason.hardwareKeepsUp);
      await DecodeSettings.init(store, adapter: amd);
      expect(DecodeSettings.reason, DecodePathReason.cpuSooner,
          reason: 'the first adapter\'s records are still there');
    });

    test('two windows add to the same records; the switch keeps them', () async {
      watch(true, 20, 13);
      // Another window wrote meanwhile.
      await store.file.writeAsString(
        jsonEncode({
          'hardwareDecode': true,
          'paths': {
            amd: {
              'hardware': _record(60, 13, 8).toJson(),
              'cpu': _record(70, 4.8, 8).toJson(),
            },
          },
        }),
      );
      expect(DecodeSettings.reason, DecodePathReason.hardwareUnmeasured,
          reason: 'not read again yet');
      await DecodeSettings.refresh();
      expect(DecodeSettings.reason, DecodePathReason.cpuSooner);
      await DecodeSettings.remember();
      final paths = await store.loadPaths();
      expect(paths[amd]!.hardware.seconds, 80);
      expect(paths[amd]!.cpu.seconds, 70);
      await DecodeSettings.choose(false);
      expect(await store.load(), isFalse);
      expect((await store.loadPaths())[amd]!.hardware.seconds, 80);
      await DecodeSettings.choose(true);
      await DecodeSettings.init(store, adapter: amd);
      expect(DecodeSettings.reason, DecodePathReason.cpuSooner);
    });

    test('readings are written every half minute of them, and at most eight adapters', () async {
      watch(true, 29, 13);
      await Future<void>.delayed(const Duration(milliseconds: 50));
      expect((await store.loadPaths())[amd]!.hardware.seconds, 0);
      watch(true, 1, 13);
      await DecodeSettings.remember();
      expect((await store.loadPaths())[amd]!.hardware.seconds, 30);
      for (var i = 0; i < 9; i++) {
        await DecodeSettings.init(store, adapter: '8086:000$i');
        watch(true, 1, 3);
        await DecodeSettings.remember();
      }
      final paths = await store.loadPaths();
      expect(paths.length, 8);
      expect(paths.containsKey(amd), isFalse, reason: 'the oldest goes');
      expect(paths.containsKey('8086:0008'), isTrue);
    });

    test('a damaged file is no records and the default switch', () async {
      await store.file.writeAsString('{not json');
      await DecodeSettings.init(store, adapter: amd);
      expect(DecodeSettings.hardwareDecode, isTrue);
      expect(DecodeSettings.reason, DecodePathReason.hardwareUnmeasured);
      await store.file.writeAsString(
        jsonEncode({
          'hardwareDecode': true,
          'paths': {
            'AMD': {'hardware': 1},
            amd: 'x',
            nvidia: {'hardware': {'seconds': -1}},
          },
        }),
      );
      final paths = await store.loadPaths();
      expect(paths.keys, [amd, nvidia]);
      expect(paths[amd]!.hardware.seconds, 0);
      expect(paths[nvidia]!.hardware.seconds, 0);
    });
  });
}
