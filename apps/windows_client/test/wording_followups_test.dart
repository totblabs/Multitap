// Wording that needed code (2026-11-10 audit follow-ups): a text that is only
// true for one of the states it was shown in.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/virtual_display.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/themed_app.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/ui/vm_resource_bars.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

DesktopMachine _machine(MachineKind kind) => DesktopMachine(
  id: kind == MachineKind.pc ? 'pc-1' : 'pve-149',
  name: 'TESTER',
  description: '',
  kind: kind,
  status: MachineStatus.ready,
  specification: 'Windows 11',
  accent: Colors.blue,
  powerState: 'running',
  powerStale: false,
  powerCanForceStop: true,
  hostOnline: true,
  interactiveSession: true,
  virtualDisplayReady: true,
  virtualMonitorControl: true,
  display: const VirtualDisplayState(
    supported: true,
    activeCount: 1,
    stale: false,
  ),
  monitors: const [
    RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
  ],
);

VmResources _sample({String? measuredBy}) => VmResources.fromJson({
  'source': 'proxmox',
  'checkedAt': DateTime.now().toUtc().toIso8601String(),
  'cpuUsagePercent': 12.5,
  'vcpus': 4,
  'memoryUsedBytes': 3 * 1024 * 1024 * 1024,
  'memoryTotalBytes': 8 * 1024 * 1024 * 1024,
  'stale': false,
  'measuredBy': ?measuredBy,
});

Future<String> _barsTooltip(
  WidgetTester tester,
  VmResources sample, {
  bool pc = false,
}) async {
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: Scaffold(
        body: Center(
          child: SizedBox(
            width: 320,
            child: VmResourceBars(sample: sample, pc: pc),
          ),
        ),
      ),
    ),
  );
  return tester.widget<Tooltip>(find.byType(Tooltip)).message!;
}

Future<void> _workspace(
  WidgetTester tester, {
  required String error,
  required List<DesktopMachine> machines,
}) async {
  tester.view.physicalSize = const Size(1280, 800);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  final appearance = AppAppearance();
  addTearDown(appearance.dispose);
  await tester.pumpWidget(
    ThemedApp(
      appearance: appearance,
      home: WorkspaceShell(
        onSignOut: () {},
        onRefresh: () {},
        displayName: 'tester',
        serverOrigin: 'https://cloud.example.com',
        machines: machines,
        error: error,
        localMonitors: 1,
        onConnect: (_, {options = const StreamOptions()}) async =>
            throw StateError('this fixture does not connect'),
        onPower: (_, _) async {},
        onConfigureDisplays: (_, _) async => _machine(MachineKind.vm),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

void main() {
  group('who measured the CPU and memory', () {
    test('one wording for the dialog meters and the card strip', () {
      expect(resourceSourceText(_sample(), pc: false), 'Proxmox 기준');
      expect(
        resourceSourceText(_sample(measuredBy: 'host'), pc: false),
        '데스크톱 안에서 측정',
      );
      expect(resourceSourceText(_sample(), pc: true), '로컬 PC에서 측정');
      expect(
        resourceSourceText(_sample(measuredBy: 'host'), pc: true),
        '로컬 PC에서 측정',
      );
      expect(resourceNeedsMemoryNote(_sample(), pc: false), isTrue);
      expect(
        resourceNeedsMemoryNote(_sample(measuredBy: 'host'), pc: false),
        isFalse,
      );
      expect(resourceNeedsMemoryNote(_sample(), pc: true), isFalse);
    });

    testWidgets('a sample the Host measured is not called Proxmox', (
      tester,
    ) async {
      final host = await _barsTooltip(tester, _sample(measuredBy: 'host'));
      expect(host, startsWith('데스크톱 안에서 측정'));
      expect(host, isNot(contains('Proxmox')));
      expect(host, isNot(contains('Windows 작업 관리자')));
    });

    testWidgets('a sample Proxmox measured keeps its name and its note', (
      tester,
    ) async {
      final proxmox = await _barsTooltip(tester, _sample());
      expect(proxmox, startsWith('Proxmox 기준'));
      expect(proxmox, contains('Windows 작업 관리자'));
    });

    testWidgets('a local PC names itself', (tester) async {
      final pc = await _barsTooltip(tester, _sample(), pc: true);
      expect(pc, startsWith('로컬 PC에서 측정'));
      expect(pc, isNot(contains('Windows 작업 관리자')));
    });
  });

  group('the list could not be read', () {
    testWidgets('the empty page says what to do under its title', (
      tester,
    ) async {
      await _workspace(tester, error: desktopListFailureText, machines: []);
      final card = find.byKey(const Key('workspace-error-card'));
      expect(card, findsOneWidget);
      expect(
        find.descendant(of: card, matching: find.text('목록을 불러오지 못했어요')),
        findsOneWidget,
      );
      expect(
        tester.widget<Text>(find.byKey(const Key('workspace-error'))).data,
        '잠시 후 다시 시도하세요.',
      );
      // The title is not said a second time in other words.
      expect(find.textContaining('목록을 확인하지 못했어요'), findsNothing);
      expect(find.text(desktopListFailureText), findsNothing);
    });

    testWidgets('a reason the server gave is shown as it is', (tester) async {
      const reason = '서버가 응답하지 않아요. 잠시 후 다시 시도하세요.';
      await _workspace(tester, error: reason, machines: []);
      expect(
        tester.widget<Text>(find.byKey(const Key('workspace-error'))).data,
        reason,
      );
    });

    testWidgets('over an older list the banner says what happened', (
      tester,
    ) async {
      await _workspace(
        tester,
        error: desktopListFailureText,
        machines: [_machine(MachineKind.vm)],
      );
      expect(find.byKey(const Key('workspace-error-card')), findsNothing);
      expect(find.text('데스크톱 목록을 불러오지 못했어요.'), findsOneWidget);
      expect(find.text('잠시 후 다시 시도하세요.'), findsNothing);
    });
  });
}
