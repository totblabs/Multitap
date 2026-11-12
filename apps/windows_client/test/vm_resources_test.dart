import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/host_update.dart';
import 'package:multitap_client/models/virtual_display.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/vm_resource_bars.dart';

void main() {
  test('Missing/invalid resource metadata remains unknown and freshness uses local receipt time', () {
    for (final value in [
      null,
      [],
      'bad',
      {'source': 'unknown', 'cpuUsagePercent': 20},
    ]) {
      final sample = VmResources.fromJson(value);
      expect(sample.cpuUsagePercent, isNull);
      expect(sample.stale, isTrue);
    }
    final received = DateTime.utc(2026, 10, 24);
    final sample = VmResources.fromJson({
      'source': 'proxmox',
      'cpuUsagePercent': 2.6,
      'vcpus': 4,
      'memoryUsedBytes': 9 * 1024 * 1024 * 1024,
      'memoryTotalBytes': 8 * 1024 * 1024 * 1024,
      'checkedAt': '2020-01-01T00:00:00Z',
      'stale': false,
    }, receivedAt: received);
    expect(sample.stale, isFalse);
    expect(sample.expired(received.add(const Duration(seconds: 9))), isFalse);
    expect(sample.expired(received.add(const Duration(seconds: 10))), isTrue);
    expect(sample.asStale().memoryUsedBytes, sample.memoryUsedBytes);
    expect(sample.asStale().stale, isTrue);
    for (final invalid in [-1, '0', double.nan, double.infinity, null]) {
      final bad = VmResources.fromJson({
        'source': 'proxmox',
        'cpuUsagePercent': invalid,
        'vcpus': invalid,
        'memoryUsedBytes': invalid,
        'memoryTotalBytes': invalid,
        'checkedAt': 'invalid',
        'stale': false,
      });
      expect(bad.cpuUsagePercent, isNull);
      expect(bad.vcpus, isNull);
      expect(bad.memoryUsedBytes, isNull);
      expect(bad.memoryTotalBytes, isNull);
      expect(bad.stale, isTrue);
    }
    const machine = DesktopMachine(
      id: 'pve-170',
      name: 'VM',
      description: '',
      kind: MachineKind.vm,
      status: MachineStatus.ready,
      specification: 'Windows',
      accent: Colors.blue,
    );
    final measured = machine.withResources(sample);
    expect(
      measured.withDisplay(const VirtualDisplayState()).resources,
      same(sample),
    );
    expect(
      measured
          .withPower(
            state: 'running',
            checkedAt: null,
            stale: false,
            busy: false,
          )
          .resources,
      same(sample),
    );
  });

  test('Uptime is a bounded whole number, extrapolated from local receipt and dropped when stale', () {
    final received = DateTime.utc(2026, 10, 26, 9);
    VmResources parse(dynamic uptime) => VmResources.fromJson({
      'source': 'proxmox',
      'uptimeSeconds': uptime,
      'checkedAt': '2026-10-26T09:00:00Z',
      'stale': false,
    }, receivedAt: received);
    final sample = parse(11520);
    expect(sample.uptimeSeconds, 11520);
    expect(
      sample.uptimeAt(received.add(const Duration(seconds: 30))),
      const Duration(seconds: 11550),
    );
    // A clock behind the receipt never makes the VM younger.
    expect(
      sample.uptimeAt(received.subtract(const Duration(minutes: 5))),
      const Duration(seconds: 11520),
    );
    expect(sample.asStale().uptimeSeconds, 11520);
    expect(sample.asStale().uptimeAt(received), isNull);
    expect(parse(0).uptimeSeconds, 0);
    for (final invalid in [
      -1,
      1.5,
      '60',
      double.infinity,
      101 * 366 * 86400,
      null,
    ]) {
      expect(parse(invalid).uptimeSeconds, isNull, reason: '$invalid');
    }
    // A released broker never sends it.
    expect(
      VmResources.fromJson({
        'source': 'proxmox',
        'vcpus': 4,
        'checkedAt': '2026-10-26T09:00:00Z',
        'stale': false,
      }).uptimeAt(DateTime.now()),
      isNull,
    );
  });

  test('Tile wording: shares, sizes and uptime', () {
    expect(percentText(0), '0%');
    expect(percentText(.4), '<1%');
    expect(percentText(23.4), '23%');
    expect(percentText(99.6), '100%');
    expect(memorySizeText(8 * 1024 * 1024 * 1024), '8 GB');
    expect(memorySizeText(1536 * 1024 * 1024), '1.5 GB');
    expect(memorySizeText(512 * 1024 * 1024), '512 MB');
    expect(memoryUsedText(5583457484, total: 8589934592), '5.2 GB');
    expect(
      memoryUsedText(310 * 1024 * 1024, total: 512 * 1024 * 1024),
      '310 MB',
    );
    expect(uptimeText(const Duration(seconds: 40)), '방금 켜짐');
    expect(uptimeText(const Duration(minutes: 12)), '12분');
    expect(uptimeText(const Duration(hours: 3, minutes: 12)), '3시간');
    expect(uptimeText(const Duration(days: 4, hours: 6)), '4일');
    expect(uptimeLongText(const Duration(hours: 3, minutes: 12)), '3시간 12분');
    expect(uptimeLongText(const Duration(days: 4, hours: 6)), '4일 6시간');
    final p = AppPalette(true);
    expect(loadColor(p, .5, p.blue), p.blue);
    expect(loadColor(p, .8, p.blue), p.amber);
    expect(loadColor(p, .95, p.blue), p.red);
    expect(loadColor(p, null, p.purple), p.purple);
  });

  test('In use: the broker\'s own-account flag, else a Host update view\'s session flag', () {
    expect(desktopUseFromJson({'mine': true}, null), DesktopUse.mine);
    expect(desktopUseFromJson({'mine': false}, null), DesktopUse.other);
    expect(desktopUseAccountFromJson({'mine': false, 'account': 'alice'}), 'alice');
    expect(desktopUseAccountFromJson({'mine': true, 'account': 'alice'}), isNull);
    expect(desktopUseAccountFromJson({'mine': false}), isNull);
    expect(desktopUseAccountFromJson({'mine': false, 'account': 'a\u202Eb'}), isNull);
    expect(desktopUseAccountFromJson({'mine': false, 'account': 'x' * 65}), isNull);
    expect(desktopUseFromJson({}, null), DesktopUse.someone);
    expect(desktopUseFromJson(null, null), DesktopUse.idle);
    final view = {'protocol': 'host-update-v1', 'sessionActive': true};
    expect(desktopUseFromJson(null, view), DesktopUse.someone);
    expect(desktopUseFromJson({'mine': true}, view), DesktopUse.mine);
    expect(
      desktopUseFromJson(null, {'protocol': 'other', 'sessionActive': true}),
      DesktopUse.idle,
    );
    for (final use in DesktopUse.values) {
      expect(use.label == null, !use.active);
      expect(use.hint == null, !use.active);
    }
    final machine = const DesktopMachine(
      id: 'pve-170',
      name: 'VM',
      description: '',
      kind: MachineKind.vm,
      status: MachineStatus.ready,
      specification: 'Windows',
      accent: Colors.blue,
      use: DesktopUse.other,
      maxBitrate: 50000,
    );
    expect(machine.withResources(const VmResources()).use, DesktopUse.other);
    expect(
      machine.withDisplay(const VirtualDisplayState()).use,
      DesktopUse.other,
    );
    final updated = machine.withHostUpdate(HostUpdateState.absent);
    expect(updated.use, DesktopUse.other);
    // It used to drop the Host's bitrate ceiling.
    expect(updated.maxBitrate, 50000);
    expect(
      machine
          .withPower(
            state: 'running',
            checkedAt: null,
            stale: false,
            busy: false,
          )
          .use,
      DesktopUse.other,
    );
  });

  testWidgets(
    'Resource bars preserve over-allocation values, clamp drawing and label stale observations in both themes',
    (t) async {
      const fresh = VmResources(
        cpuUsagePercent: 100,
        vcpus: 4,
        memoryUsedBytes: 9 * 1024 * 1024 * 1024,
        memoryTotalBytes: 8 * 1024 * 1024 * 1024,
        stale: false,
      );
      for (final brightness in Brightness.values) {
        for (final sample in [fresh, fresh.asStale(), const VmResources()]) {
          await t.pumpWidget(
            MaterialApp(
              theme: buildAppTheme(brightness: brightness),
              home: Scaffold(
                body: Center(
                  child: SizedBox(
                    width: 250,
                    child: VmResourceBars(sample: sample),
                  ),
                ),
              ),
            ),
          );
          await t.pumpAndSettle();
          expect(t.takeException(), isNull);
          final bars = t
              .widgetList<LinearProgressIndicator>(
                find.byType(LinearProgressIndicator),
              )
              .toList();
          expect(bars.length, 2);
          expect(
            bars.every(
              (bar) => bar.value != null && bar.value! >= 0 && bar.value! <= 1,
            ),
            isTrue,
          );
          if (sample.memoryUsedBytes != null) {
            // The tile's units (GB as Windows shows GiB, 2026-10-26).
            expect(find.text('9.0 GB / 8 GB'), findsOneWidget);
            expect(bars.last.value, 1);
          }
          expect(
            find.text('이전 값'),
            sample.stale && sample.cpuUsagePercent != null
                ? findsOneWidget
                : findsNothing,
          );
        }
      }
    },
  );
}
