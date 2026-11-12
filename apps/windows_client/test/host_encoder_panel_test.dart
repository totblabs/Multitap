import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_settings_api.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_encoder_panel.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'host_encoder_settings_test.dart' show encoderFixture;
import 'host_settings_test.dart' as existing;

const _second = 'abcdef01:23456789';

/// [encoderFixture] with a second GPU candidate (AMD).
Map<String, dynamic> twoGpus({String backend = 'x264', String? adapter}) {
  final value = encoderFixture(backend: backend, adapter: adapter);
  (value['adapters'] as List).add({
    'id': _second,
    'vendorId': 0x1002,
    'deviceId': 0x744c,
    'name': 'Second GPU',
    'candidate': true,
  });
  return value;
}

class _Api implements HostSettingsApi {
  final base = existing.FakeHostSettings(registered: true);
  Map<String, dynamic> settings = encoderFixture();
  final calls = <Map<String, dynamic>>[];
  bool loseReply = false, foreign = false;

  /// Thrown by the next apply (and the stored settings are replaced by
  /// [conflictSettings] first, as another writer would have left them).
  String? failWith;
  Map<String, dynamic>? conflictSettings;
  Completer<Map<String, dynamic>>? readGate;
  Completer<void>? applyGate;
  @override
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]) async {
    calls.add({'action': action, ...fields});
    if (action == 'encoderSettings') {
      if (readGate != null) return await readGate!.future;
      return {'ok': true, 'encoderSettings': settings};
    }
    if (action == 'applyEncoderSettings') {
      if (applyGate != null) await applyGate!.future;
      if (failWith != null) {
        if (conflictSettings != null) settings = conflictSettings!;
        final code = failWith!;
        failWith = null;
        throw HostSettingsException(code, code: code);
      }
      settings = {
        ...settings,
        'backend': fields['backend'],
        'adapterId': fields['adapterId'],
        'operationId': foreign
            ? 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
            : fields['operationId'],
        'revision': List.filled(64, 'b').join(),
      };
      if (loseReply) throw const HostSettingsException('synthetic lost reply');
      return {'ok': true, 'encoderSettings': settings};
    }
    return base.request(action, fields);
  }

  List<Map<String, dynamic>> get saves =>
      calls.where((v) => v['action'] == 'applyEncoderSettings').toList();
}

Future<void> _open(
  WidgetTester t,
  _Api api, {
  bool protect = true,
  bool administrator = true,
  ValueChanged<bool>? pending,
  ValueChanged<bool>? dirty,
  Size size = const Size(900, 900),
  double scale = 1,
  Brightness brightness = Brightness.dark,
}) async {
  await t.binding.setSurfaceSize(size);
  addTearDown(() => t.binding.setSurfaceSize(null));
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: brightness),
      home: MediaQuery(
        data: MediaQueryData(size: size, textScaler: TextScaler.linear(scale)),
        child: Scaffold(
          body: SingleChildScrollView(
            child: HostEncoderPanel(
              api: api,
              administrator: administrator,
              blocked: false,
              protectMutation: (begin) async => protect,
              onDirty: dirty ?? (_) {},
              onPending: pending,
            ),
          ),
        ),
      ),
    ),
  );
  await t.pumpAndSettle();
}

Finder _option(String value) => find.byKey(Key('host-encoder-option-$value'));

Future<void> _choose(WidgetTester t, String value) async {
  await t.ensureVisible(_option(value));
  await t.tap(_option(value));
  await t.pumpAndSettle();
}

List<String> _order(WidgetTester t) => [
  for (final tile in t.widgetList<RadioListTile<String>>(
    find.descendant(
      of: find.byKey(const Key('host-encoder-options')),
      matching: find.byType(RadioListTile<String>),
    ),
  ))
    tile.value,
];

bool _enabled(WidgetTester t, String value) =>
    t.widget<RadioListTile<String>>(_option(value)).enabled ?? true;

