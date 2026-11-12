import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/bulk_host_updates.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/bulk_host_update_dialog.dart';

import 'bulk_host_updates_test.dart';

bool _fontsLoaded = false;
Future<void> loadBulkFonts(WidgetTester tester) async {
  if (_fontsLoaded) return;
  await tester.runAsync(() async {
    Future<ByteData> bytes(String path) async =>
        ByteData.sublistView(await File(path).readAsBytes());
    final loader = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      loader.addFont(bytes('assets/fonts/Pretendard-$weight.otf'));
    }
    await loader.load();
    final artifacts = File(Platform.resolvedExecutable).parent.parent.parent;
    await (FontLoader('MaterialIcons')..addFont(
          bytes('${artifacts.path}/material_fonts/MaterialIcons-Regular.otf'),
        ))
        .load();
  });
  _fontsLoaded = true;
}

Future<void> openBulk(
  WidgetTester tester,
  FakeBulkApi api,
  ValueNotifier<List<DesktopMachine>> machines, {
  bool observer = true,
  Size size = const Size(960, 640),
  Brightness brightness = Brightness.dark,
  double scale = 1,
  bool administrator = false,
  BulkHostUpdateController Function(ProductRelease, List<DesktopMachine>)?
  controllerFactory,
}) async {
  await loadBulkFonts(tester);
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    RepaintBoundary(
      key: const Key('bulk-host-render'),
      child: MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildAppTheme(brightness: brightness),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context)
              .copyWith(textScaler: TextScaler.linear(scale)),
          child: child!,
        ),
        home: Builder(
          builder: (context) => Scaffold(
            body: Center(
              child: FilledButton(
                key: const Key('open-bulk'),
                onPressed: () => unawaited(
                  showDialog<void>(
                    context: context,
                    barrierDismissible: false,
                    builder: (_) => BulkHostUpdateDialog(
                      api: api,
                      token: 'in-memory-token',
                      server: 'https://fixture.invalid',
                      assignedMachines: machines.value,
                      liveMachines: observer ? machines : null,
                      loadRelease: () async => bulkRelease,
                      administrator: administrator,
                      controllerFactory:
                          controllerFactory ??
                          (release, assigned) => BulkHostUpdateController(
                            api: api,
                            token: 'in-memory-token',
                            release: release,
                            assignedMachines: assigned,
                            minimumRequestInterval: Duration.zero,
                          ),
                    ),
                  ),
                ),
                child: const Text('Host 일괄 업데이트'),
              ),
            ),
          ),
        ),
      ),
    ),
  );
  await tester.tap(find.byKey(const Key('open-bulk')));
  await tester.pumpAndSettle();
}

/// Selects every available row and presses the button. The note above it
/// says what it does, so no second question opens.
Future<void> confirmBulk(WidgetTester tester) async {
  await tester.ensureVisible(find.byKey(const Key('bulk-host-select-all')));
  await tester.tap(find.byKey(const Key('bulk-host-select-all')));
  await tester.pump();
  expect(find.byKey(const Key('bulk-host-disconnect-note')), findsOneWidget);
  await tester.ensureVisible(find.byKey(const Key('bulk-host-submit')));
  await tester.tap(find.byKey(const Key('bulk-host-submit')));
  await tester.pump(const Duration(milliseconds: 400));
  expect(find.byKey(const Key('bulk-host-confirm')), findsNothing);
}

Future<void> captureBulk(WidgetTester tester, String name) async {
  final directory = Platform.environment['MULTITAP_BULK_RENDER_DIR'];
  if (directory == null) return;
  final boundary = tester.renderObject<RenderRepaintBoundary>(
    find.byKey(const Key('bulk-host-render')),
  );
  await tester.runAsync(() async {
    final image = await boundary.toImage(pixelRatio: 1);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      final folder = Directory(directory);
      await folder.create(recursive: true);
      await File('${folder.path}/$name.png')
          .writeAsBytes(bytes!.buffer.asUint8List());
    } finally {
      image.dispose();
    }
  });
}

