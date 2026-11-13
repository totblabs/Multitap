import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/ui/app_progress.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/usb_panel.dart';

import 'usb_redirection_test.dart'
    show started, shared, stick, FakeBackend, FakeSocket;
import 'helpers/pump_work.dart';

// The USB panel: what a person sees and can do. The session, relay and
// capture are the fakes of usb_redirection_test.dart.
Widget host(
  UsbSession usb, {
  Brightness brightness = Brightness.dark,
  double width = 420,
  double scale = 1,
  Future<bool> Function()? restartPc,
}) => MaterialApp(
  theme: buildAppTheme(brightness: brightness),
  home: MediaQuery(
    data: MediaQueryData(textScaler: TextScaler.linear(scale)),
    child: Scaffold(
      body: Center(
        child: SizedBox(
          width: width,
          // Never this PC's real restart: a test that reaches it fails.
          child: UsbPanel(
            usb: usb,
            restartPc:
                restartPc ?? () async => fail('restartPc was not expected'),
          ),
        ),
      ),
    ),
  ),
);

void main() {
  for (final brightness in Brightness.values) {
    testWidgets(
      'Pending check can be cancelled and return blocks duplicate capture in $brightness',
      (tester) async {
        late UsbSession usb;
        late FakeSocket socket;
        late FakeBackend backend;
        final prepared = Completer<void>();
        await tester.runAsync(() async {
          final result = await started();
          usb = result.$1;
          socket = result.$2;
          backend = result.$3;
          backend.prepareGate = prepared;
        });
        await tester.pumpWidget(
          host(usb, brightness: brightness, width: 300, scale: 1.5),
        );
        await tester.pump();
        await tester.ensureVisible(
          find.byKey(const ValueKey('usb-share-stick')),
        );
        await tester.tap(find.byKey(const ValueKey('usb-share-stick')));
        await tester.pump();
        expect(find.text('열린 파일이 없는지 확인하는 중…'), findsOneWidget);
        final cancel = find.byKey(const ValueKey('usb-stop-stick'));
        expect(tester.widget<OutlinedButton>(cancel).onPressed, isNotNull);
        expect(find.text('취소'), findsOneWidget);
        await tester.ensureVisible(cancel);
        await tester.tap(cancel);
        await tester.pump();
        expect(usb.entries.single.state, UsbEntryState.returning);
        await tester.runAsync(() async {
          prepared.complete();
          await Future<void>.delayed(const Duration(milliseconds: 20));
          expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
          backend.prepareGate = null;
          final (entry, capture) = await shared(usb, socket, backend);
          capture.releaseGate = Completer<void>();
          unawaited(usb.stop(entry));
        });
        await tester.pump();
        expect(find.text('PC로 장치를 돌려주는 중…'), findsOneWidget);
        expect(find.text('되돌리는 중'), findsOneWidget);
        expect(tester.widget<OutlinedButton>(cancel).onPressed, isNull);
        expect(find.byKey(const ValueKey('usb-share-stick')), findsNothing);
        expect(tester.takeException(), isNull);
        await tester.runAsync(() async {
          backend.captures.single.releaseGate!.complete();
          await Future<void>.delayed(const Duration(milliseconds: 20));
          await usb.close();
        });
      },
    );

    testWidgets(
      'No-drive and return-failure guidance stays actionable in $brightness',
      (tester) async {
        late UsbSession usb;
        late FakeSocket socket;
        late FakeBackend backend;
        await tester.runAsync(() async {
          final result = await started(devices: [stick(noReadableDrive: true)]);
          usb = result.$1;
          socket = result.$2;
          backend = result.$3;
          backend.returnedState = 2;
        });
        await tester.pumpWidget(
          host(usb, brightness: brightness, width: 300, scale: 1.5),
        );
        await tester.pump();
        expect(find.textContaining('PC에서 읽을 수 있는 드라이브가 없어요'), findsOneWidget);
        await tester.runAsync(() async {
          usb.share(usb.entries.single);
          final attach = await socket.text('usb.attach');
          socket.emit({'type': 'usb.attached', 'device': attach['device']});
          await Future<void>.delayed(const Duration(milliseconds: 30));
        });
        await tester.pump();
        expect(
          find.text('데스크톱에서 드라이브가 보이지 않으면 ‘디스크 관리’를 확인하세요.'),
          findsOneWidget,
        );
        await tester.runAsync(() async {
          socket.emit({
            'type': 'usb.detach',
            'device': 1,
            'reason': 'usb_device_error',
          });
          await Future<void>.delayed(const Duration(milliseconds: 30));
        });
        await tester.pump();
        expect(find.text('PC로 되돌리기'), findsOneWidget);
        expect(find.textContaining('장치가 PC로 돌아오지 않았어요'), findsOneWidget);
        expect(
          tester.takeException(),
          isNull,
          reason: 'narrow panel with 150% text',
        );
        await tester.ensureVisible(
          find.byKey(const ValueKey('usb-checkreturn-stick')),
        );
        await tester.pump();
        // The button gets the device back without unplugging it: UsbDk takes it
        // once more and gives it straight back, then the return is checked.
        backend.returnedState = 1;
        final captures = backend.captureCalls;
        await tester.tap(find.byKey(const ValueKey('usb-checkreturn-stick')));
        await tester.pump();
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 30)),
        );
        await tester.pump();
        expect(backend.captureCalls, captures + 1);
        expect(backend.captures.last.released, isTrue);
        expect(
          find.byKey(const ValueKey('usb-notreturned-stick')),
          findsNothing,
        );
        expect(usb.entries.single.returnUncertain, isFalse);
        expect(find.text('다시 시도'), findsOneWidget);
        await tester.runAsync(usb.close);
      },
    );
  }

  testWidgets(
    'A device waits for the VM, connects on request and says it left this PC',
    (tester) async {
      late UsbSession usb;
      late FakeSocket socket;
      await tester.runAsync(() async {
        final result = await started(hostReady: false);
        usb = result.$1;
        socket = result.$2;
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      expect(find.text('PC USB'), findsOneWidget);
      expect(find.text('SanDisk Cruzer'), findsOneWidget);
      expect(find.byKey(const Key('usb-status')), findsOneWidget);
      expect(find.textContaining('준비를 하고 있어요'), findsOneWidget);
      var share = tester.widget<FilledButton>(
        find.byKey(const ValueKey('usb-share-stick')),
      );
      expect(share.onPressed, isNull, reason: 'not while the VM prepares');

      await tester.runAsync(() async {
        socket.emit({'type': 'usb.status', 'state': 'ready'});
        await Future<void>.delayed(const Duration(milliseconds: 30));
      });
      await tester.pump();
      expect(find.byKey(const Key('usb-status')), findsNothing);
      share = tester.widget<FilledButton>(
        find.byKey(const ValueKey('usb-share-stick')),
      );
      expect(share.onPressed, isNotNull);
      await tester.tap(find.byKey(const ValueKey('usb-share-stick')));
      await tester.pump();
      expect(find.text('데스크톱에 연결하는 중…'), findsOneWidget);
      expect(
        socket.texts.where((m) => m['type'] == 'usb.attach'),
        hasLength(1),
      );

      await tester.runAsync(() async {
        socket.emit({'type': 'usb.attached', 'device': 1});
        await Future<void>.delayed(const Duration(milliseconds: 30));
      });
      await tester.pump();
      expect(find.textContaining('PC에서 쓸 수 없어요'), findsOneWidget);
      expect(find.text('연결 끊기'), findsOneWidget);
      expect(
        find.textContaining('하드웨어 안전하게 제거'),
        findsOneWidget,
        reason: 'a storage device warns before it is cut off',
      );

      // 연결 끊기 asks first; 취소 keeps the device with the VM.
      await tester.tap(find.byKey(const ValueKey('usb-stop-stick')));
      await tester.pumpAndSettle();
      expect(find.text('USB 연결 끊기'), findsOneWidget);
      expect(find.textContaining('저장하지 않은 내용이 사라질 수 있어요'), findsOneWidget);
      expect(find.textContaining('끊으면 장치가 PC로 돌아와요'), findsOneWidget);
      await tester.tap(find.byKey(const Key('usb-stop-confirm-no')));
      await tester.pumpAndSettle();
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 30)),
      );
      expect(usb.entries.single.state, UsbEntryState.shared);
      expect(socket.texts.where((m) => m['type'] == 'usb.detach'), isEmpty);
      await tester.tap(find.byKey(const ValueKey('usb-stop-stick')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('usb-stop-confirm-yes')));
      await tester.pump();
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 30)),
      );
      await tester.pump();
      expect(find.text('사용 가능'), findsOneWidget);
      expect(
        socket.texts.any(
          (m) => m['type'] == 'usb.detach' && m['reason'] == 'usb_user_stopped',
        ),
        isTrue,
      );
      await tester.runAsync(usb.close);
    },
  );

  testWidgets(
    'A refusal says why in words; a missing driver is installed only after a confirmation',
    (tester) async {
      late UsbSession usb;
      late FakeSocket socket;
      late FakeBackend backend;
      await tester.runAsync(() async {
        final result = await started();
        usb = result.$1;
        socket = result.$2;
        backend = result.$3;
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      await tester.tap(find.byKey(const ValueKey('usb-share-stick')));
      await tester.pump();
      await tester.runAsync(() async {
        socket.emit({
          'type': 'usb.rejected',
          'device': 1,
          'reason': 'usb_class_not_granted',
        });
        await Future<void>.delayed(const Duration(milliseconds: 30));
      });
      await tester.pump();
      // A USB stick needs the drive permission too: named exactly, as the
      // admin console's 장치 공유 group names it.
      expect(
        find.text('이 장치를 쓰려면 ‘공유 폴더’ 권한도 필요해요. 관리자에게 요청하세요.'),
        findsOneWidget,
      );

      await tester.runAsync(() async {
        backend.driver = UsbDriverState.missing;
        await usb.refresh();
      });
      await tester.pump();
      expect(find.byKey(const Key('usb-install')), findsOneWidget);
      expect(find.text('USB 드라이버 설치가 필요해요.'), findsOneWidget);
      expect(backend.installed, isFalse);
      await tester.tap(find.byKey(const Key('usb-install')));
      await tester.pumpAndSettle();
      expect(find.text('UsbDk'), findsOneWidget);
      expect(find.textContaining('서명을 확인'), findsWidgets);
      await tester.tap(find.byKey(const Key('usb-install-cancel')));
      await tester.pumpAndSettle();
      expect(backend.installed, isFalse, reason: 'cancel installs nothing');
      await tester.tap(find.byKey(const Key('usb-install')));
      await tester.pumpAndSettle();
      await tester.runAsync(() async {
        await tester.tap(find.byKey(const Key('usb-install-confirm')));
        await Future<void>.delayed(const Duration(milliseconds: 50));
      });
      await tester.pumpAndSettle();
      expect(backend.installed, isTrue);
      expect(find.byKey(const Key('usb-install')), findsNothing);
      await tester.runAsync(usb.close);
    },
  );

  testWidgets(
    'The driver install names the Windows approval; a needed restart is the state, not a red failure or a second install',
    (tester) async {
      late UsbSession usb;
      late FakeBackend backend;
      await tester.runAsync(() async {
        final result = await started();
        usb = result.$1;
        backend = result.$3;
        backend.driver = UsbDriverState.missing;
        await usb.refresh();
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      expect(find.text('USB 드라이버 설치…'), findsOneWidget);
      await tester.tap(find.byKey(const Key('usb-install')));
      await tester.pumpAndSettle();
      expect(find.textContaining('관리자 승인을 물으면'), findsOneWidget);
      backend.installFailure = const UsbBackendException(
        'usbdk_restart_required',
      );
      await tester.runAsync(() async {
        await tester.tap(find.byKey(const Key('usb-install-confirm')));
        await Future<void>.delayed(const Duration(milliseconds: 50));
      });
      await tester.pumpAndSettle();
      expect(usb.restartRequired, isTrue);
      expect(
        tester.widget<Text>(find.byKey(const Key('usb-status'))).data,
        'PC를 다시 시작하면 USB 장치를 쓸 수 있어요.',
      );
      expect(find.byKey(const Key('usb-notice')), findsNothing);
      expect(find.byKey(const Key('usb-install')), findsNothing);
      expect(find.textContaining('UsbDk'), findsNothing);
      await tester.runAsync(usb.close);
    },
  );

  testWidgets(
    'A needed restart offers 지금 다시 시작 and 나중에; the restart is only requested after the save-your-work question',
    (tester) async {
      late UsbSession usb;
      late FakeBackend backend;
      var restarts = 0;
      await tester.runAsync(() async {
        final result = await started();
        usb = result.$1;
        backend = result.$3;
        backend.driver = UsbDriverState.missing;
        await usb.refresh();
        backend.installFailure = const UsbBackendException(
          'usbdk_restart_required',
        );
        await usb.installDriver();
      });
      await tester.pumpWidget(
        host(
          usb,
          restartPc: () async {
            restarts++;
            return true;
          },
        ),
      );
      await tester.pump();
      expect(find.byKey(const Key('usb-restart-now')), findsOneWidget);
      expect(find.byKey(const Key('usb-restart-later')), findsOneWidget);
      await tester.tap(find.byKey(const Key('usb-restart-now')));
      await tester.pumpAndSettle();
      expect(find.textContaining('저장하지 않은 작업은 사라질 수 있어요'), findsOneWidget);
      await tester.tap(find.byKey(const Key('usb-restart-confirm-no')));
      await tester.pumpAndSettle();
      expect(restarts, 0, reason: '취소 restarts nothing');
      await tester.tap(find.byKey(const Key('usb-restart-now')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('usb-restart-confirm-yes')));
      await tester.pumpAndSettle();
      expect(restarts, 1);
      // A request, not a restart: said as asked.
      expect(
        tester.widget<Text>(find.byKey(const Key('usb-status'))).data,
        startsWith('다시 시작을 요청했어요.'),
      );
      expect(find.byKey(const Key('usb-restart-now')), findsNothing);

      // 나중에 keeps the state and drops the buttons.
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpWidget(host(usb));
      await tester.pump();
      await tester.tap(find.byKey(const Key('usb-restart-later')));
      await tester.pump();
      expect(find.byKey(const Key('usb-restart-now')), findsNothing);
      expect(
        tester.widget<Text>(find.byKey(const Key('usb-status'))).data,
        'PC를 다시 시작하면 USB 장치를 쓸 수 있어요.',
      );
      await tester.runAsync(usb.close);
    },
  );

  // One window must not say "내 PC에서 장치를 가져오지 못했어요" for a stick
  // another window has.
  testWidgets(
    'A device another window holds names that window; 이 창으로 옮기기 asks its own question first and 취소 changes nothing',
    (tester) async {
      late UsbSession usb;
      late FakeSocket socket;
      late FakeBackend backend;
      await tester.runAsync(() async {
        final result = await started(devices: [stick(otherWindow: 'Code2')]);
        usb = result.$1;
        socket = result.$2;
        backend = result.$3;
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      expect(find.text('Code2 창에서 쓰는 중이에요'), findsOneWidget);
      expect(find.byKey(const ValueKey('usb-share-stick')), findsNothing);
      final move = find.byKey(const ValueKey('usb-move-stick'));
      expect(find.text('이 창으로 옮기기'), findsOneWidget);
      expect(tester.widget<FilledButton>(move).onPressed, isNotNull);

      await tester.tap(move);
      await tester.pumpAndSettle();
      // Its own words (not "USB 연결 끊기" with a 연결 끊기 button).
      expect(find.text('이 창으로 옮길까요?'), findsOneWidget);
      expect(find.text('USB 연결 끊기'), findsNothing);
      expect(
        find.descendant(
          of: find.byKey(const Key('usb-stop-confirm-yes')),
          matching: find.text('옮기기'),
        ),
        findsOneWidget,
      );
      expect(find.text('SanDisk Cruzer'), findsNWidgets(2));
      expect(find.textContaining('저장하지 않은 내용이 사라질 수 있어요'), findsOneWidget);
      expect(find.textContaining('끊으면 장치가 PC로 돌아와요'), findsNothing);
      await tester.tap(find.byKey(const Key('usb-stop-confirm-no')));
      await tester.pumpAndSettle();
      expect(backend.handOverAsks, isEmpty, reason: '취소 changes nothing');
      expect(find.text('Code2 창에서 쓰는 중이에요'), findsOneWidget);

      var held = true;
      backend.holdsElsewhere = () => held;
      backend.devices = [stick()];
      await tester.tap(move);
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('usb-stop-confirm-yes')));
      // The row moves while the device comes over: only the question's
      // route is let finish (a full settle would run into the move's wait).
      await pumpWork(tester, limit: const Duration(milliseconds: 400));
      expect(backend.handOverAsks, ['stick']);
      expect(find.text('Code2 창에서 가져오는 중…'), findsOneWidget);
      expect(
        find.descendant(
          of: find.ancestor(
            of: find.byKey(const ValueKey('usb-status-stick')),
            matching: find.byType(AppBusyLine),
          ),
          matching: find.byKey(const Key('app-busy-line-spinner')),
        ),
        findsOneWidget,
        reason: 'being brought over is work under way',
      );
      expect(find.text('옮기는 중'), findsOneWidget);
      expect(tester.widget<FilledButton>(move).onPressed, isNull);
      expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);

      held = false;
      await tester.pump(const Duration(milliseconds: 150));
      await tester.pump();
      expect(
        socket.texts.where((m) => m['type'] == 'usb.attach'),
        hasLength(1),
      );
      expect(find.text('데스크톱에 연결하는 중…'), findsOneWidget);
      expect(find.byKey(const Key('usb-notice')), findsNothing);
      await tester.runAsync(usb.close);
    },
  );

  testWidgets('A move that did not happen is said plainly', (tester) async {
    late UsbSession usb;
    late FakeBackend backend;
    await tester.runAsync(() async {
      final result = await started(
        devices: [stick(otherWindow: 'Code2')],
        moveWait: const Duration(milliseconds: 300),
      );
      usb = result.$1;
      backend = result.$3;
      backend.holdsElsewhere = () => true;
    });
    await tester.pumpWidget(host(usb));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey('usb-move-stick')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('usb-stop-confirm-yes')));
    await tester.pumpAndSettle();
    await tester.pump(const Duration(milliseconds: 500));
    await tester.pump();
    expect(
      tester.widget<Text>(find.byKey(const Key('usb-notice'))).data,
      usbMessage(usbMoveTimeout),
    );
    expect(find.text('Code2 창에서 쓰는 중이에요'), findsOneWidget);
    expect(find.text('이 창으로 옮기기'), findsOneWidget);
    await tester.runAsync(usb.close);
  });

  // The row must not say "'PC로 되돌리기'를 눌러 주세요" under a button
  // that reads 이 창으로 옮기기.
  testWidgets(
    'A moved device that did not come back to the PC offers PC로 되돌리기, as its line says',
    (tester) async {
      late UsbSession usb;
      late FakeBackend backend;
      await tester.runAsync(() async {
        final result = await started(devices: [stick(otherWindow: 'Code2')]);
        usb = result.$1;
        backend = result.$3;
        backend.returnedState = 2;
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      await tester.tap(find.byKey(const ValueKey('usb-move-stick')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('usb-stop-confirm-yes')));
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 60)),
      );
      await tester.pumpAndSettle();
      expect(
        tester.widget<Text>(find.byKey(const Key('usb-notice'))).data,
        usbMessage(usbNotReturned),
      );
      expect(find.byKey(const ValueKey('usb-move-stick')), findsNothing);
      expect(find.byKey(const ValueKey('usb-checkreturn-stick')), findsOneWidget);
      expect(find.text('PC로 되돌리기'), findsOneWidget);
      await tester.runAsync(usb.close);
    },
  );

  // "장치는 한 번에 4개까지…" must not stay after a device is
  // disconnected.
  testWidgets('A notice goes once the list changed, not only with the next action', (
    tester,
  ) async {
    late UsbSession usb;
    late FakeBackend backend;
    await tester.runAsync(() async {
      final result = await started(
        devices: [stick(otherWindow: 'Code2')],
        moveWait: const Duration(milliseconds: 300),
      );
      usb = result.$1;
      backend = result.$3;
      backend.holdsElsewhere = () => true;
    });
    await tester.pumpWidget(host(usb));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey('usb-move-stick')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('usb-stop-confirm-yes')));
    await tester.pumpAndSettle();
    await tester.pump(const Duration(milliseconds: 500));
    await tester.pump();
    expect(find.byKey(const Key('usb-notice')), findsOneWidget);
    // The same list read again: nothing changed, the notice stays.
    await tester.runAsync(usb.refresh);
    await tester.pump();
    expect(find.byKey(const Key('usb-notice')), findsOneWidget);
    // The other window let the device go: what the notice was about is over.
    backend.devices = [stick()];
    await tester.runAsync(usb.refresh);
    await tester.pump();
    expect(find.byKey(const Key('usb-notice')), findsNothing);
    await tester.runAsync(usb.close);
  });

  // After a channel loss the rows of connected devices must not read a
  // red "장치를 연결하지 못했어요."
  testWidgets(
    'A device the ended channel gave back says so, not that it could not be connected',
    (tester) async {
      late UsbSession usb;
      await tester.runAsync(() async {
        final (session, socket, backend) = await started();
        usb = session;
        await shared(session, socket, backend);
        socket.emit({'type': 'usb.error', 'reason': 'usb_transport_closed'});
        await Future<void>.delayed(const Duration(milliseconds: 60));
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      expect(usb.entries.single.code, 'usb_session_closed');
      expect(
        tester.widget<Text>(find.byKey(const ValueKey('usb-status-stick'))).data,
        '연결이 끊겨 장치가 PC로 돌아왔어요.',
      );
      expect(find.text('장치를 연결하지 못했어요.'), findsNothing);
    },
  );

  testWidgets(
    'A SuperSpeed device has its own line that names the USB 2.0 port',
    (tester) async {
      late UsbSession usb;
      await tester.runAsync(() async {
        final fast = Uint8List.fromList(stick().descriptors)..[7] = 9;
        final result = await started(
          devices: [
            stick(),
            UsbLocalDevice(
              id: 'fast',
              name: 'USB 3 stick',
              descriptors: fast,
              speed: 'super',
            ),
          ],
        );
        usb = result.$1;
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      expect(
        tester.widget<Text>(find.byKey(const Key('usb-superspeed'))).data,
        'USB 3.0 포트에 꽂은 USB 3.0 장치 1개는 연결할 수 없어요. USB 2.0 포트에 꽂아 주세요.',
      );
      expect(find.byKey(const Key('usb-unreadable')), findsNothing);
      await tester.runAsync(usb.close);
    },
  );

  // A wait says that it goes on: the line of a device on its way and of a
  // panel getting ready moves, and the result takes the mark's place.
  testWidgets(
    'In-flight device rows and the waiting panel move; the result or a refusal replaces the mark',
    (tester) async {
      late UsbSession usb;
      late FakeSocket socket;
      await tester.runAsync(() async {
        final result = await started(hostReady: false);
        usb = result.$1;
        socket = result.$2;
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      // The spinner of the busy line whose sentence has this key.
      Finder mark(Key text) => find.descendant(
        of: find.ancestor(
          of: find.byKey(text),
          matching: find.byType(AppBusyLine),
        ),
        matching: find.byKey(const Key('app-busy-line-spinner')),
      );
      const row = ValueKey('usb-status-stick');
      // The desktop is getting ready: the panel's line moves. The device
      // itself is settled (its dot), nothing turns on its row.
      expect(find.textContaining('준비를 하고 있어요'), findsOneWidget);
      expect(mark(const Key('usb-status')), findsOneWidget);
      expect(find.byKey(row), findsOneWidget);
      expect(mark(row), findsNothing);

      await tester.runAsync(() async {
        socket.emit({'type': 'usb.status', 'state': 'ready'});
        await Future<void>.delayed(const Duration(milliseconds: 30));
      });
      await tester.pump();
      expect(find.byKey(const Key('usb-status')), findsNothing);
      expect(find.byKey(const Key('app-busy-line-spinner')), findsNothing);

      // Being attached: the row's line moves until the desktop answers.
      await tester.tap(find.byKey(const ValueKey('usb-share-stick')));
      await tester.pump();
      expect(find.text('데스크톱에 연결하는 중…'), findsOneWidget);
      expect(mark(row), findsOneWidget);

      // Refused: the reason takes the line, and nothing keeps turning.
      await tester.runAsync(() async {
        socket.emit({
          'type': 'usb.rejected',
          'device': socket.texts.lastWhere(
            (m) => m['type'] == 'usb.attach',
          )['device'],
          'reason': 'usb_class_not_granted',
        });
        await Future<void>.delayed(const Duration(milliseconds: 30));
      });
      await tester.pump();
      expect(usb.entries.single.state, UsbEntryState.refused);
      expect(find.text('데스크톱에 연결하는 중…'), findsNothing);
      expect(find.byKey(const Key('app-busy-line-spinner')), findsNothing);
      expect(find.byKey(row), findsOneWidget);

      // Asked again and taken: connected, with its dot and no mark.
      await tester.tap(find.byKey(const ValueKey('usb-share-stick')));
      await tester.pump();
      expect(mark(row), findsOneWidget);
      await tester.runAsync(() async {
        socket.emit({
          'type': 'usb.attached',
          'device': socket.texts.lastWhere(
            (m) => m['type'] == 'usb.attach',
          )['device'],
        });
        await Future<void>.delayed(const Duration(milliseconds: 30));
      });
      await tester.pump();
      expect(find.textContaining('PC에서 쓸 수 없어요'), findsOneWidget);
      expect(find.byKey(const Key('app-busy-line-spinner')), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.runAsync(usb.close);
    },
  );

  testWidgets('Looking for devices moves; the empty list does not', (
    tester,
  ) async {
    late UsbSession usb;
    late FakeBackend backend;
    final listed = Completer<void>();
    await tester.runAsync(() async {
      final result = await started(devices: []);
      usb = result.$1;
      backend = result.$3;
    });
    await tester.pumpWidget(host(usb));
    await tester.pump();
    expect(find.text('연결할 USB 장치가 없어요.'), findsOneWidget);
    expect(find.byKey(const Key('app-busy-line-spinner')), findsNothing);
    // The list is being read again and has not answered.
    backend.listGate = listed;
    await tester.tap(find.byKey(const Key('usb-refresh')));
    await tester.pump();
    expect(usb.listing, isTrue);
    expect(find.text('장치를 찾고 있어요…'), findsOneWidget);
    expect(find.byKey(const Key('app-busy-line-spinner')), findsOneWidget);
    await tester.runAsync(() async {
      listed.complete();
      await Future<void>.delayed(const Duration(milliseconds: 30));
    });
    await tester.pump();
    expect(find.text('장치를 찾고 있어요…'), findsNothing);
    expect(find.byKey(const Key('app-busy-line-spinner')), findsNothing);
    await tester.runAsync(usb.close);
  });

  testWidgets('A finished session says so and offers nothing', (tester) async {
    late UsbSession usb;
    late FakeSocket socket;
    await tester.runAsync(() async {
      final result = await started(devices: [stick()]);
      usb = result.$1;
      socket = result.$2;
      socket.emit({'type': 'usb.error', 'reason': 'usb_policy_changed'});
      await Future<void>.delayed(const Duration(milliseconds: 30));
    });
    await tester.pumpWidget(host(usb));
    await tester.pump();
    expect(find.textContaining('관리자가 USB 권한을 해제했어요'), findsOneWidget);
    final share = tester.widget<FilledButton>(
      find.byKey(const ValueKey('usb-share-stick')),
    );
    expect(share.onPressed, isNull);
  });
}