void main() {
  testWidgets('automatic comes first, then GPU and the two CPU choices', (
    t,
  ) async {
    final api = _Api();
    await _open(t, api);
    expect(_order(t), ['auto', 'gpu', 'x264', 'mft']);
    expect(find.text('자동 (권장)'), findsOneWidget);
    expect(find.text('GPU가 있으면 GPU, 없으면 CPU'), findsOneWidget);
    expect(find.text('GPU'), findsOneWidget);
    expect(find.text('CPU (x264)'), findsOneWidget);
    expect(find.text('CPU (Windows 기본)'), findsOneWidget);
    // No save or cancel buttons, no dropdown, no icon buttons.
    expect(find.byKey(const Key('host-encoder-apply')), findsNothing);
    expect(find.byKey(const Key('host-encoder-cancel')), findsNothing);
    expect(find.text('선택 저장'), findsNothing);
    expect(find.text('변경 취소'), findsNothing);
    expect(find.byType(DropdownButtonFormField<String>), findsNothing);
    expect(
      find.descendant(
        of: find.byKey(const Key('host-encoder-settings')),
        matching: find.byType(Icon),
      ),
      findsOneWidget, // only the 자세히 fold's chevron
    );
    expect(api.saves, isEmpty);
  });

  testWidgets(
    'saved automatic choice keeps timestamped historical evidence in one line',
    (t) async {
      final api = _Api();
      api.settings = encoderFixture(backend: 'auto')
        ..['recentUsage'] = {
          'protocol': 'host-encoder-usage-v1',
          'trackId': '0',
          'time': '2026-10-30T01:02:03.004Z',
          'backend': 'x264',
          'hardwareVerified': false,
          'gpuFallbackCode': 0,
          'softwareFallbackCode': 0,
        };
      await _open(t, api, size: const Size(460, 1000), scale: 2);
      await t.ensureVisible(find.byKey(const Key('host-encoder-recent-usage')));
      expect(find.textContaining('최근 확인: CPU · x264 ('), findsOneWidget);
      expect(
        t.widget<RadioListTile<String>>(_option('auto')).value,
        'auto',
      );
      for (final folded in [
        '저장된 선택: 자동 (권장)',
        '다음 스트리밍 연결',
        '현재 연결 상태를 뜻하지 않아요',
        '캡처에 사용한 GPU',
        '최근 출력: 화면 1',
      ]) {
        expect(find.textContaining(folded), findsNothing, reason: folded);
      }
      await t.ensureVisible(find.byKey(const Key('host-encoder-details')));
      await t.tap(find.byKey(const Key('host-encoder-details')));
      await t.pumpAndSettle();
      expect(find.text('저장된 선택: 자동 (권장)'), findsOneWidget);
      expect(find.textContaining('다음 스트리밍 연결'), findsOneWidget);
      expect(find.textContaining('현재 연결 상태를 뜻하지 않아요'), findsOneWidget);
      expect(find.textContaining('캡처에 사용한 GPU'), findsOneWidget);
      expect(find.textContaining('최근 출력: 화면 1 · CPU · x264 · '), findsOneWidget);
      expect(api.saves, isEmpty);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'choosing automatic applies at once with no LUID and no fabricated GPU output',
    (t) async {
      final api = _Api();
      await _open(t, api);
      await _choose(t, 'auto');
      expect(api.saves, hasLength(1));
      final save = api.saves.single;
      expect(save['backend'], 'auto');
      expect(save['adapterId'], isNull);
      expect(save['confirmed'], true);
      expect(save['expectedRevision'], List.filled(64, 'a').join());
      expect(
        save['operationId'],
        matches(RegExp(r'^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$')),
      );
      expect(save.keys.toSet(), {
        'action',
        'operationId',
        'expectedRevision',
        'backend',
        'adapterId',
        'confirmed',
      });
      expect(find.text('적용했어요'), findsOneWidget);
      expect(
        t.widget<Text>(find.byKey(const Key('host-encoder-usage-unknown'))).data,
        '최근 확인: 아직 기록이 없어요',
      );
      expect(find.byKey(const Key('host-encoder-recent-usage')), findsNothing);
      // Choosing the saved value again sends nothing.
      await _choose(t, 'auto');
      expect(api.saves, hasLength(1));
    },
  );

  testWidgets('the list is disabled while the change is applied', (t) async {
    final api = _Api()..applyGate = Completer<void>();
    await _open(t, api);
    await t.tap(_option('mft'));
    await t.pump();
    await t.pump();
    expect(api.saves, hasLength(1));
    for (final value in ['auto', 'gpu', 'x264', 'mft']) {
      expect(_enabled(t, value), isFalse, reason: value);
    }
    expect(find.text('적용 중'), findsOneWidget);
    expect(
      t.widget<RadioGroup<String>>(find.byType(RadioGroup<String>).first)
          .groupValue,
      'mft',
    );
    api.applyGate!.complete();
    await t.pumpAndSettle();
    expect(_enabled(t, 'x264'), isTrue);
    expect(find.text('적용했어요'), findsOneWidget);
    expect(
      api.calls.any(
        (v) => v['action'] == 'service' || v['action'] == 'display',
      ),
      isFalse,
    );
  });

  testWidgets(
    'old Host keeps CPU and manual choices without exposing unsupported auto',
    (t) async {
      final api = _Api();
      api.settings.remove('autoSupported');
      await _open(t, api);
      expect(_order(t), ['gpu', 'x264', 'mft']);
      expect(find.text('자동 (권장)'), findsNothing);
      expect(api.saves, isEmpty);
    },
  );

  testWidgets('nothing is sent when close protection cannot start', (t) async {
    final api = _Api();
    await _open(t, api, protect: false);
    await _choose(t, 'mft');
    expect(api.saves, isEmpty);
    expect(find.textContaining('적용을 시작하지 못했어요'), findsOneWidget);
    // The saved choice stays shown.
    expect(
      t.widget<RadioGroup<String>>(find.byType(RadioGroup<String>).first)
          .groupValue,
      'x264',
    );
  });

  testWidgets(
    'lost apply reply uses GET only and retains UUID until matching current status',
    (t) async {
      final api = _Api()..loseReply = true;
      final pending = <bool>[];
      await _open(t, api, pending: pending.add);
      await _choose(t, 'mft');
      final id = api.saves.single['operationId'];
      expect(pending.last, isTrue);
      expect(_enabled(t, 'x264'), isFalse);
      await t.tap(find.byKey(const Key('host-encoder-refresh')));
      await t.pumpAndSettle();
      expect(api.saves, hasLength(1));
      expect(api.settings['operationId'], id);
      expect(pending.last, isFalse);
      expect(find.text('적용했어요'), findsOneWidget);
    },
  );

  testWidgets('foreign status never confirms the pending operation', (t) async {
    final api = _Api()..foreign = true;
    final pending = <bool>[];
    await _open(t, api, pending: pending.add);
    await _choose(t, 'mft');
    await t.tap(find.byKey(const Key('host-encoder-refresh')));
    await t.pumpAndSettle();
    expect(api.saves, hasLength(1));
    expect(pending.last, isTrue);
    expect(find.textContaining('아직 확인하지 못했어요'), findsOneWidget);
  });

  testWidgets(
    'a revision conflict reloads the saved choice and says so',
    (t) async {
      final api = _Api()
        ..failWith = 'encoder_settings_changed'
        ..conflictSettings = (encoderFixture(backend: 'auto')
          ..['revision'] = List.filled(64, 'c').join());
      final pending = <bool>[];
      await _open(t, api, pending: pending.add);
      final reads = api.calls
          .where((v) => v['action'] == 'encoderSettings')
          .length;
      await _choose(t, 'mft');
      expect(api.saves, hasLength(1));
      expect(
        api.calls.where((v) => v['action'] == 'encoderSettings').length,
        reads + 1,
      );
      expect(pending.last, isFalse);
      expect(
        find.text('다른 곳에서 설정이 바뀌어 다시 불러왔어요. 다시 골라 주세요.'),
        findsOneWidget,
      );
      expect(
        t.widget<RadioGroup<String>>(find.byType(RadioGroup<String>).first)
            .groupValue,
        'auto',
      );
      // The next choice uses the reloaded revision.
      await _choose(t, 'x264');
      expect(api.saves, hasLength(2));
      expect(api.saves.last['expectedRevision'], List.filled(64, 'c').join());
      expect(api.saves.last['backend'], 'x264');
    },
  );

  testWidgets('one GPU candidate is applied with its exact LUID at once', (
    t,
  ) async {
    final api = _Api();
    await _open(t, api);
    await _choose(t, 'gpu');
    expect(api.saves.single['backend'], 'gpu');
    expect(api.saves.single['adapterId'], 'fedcba98:76543210');
    expect(find.byKey(const Key('host-encoder-adapter-label')), findsNothing);
    expect(find.text('적용했어요'), findsOneWidget);
  });

  testWidgets(
    'several GPUs ask which one and apply only on that second choice',
    (t) async {
      final api = _Api()..settings = twoGpus();
      final dirty = <bool>[];
      await _open(t, api, dirty: dirty.add);
      await _choose(t, 'gpu');
      expect(api.saves, isEmpty);
      expect(dirty.last, isTrue);
      expect(find.text('쓸 GPU를 고르세요.'), findsOneWidget);
      expect(find.text('Synthetic GPU'), findsOneWidget);
      expect(find.text('Second GPU'), findsOneWidget);
      await t.tap(find.byKey(const Key('host-encoder-adapter-$_second')));
      await t.pumpAndSettle();
      expect(api.saves.single['backend'], 'gpu');
      expect(api.saves.single['adapterId'], _second);
      expect(dirty.last, isFalse);
      expect(find.text('쓸 GPU'), findsOneWidget);
      // Going back to CPU drops the GPU list and applies at once.
      await _choose(t, 'x264');
      expect(api.saves.last['backend'], 'x264');
      expect(api.saves.last['adapterId'], isNull);
      expect(find.byKey(const Key('host-encoder-adapter-label')), findsNothing);
    },
  );

  testWidgets('GPU cannot be chosen without a confirmed candidate', (t) async {
    final api = _Api()..settings = encoderFixture(known: false);
    await _open(t, api);
    expect(_enabled(t, 'gpu'), isFalse);
    expect(find.text('GPU 목록을 확인하지 못했어요. 상태를 다시 확인하세요.'), findsOneWidget);
    await t.tap(_option('gpu'));
    await t.pumpAndSettle();
    expect(api.saves, isEmpty);
  });

  for (final (name, widgetAdmin, hostAdmin) in [
    ('Host settings not elevated', false, true),
    ('Host reports no administrator', true, false),
  ]) {
    testWidgets('read-only with one line when $name', (t) async {
      final api = _Api();
      api.settings['administrator'] = hostAdmin;
      await _open(t, api, administrator: widgetAdmin);
      for (final value in ['auto', 'gpu', 'x264', 'mft']) {
        expect(_enabled(t, value), isFalse, reason: value);
      }
      expect(
        find.text('관리자 권한으로 Host 설정을 열면 바꿀 수 있어요.'),
        findsOneWidget,
      );
      await t.tap(_option('mft'));
      await t.pumpAndSettle();
      expect(api.saves, isEmpty);
    });
  }

  testWidgets('administrator sees no read-only line', (t) async {
    await _open(t, _Api());
    expect(find.byKey(const Key('host-encoder-admin-only')), findsNothing);
  });

  group('English', () {
    setUp(() {
      AppLanguageController.debugSystemLanguage = 'en';
      AppLanguageController.instance.debugReset(AppLanguage.en);
    });
    tearDown(() {
      AppLanguageController.debugSystemLanguage = 'ko';
      AppLanguageController.instance.debugReset();
    });
    testWidgets('labels, applied note and last confirmed line', (t) async {
      final api = _Api();
      api.settings['recentUsage'] = {
        'protocol': 'host-encoder-usage-v1',
        'trackId': '0',
        'time': '2026-10-30T01:02:03.004Z',
        'backend': 'nvenc',
        'hardwareVerified': true,
        'gpuFallbackCode': 0,
        'softwareFallbackCode': 0,
      };
      await _open(t, api);
      expect(find.text('Automatic (recommended)'), findsOneWidget);
      expect(find.text('GPU if there is one, otherwise CPU'), findsOneWidget);
      expect(find.text('GPU'), findsOneWidget);
      expect(find.text('CPU (x264)'), findsOneWidget);
      expect(find.text('CPU (Windows built-in)'), findsOneWidget);
      expect(
        find.textContaining('Last confirmed: GPU · NVIDIA NVENC ('),
        findsOneWidget,
      );
      await _choose(t, 'mft');
      expect(find.text('Applied'), findsOneWidget);
    });
    testWidgets('read-only line', (t) async {
      await _open(t, _Api(), administrator: false);
      expect(
        find.text('Open Host settings as administrator to change this.'),
        findsOneWidget,
      );
    });
  });

  testWidgets(
    'historical GPU output is visible after live observation has expired',
    (t) async {
      final api = _Api();
      api.settings['recentUsage'] = {
        'protocol': 'host-encoder-usage-v1',
        'trackId': '0',
        'time': '2026-10-30T01:02:03.004Z',
        'backend': 'nvenc',
        'hardwareVerified': true,
        'gpuFallbackCode': 0,
        'softwareFallbackCode': 0,
      };
      await _open(t, api, size: const Size(460, 1000), scale: 2);
      await t.ensureVisible(find.byKey(const Key('host-encoder-recent-usage')));
      expect(find.textContaining('최근 확인: GPU · NVIDIA NVENC ('), findsOneWidget);
      expect(find.byKey(const Key('host-encoder-usage-unknown')), findsNothing);
      await t.ensureVisible(find.byKey(const Key('host-encoder-details')));
      await t.tap(find.byKey(const Key('host-encoder-details')));
      await t.pumpAndSettle();
      expect(find.textContaining('현재 연결 상태를 뜻하지 않아요'), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'late read after card disposal cannot acquire or mutate ownership',
    (t) async {
      final api = _Api()..readGate = Completer<Map<String, dynamic>>();
      final pending = <bool>[];
      await t.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: HostEncoderPanel(
              api: api,
              administrator: true,
              blocked: false,
              protectMutation: (_) async => true,
              onDirty: (_) {},
              onPending: pending.add,
            ),
          ),
        ),
      );
      await t.pump();
      await t.pumpWidget(const SizedBox());
      api.readGate!.complete({
        'ok': true,
        'encoderSettings': encoderFixture(
          backend: 'gpu',
          adapter: 'fedcba98:76543210',
        ),
      });
      await t.pump();
      expect(api.saves, isEmpty);
      expect(pending, isEmpty);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'actual Host settings display pane applies through the close guard',
    (t) async {
      final api = _Api(), guard = existing.FakeCloseGuard();
      await t.binding.setSurfaceSize(const Size(1200, 1000));
      addTearDown(() => t.binding.setSurfaceSize(null));
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: HostSettingsScreen(api: api, closeGuard: guard),
        ),
      );
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('host-nav-2')));
      await t.pumpAndSettle();
      await t.ensureVisible(find.byKey(const Key('host-encoder-settings')));
      expect(find.byKey(const Key('host-encoder-settings')), findsOneWidget);
      await _choose(t, 'mft');
      expect(api.saves.single['backend'], 'mft');
      expect(guard.dirty, isFalse);
      expect(find.text('적용했어요'), findsOneWidget);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets(
    'several GPUs on the Host settings page guard the unpicked GPU choice',
    (t) async {
      final api = _Api()..settings = twoGpus(), guard = existing.FakeCloseGuard();
      await t.binding.setSurfaceSize(const Size(1200, 1000));
      addTearDown(() => t.binding.setSurfaceSize(null));
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: HostSettingsScreen(api: api, closeGuard: guard),
        ),
      );
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('host-nav-2')));
      await t.pumpAndSettle();
      await _choose(t, 'gpu');
      expect(guard.dirty, isTrue);
      expect(api.saves, isEmpty);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  for (final width in [460.0, 800.0, 1200.0]) {
    for (final brightness in Brightness.values) {
      testWidgets('GPU settings wraps $width $brightness large text', (
        t,
      ) async {
        final api = _Api()
          ..settings = twoGpus(backend: 'gpu', adapter: 'fedcba98:76543210');
        await _open(
          t,
          api,
          size: Size(width, 1000),
          scale: 2,
          brightness: brightness,
        );
        await t.ensureVisible(find.byKey(const Key('host-encoder-details')));
        await t.pumpAndSettle();
        expect(t.takeException(), isNull);
        expect(find.byKey(const Key('host-encoder-adapter-label')), findsOneWidget);
      });
    }
  }
}