void main() {
  testWidgets(
    'Public bulk entry point uses the existing assigned-VM API without requiring an admin role',
    (tester) async {
      await loadBulkFonts(tester);
      final api = FakeBulkApi();
      final machines = ValueNotifier([bulkMachine('pve-149')]);
      addTearDown(machines.dispose);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                key: const Key('public-bulk'),
                onPressed: () => unawaited(
                  showBulkHostUpdateDialog(
                    context,
                    api: api,
                    token: 'in-memory-token',
                    server: 'https://fixture.invalid',
                    assignedMachines: machines.value,
                    liveMachines: machines,
                    loadRelease: () async => bulkRelease,
                  ),
                ),
                child: const Text('업데이트'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.byKey(const Key('public-bulk')));
      await tester.pumpAndSettle();
      await confirmBulk(tester);
      expect(api.calls.single.id, 'pve-149');
      expect(api.calls.single.disconnect, isTrue);
      await tester.tap(find.byKey(const Key('bulk-host-close')));
      await tester.pumpAndSettle();
    },
  );

  testWidgets(
    'Any assigned account can select VMs; waiting, acknowledgement and actual terminal results stay distinct',
    (tester) async {
      final api = FakeBulkApi()
        ..handler = (_, index) async => bulkState(
          operation: bulkOperation(index == 0 ? 'waiting' : 'accepted'),
        );
      final machines = ValueNotifier([
        bulkMachine('pve-149'),
        bulkMachine('pve-170'),
      ]);
      addTearDown(machines.dispose);
      await openBulk(tester, api, machines);
      expect(api.calls, isEmpty);
      expect(find.textContaining('USB와 클립보드 안정성을 개선했어요.'), findsOneWidget);
      await confirmBulk(tester);
      expect(api.calls.length, 2);
      expect(api.calls.every((c) => c.disconnect), isTrue);
      expect(find.textContaining('연결이 끝나면 업데이트해요'), findsOneWidget);
      expect(find.textContaining('서버 접수됨'), findsOneWidget);
      expect(find.text('대기'), findsNWidgets(2));
      expect(find.text('진행 중 2대 · 완료 0대 · 실패 0대'), findsOneWidget);
      machines.value = [
        bulkMachine(
          'pve-149',
          state: bulkState(operation: bulkOperation('waiting')),
        ),
        bulkMachine(
          'pve-170',
          state: bulkState(
            connected: false,
            operation: bulkOperation('installing', tick: 2),
          ),
        ),
      ];
      await tester.pump();
      expect(find.text('다시 연결'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('bulk-host-progress-pve-170')),
        findsOneWidget,
      );
      machines.value = [
        bulkMachine(
          'pve-149',
          state: bulkState(
            version: bulkTarget,
            operation: bulkOperation('completed', reboot: true, tick: 3),
          ),
        ),
        bulkMachine(
          'pve-170',
          state: bulkState(
            version: bulkTarget,
            operation: bulkOperation(
              'completed',
              code: 'result_unreported',
              tick: 3,
            ),
          ),
        ),
      ];
      await tester.pump();
      expect(find.text('설치 완료 · Windows 다시 시작 필요'), findsOneWidget);
      expect(find.text('새 버전으로 실행 중 · 설치 결과는 보고되지 않았어요'), findsOneWidget);
      expect(find.text('완료'), findsOneWidget);
      expect(find.text('확인 필요'), findsOneWidget);
      expect(
        find.text('진행 상태 확인 필요 · 완료 1대 · 실패 0대 · 확인 필요 1대'),
        findsOneWidget,
      );
      expect(api.calls.length, 2);
      await tester.tap(find.byKey(const Key('bulk-host-close')));
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'The note above the button says connections end; local PCs are listed as such',
    (tester) async {
      for (final administrator in [false, true]) {
        final api = FakeBulkApi();
        final machines = ValueNotifier([
          bulkMachine('pve-149', state: bulkState(sessionActive: true)),
          bulkMachine('pc-gaming', name: 'TOWER-PC', kind: MachineKind.pc),
        ]);
        addTearDown(machines.dispose);
        await openBulk(tester, api, machines, administrator: administrator);
        final note = tester.widget<Text>(
          find.descendant(
            of: find.byKey(const Key('bulk-host-disconnect-note')),
            matching: find.byWidgetPredicate((w) => w is Text),
          ),
        );
        expect(
          note.data,
          administrator
              ? '연결된 데스크톱은 연결을 끊고 바로 업데이트해요.\nWindows는 다시 시작하지 않아요.'
              : '연결된 데스크톱은 내 연결을 끊고 바로 업데이트해요.\n'
                    '다른 사용자가 연결 중이면 그 연결이 끝난 뒤 업데이트해요.',
        );
        expect(find.text('사용 중 · 요청하면 연결을 끊어요'), findsOneWidget);
        expect(
          tester
              .widget<Text>(
                find.byKey(const ValueKey('bulk-host-version-pc-gaming')),
              )
              .data,
          '로컬 PC · 0.2.80 → $bulkTarget',
        );
        await confirmBulk(tester);
        expect(
          [for (final call in api.calls) (call.id, call.disconnect)],
          [('pve-149', true), ('pc-gaming', true)],
        );
        await tester.tap(find.byKey(const Key('bulk-host-close')));
        await tester.pumpAndSettle();
      }
    },
  );

  testWidgets(
    'One 모두 선택 box, a badge only for what is not a target, the note above the button (design audit F-25)',
    (tester) async {
      final api = FakeBulkApi();
      final machines = ValueNotifier([
        bulkMachine('pve-149'),
        bulkMachine('pve-170', state: bulkState(sessionActive: true)),
        bulkMachine('pve-247', state: HostUpdateState.absent),
      ]);
      addTearDown(machines.dispose);
      await openBulk(tester, api, machines);
      // One control instead of two amber text buttons.
      expect(find.text('모두 선택'), findsOneWidget);
      expect(find.text('가능한 항목 모두 선택'), findsNothing);
      expect(find.text('선택 해제'), findsNothing);
      Checkbox box() => tester.widget<Checkbox>(
        find.descendant(
          of: find.byKey(const Key('bulk-host-select-all')),
          matching: find.byType(Checkbox),
        ),
      );
      FilledButton submit() => tester.widget<FilledButton>(
        find.byKey(const Key('bulk-host-submit')),
      );
      expect(box().value, isFalse);
      expect(submit().onPressed, isNull);
      // One desktop chosen: a dash; a press then chooses them all.
      await tester.ensureVisible(
        find.byKey(const ValueKey('bulk-host-select-pve-149')),
      );
      await tester.tap(find.byKey(const ValueKey('bulk-host-select-pve-149')));
      await tester.pump();
      expect(box().value, isNull);
      await tester.ensureVisible(find.byKey(const Key('bulk-host-select-all')));
      await tester.tap(find.byKey(const Key('bulk-host-select-all')));
      await tester.pump();
      expect(box().value, isTrue);
      expect(find.text('선택한 2대 업데이트'), findsOneWidget);
      // A press on the full box clears the choice.
      await tester.tap(find.byKey(const Key('bulk-host-select-all')));
      await tester.pump();
      expect(box().value, isFalse);
      expect(submit().onPressed, isNull);

      // "업데이트 가능" is not said twice: the badge is for the other states.
      expect(
        find.byKey(const ValueKey('bulk-host-stage-pve-149')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey('bulk-host-stage-pve-247')),
        findsOneWidget,
      );
      // The state line is ordinary text; its colour is the dot's.
      final p = AppPalette.of(
        tester.element(find.byKey(const Key('bulk-host-disconnect-note'))),
      );
      expect(
        tester
            .widget<Text>(
              find.byKey(const ValueKey('bulk-host-status-pve-149')),
            )
            .style!
            .color,
        p.muted,
      );
      // The note is outside what scrolls, between the list and the buttons.
      final note = find.byKey(const Key('bulk-host-disconnect-note'));
      expect(
        find.ancestor(of: note, matching: find.byType(SingleChildScrollView)),
        findsNothing,
      );
      expect(
        tester.getRect(note).bottom,
        lessThanOrEqualTo(
          tester.getRect(find.byKey(const Key('bulk-host-submit'))).top,
        ),
      );
      expect(api.calls, isEmpty);
      expect(tester.takeException(), isNull);
      await tester.tap(find.byKey(const Key('bulk-host-close')));
      await tester.pumpAndSettle();
    },
  );

  testWidgets('No observation source means no executable bulk action', (
    tester,
  ) async {
    final api = FakeBulkApi();
    final machines = ValueNotifier([bulkMachine('pve-149')]);
    addTearDown(machines.dispose);
    await openBulk(tester, api, machines, observer: false);
    await tester.tap(find.byKey(const Key('bulk-host-select-all')));
    await tester.pump();
    expect(find.byKey(const Key('bulk-host-no-observer')), findsOneWidget);
    expect(
      tester
          .widget<FilledButton>(find.byKey(const Key('bulk-host-submit')))
          .onPressed,
      isNull,
    );
    expect(api.calls, isEmpty);
    await tester.tap(find.byKey(const Key('bulk-host-close')));
    await tester.pumpAndSettle();
  });

  testWidgets(
    'Stop preserves an in-flight accepted request and sends nothing for remaining rows',
    (tester) async {
      final gate = Completer<HostUpdateState>();
      final api = FakeBulkApi()..handler = (_, _) => gate.future;
      final machines = ValueNotifier([
        bulkMachine('pve-149'),
        bulkMachine('pve-170'),
      ]);
      addTearDown(machines.dispose);
      await openBulk(tester, api, machines);
      await confirmBulk(tester);
      await tester.tap(find.byKey(const Key('bulk-host-stop')));
      await tester.pump();
      expect(api.calls.length, 1);
      gate.complete(bulkState(operation: bulkOperation('waiting')));
      await tester.pumpAndSettle();
      expect(find.text('아직 접수되지 않은 남은 요청을 중지했어요'), findsOneWidget);
      expect(find.textContaining('연결이 끝나면 업데이트해요'), findsOneWidget);
      expect(api.calls.length, 1);
      await tester.tap(find.byKey(const Key('bulk-host-close')));
      await tester.pumpAndSettle();
    },
  );

  testWidgets(
    'Closing during a POST stops unsent rows without pretending accepted work was cancelled',
    (tester) async {
      final gate = Completer<HostUpdateState>();
      final api = FakeBulkApi()..handler = (_, _) => gate.future;
      final machines = ValueNotifier([
        bulkMachine('pve-149'),
        bulkMachine('pve-170'),
      ]);
      addTearDown(machines.dispose);
      await openBulk(tester, api, machines);
      await confirmBulk(tester);
      await tester.tap(find.byKey(const Key('bulk-host-close')));
      await tester.pumpAndSettle();
      gate.complete(bulkState(operation: bulkOperation('accepted')));
      await tester.pump();
      expect(api.calls.length, 1);
      expect(api.calls.single.disconnect, isTrue);
      expect(find.byKey(const Key('bulk-host-update-dialog')), findsNothing);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Assignment and latest-release changes after selection are rechecked with no hidden retargeting',
    (tester) async {
      final api = FakeBulkApi();
      final machines = ValueNotifier([bulkMachine('pve-149')]);
      addTearDown(machines.dispose);
      await openBulk(tester, api, machines);
      await tester.tap(find.byKey(const Key('bulk-host-select-all')));
      await tester.pump();
      machines.value = [
        bulkMachine('pve-149', state: bulkState(latest: '0.2.82')),
      ];
      await tester.pump();
      expect(
        tester
            .widget<FilledButton>(find.byKey(const Key('bulk-host-submit')))
            .onPressed,
        isNull,
      );
      await tester.ensureVisible(find.byKey(const Key('bulk-host-submit')));
      await tester.tap(find.byKey(const Key('bulk-host-submit')));
      await tester.pumpAndSettle();
      expect(api.calls, isEmpty);
      expect(
        find.byKey(const Key('bulk-host-catalog-changed')),
        findsOneWidget,
      );
      machines.value = [];
      await tester.pump();
      expect(find.text('VM 149'), findsNothing);
      await tester.tap(find.byKey(const Key('bulk-host-close')));
      await tester.pumpAndSettle();
    },
  );

  // A finished row does not read its version twice ("A → A"), a VM row does
  // not say "VM", the queue line does not come and go per desktop (moving the
  // rows), and "서버 접수는 설치 완료가 아니에요" does not stay beside
  // "모두 끝났어요".
  testWidgets(
    'A finished row reads its one version; the queue line stays through the run; the accepted-is-not-done line goes once all finished',
    (tester) async {
      final first = Completer<HostUpdateState>();
      final api = FakeBulkApi()
        ..handler = (_, index) => index == 0
            ? first.future
            : Future.value(bulkState(operation: bulkOperation('accepted')));
      final machines = ValueNotifier([
        bulkMachine('pve-149'),
        bulkMachine('pve-170'),
      ]);
      addTearDown(machines.dispose);
      await openBulk(tester, api, machines);
      String version(String id) => tester
          .widget<Text>(find.byKey(ValueKey('bulk-host-version-$id')))
          .data!;
      expect(version('pve-149'), '데스크톱 · 0.2.80 → $bulkTarget');
      expect(find.byKey(const Key('bulk-host-queue-wait')), findsNothing);
      await confirmBulk(tester);
      // The first request is on its way (not a wait between two requests)
      // and one desktop still waits its turn: the line is already there.
      expect(api.calls, hasLength(1));
      expect(find.byKey(const Key('bulk-host-queue-wait')), findsOneWidget);
      expect(find.text('다음 요청을 기다리는 중 · 남은 1대'), findsOneWidget);
      first.complete(bulkState(operation: bulkOperation('accepted')));
      await tester.pump();
      await tester.pump();
      expect(api.calls, hasLength(2));
      expect(find.byKey(const Key('bulk-host-queue-wait')), findsNothing);
      expect(find.textContaining('서버 접수는 설치 완료가 아니에요'), findsOneWidget);
      machines.value = [
        for (final id in ['pve-149', 'pve-170'])
          bulkMachine(
            id,
            state: bulkState(
              version: bulkTarget,
              newer: false,
              available: false,
              operation: bulkOperation('completed', tick: 3),
            ),
          ),
      ];
      await tester.pump();
      expect(version('pve-149'), '데스크톱 · $bulkTarget');
      expect(find.textContaining('→'), findsNothing);
      expect(find.textContaining('모두 끝났어요'), findsOneWidget);
      expect(find.textContaining('서버 접수는 설치 완료가 아니에요'), findsNothing);
      await tester.tap(find.byKey(const Key('bulk-host-close')));
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Two hundred assigned VMs keep the queue and stop action responsive while requests are paced',
    (tester) async {
      final gate = Completer<void>();
      final api = FakeBulkApi()
        ..handler = (_, _) async =>
            bulkState(operation: bulkOperation('waiting'));
      final machines = ValueNotifier([
        for (var i = 0; i < 200; i++) bulkMachine('pve-${1000 + i}'),
      ]);
      addTearDown(machines.dispose);
      await openBulk(
        tester,
        api,
        machines,
        controllerFactory: (release, assigned) => BulkHostUpdateController(
          api: api,
          token: 'in-memory-token',
          release: release,
          assignedMachines: assigned,
          pause: (_) => gate.future,
        ),
      );
      await confirmBulk(tester);
      expect(api.calls.length, 1);
      expect(find.byKey(const Key('bulk-host-queue-wait')), findsOneWidget);
      await tester.tap(find.byKey(const Key('bulk-host-stop')));
      await tester.pumpAndSettle();
      expect(api.calls.length, 1);
      expect(tester.takeException(), isNull);
      gate.complete();
      await tester.pump();
      await tester.tap(find.byKey(const Key('bulk-host-close')));
      await tester.pumpAndSettle();
    },
  );

  for (final size in [
    const Size(1920, 1080),
    const Size(1366, 768),
    const Size(960, 640),
  ]) {
    for (final brightness in Brightness.values) {
      for (final scale in [1.0, 1.5, 2.0]) {
        testWidgets(
          'Bulk layout ${size.width.toInt()}x${size.height.toInt()} $brightness text$scale',
          (tester) async {
            final api = FakeBulkApi();
            final machines = ValueNotifier([
              bulkMachine('pve-149', name: '작업용 테스트 VM 149 · 긴 이름 확인'),
              bulkMachine('pve-170', state: bulkState(sessionActive: true)),
              bulkMachine('pve-247', state: HostUpdateState.absent),
            ]);
            addTearDown(machines.dispose);
            await openBulk(
              tester,
              api,
              machines,
              size: size,
              brightness: brightness,
              scale: scale,
            );
            final submit = find.byKey(const Key('bulk-host-submit'));
            expect(submit, findsOneWidget);
            await tester.ensureVisible(
              find.byKey(const Key('bulk-host-select-all')),
            );
            await tester.tap(find.byKey(const Key('bulk-host-select-all')));
            await tester.pumpAndSettle();
            expect(tester.widget<FilledButton>(submit).onPressed, isNotNull);
            expect(tester.takeException(), isNull);
            await captureBulk(
              tester,
              'bulk-${size.width.toInt()}-${size.height.toInt()}-${brightness.name}-${(scale * 100).toInt()}',
            );
            await tester.tap(find.byKey(const Key('bulk-host-close')));
            await tester.pumpAndSettle();
            expect(
              api.calls,
              isEmpty,
              reason: 'layout inspection never applies an update',
            );
          },
        );
      }
    }
  }
}
