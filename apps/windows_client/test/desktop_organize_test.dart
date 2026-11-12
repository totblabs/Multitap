import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/desktop_organize.dart';

DesktopMachine machine(
  String id,
  String name, {
  List<String> tags = const [],
  String? pool,
  bool? favorite,
  DateTime? used,
  String power = 'running',
  String os = 'Windows 11',
  String description = '',
}) => DesktopMachine(
  id: id,
  name: name,
  description: description,
  kind: MachineKind.vm,
  status: power == 'stopped' ? MachineStatus.poweredOff : MachineStatus.ready,
  specification: os,
  accent: Colors.blue,
  powerState: power,
  powerStale: false,
  hostOnline: power == 'running',
  organize: DesktopOrganize(
    tags: tags,
    pool: pool,
    favorite: favorite ?? false,
    lastUsedAt: used,
    supported: favorite != null,
  ),
);

List<String> ids(List<DesktopMachine> list) => [for (final m in list) m.id];

void main() {
  group('search', () {
    final list = [
      machine('pve-149', '사무실 PC', tags: ['업무'], pool: '영업팀'),
      machine('pve-150', 'Dev Box', tags: ['dev', 'linux-tools']),
      machine('pve-1490', '민수의 데스크톱'),
      machine('pve-7', 'Lab', description: 'GPU 테스트'),
    ];
    List<String> find(String query) =>
        ids(organizeDesktops(list, query: query));

    test('matches name case-insensitively', () {
      expect(find('dev'), ['pve-150']);
      expect(find('DEV BOX'), ['pve-150']);
      expect(find('사무'), ['pve-149']);
    });

    test('matches the VM ID as 149 and #149', () {
      expect(find('149'), ['pve-149', 'pve-1490']);
      expect(find('#149'), ['pve-149', 'pve-1490']);
      expect(find('#7'), ['pve-7']);
      expect(find('#'), list.map((m) => m.id));
    });

    test('matches tags, pool and the visible OS or description text', () {
      expect(find('업무'), ['pve-149']);
      expect(find('linux'), ['pve-150']);
      expect(find('영업'), ['pve-149']);
      expect(find('gpu'), ['pve-7']);
      expect(find('windows 11'), list.map((m) => m.id));
    });

    test('every word must match', () {
      expect(find('사무실 업무'), ['pve-149']);
      expect(find('사무실 dev'), isEmpty);
    });

    test('Korean initial consonants stand for syllables', () {
      expect(find('ㅅㅁㅅ'), ['pve-149']);
      expect(find('ㅅㅁ'), ['pve-149']);
      expect(find('ㅁㅅ'), ['pve-149', 'pve-1490']);
      expect(find('ㅁㅅㅇ'), ['pve-1490']);
      // Spaces in the name do not break an initial-consonant word.
      expect(find('ㅁㅅㅇㄷㅅㅋㅌ'), ['pve-1490']);
      expect(find('devbox'), ['pve-150']);
      // Mixed with full syllables.
      expect(find('사ㅁ실'), ['pve-149']);
      expect(find('ㅅ무실'), ['pve-149']);
      expect(find('ㅂㅂ'), isEmpty);
    });

    test('loose jamo compose into syllables', () {
      expect(composeJamo('ㅅㅏㅁㅜ'), '사무');
      expect(composeJamo('ㅅㅏㅁㅁㅜ'), '삼무');
      expect(composeJamo('ㅅㅏㅁㅏ'), '사마');
      expect(composeJamo('ㅅㅁ'), 'ㅅㅁ');
      expect(find('ㅅㅏㅁㅜ'), ['pve-149']);
      // A cluster typed alone is two initial consonants (ㄳ = ㄱㅅ).
      expect(composeJamo('ㄳ'), 'ㄱㅅ');
    });

    test('an empty query keeps everything in order', () {
      expect(find(''), list.map((m) => m.id));
      expect(find('   '), list.map((m) => m.id));
    });
  });

  group('chips', () {
    final list = [
      machine('pve-1', 'A', tags: ['업무', 'dev'], pool: '팀', favorite: true),
      machine('pve-2', 'B', tags: ['dev'], power: 'stopped', favorite: false),
      machine('pve-3', 'C', pool: '팀', favorite: false),
      machine('pve-4', 'D', tags: ['Zeta'], favorite: false),
    ];

    test('즐겨찾기, pools, tags; empty ones are hidden, no 전체/상태 chips', () {
      expect(
        [for (final (chip, _) in chipsFor(list)) chip.id],
        ['favorites', 'pool:팀', 'tag:dev', 'tag:Zeta', 'tag:업무'],
      );
      final counts = {for (final (c, n) in chipsFor(list)) c.id: n};
      expect(counts['favorites'], 1);
      expect(counts['tag:dev'], 2);
    });

    test('with no favourites, tags or pools there is no chip at all', () {
      final calm = [machine('a', 'A'), machine('b', 'B', power: 'stopped')];
      expect(chipsFor(calm), isEmpty);
    });
    test('a chip admits only its members and ANDs with the search', () {
      expect(
        ids(
          organizeDesktops(
            list,
            chip: const DesktopChip(DesktopChipKind.tag, 'dev'),
          ),
        ),
        ['pve-1', 'pve-2'],
      );
      expect(
        ids(
          organizeDesktops(
            list,
            chip: const DesktopChip(DesktopChipKind.tag, 'dev'),
            query: 'b',
          ),
        ),
        ['pve-2'],
      );
      expect(
        ids(
          organizeDesktops(
            list,
            chip: const DesktopChip(DesktopChipKind.pool, '팀'),
          ),
        ),
        ['pve-1', 'pve-3'],
      );
    });

    test('chip ids round-trip and unknown ids mean 전체', () {
      for (final (chip, _) in chipsFor(list)) {
        expect(DesktopChip.parse(chip.id), chip);
      }
      expect(DesktopChip.parse(null), DesktopChip.all);
      expect(DesktopChip.parse('nonsense'), DesktopChip.all);
      expect(DesktopChip.parse('tag:'), DesktopChip.all);
      // 실행 중 / 꺼짐 are no chips any more (the stored ones are migrated).
      expect(DesktopChip.parse('running'), DesktopChip.all);
      expect(DesktopChip.parse('stopped'), DesktopChip.all);
    });

    test('a chip that lost its members counts as 전체', () {
      expect(
        effectiveChip(const DesktopChip(DesktopChipKind.tag, 'dev'), list),
        const DesktopChip(DesktopChipKind.tag, 'dev'),
      );
      expect(
        effectiveChip(const DesktopChip(DesktopChipKind.tag, '없음'), list),
        DesktopChip.all,
      );
    });

    test(
      'the row is worth showing from four desktops or any tag/favourite',
      () {
        expect(organizeUseful([machine('a', 'A'), machine('b', 'B')]), false);
        expect(
          organizeUseful([
            machine('a', 'A', favorite: false),
            machine('b', 'B'),
          ]),
          false,
        );
        expect(
          organizeUseful([
            machine('a', 'A', tags: ['x']),
          ]),
          true,
        );
        expect(organizeUseful([machine('a', 'A', pool: 'p')]), true);
        expect(organizeUseful([machine('a', 'A', favorite: true)]), true);
        expect(
          organizeUseful([for (var i = 0; i < 4; i++) machine('v$i', 'V$i')]),
          true,
        );
      },
    );
  });

  group('상태 filter', () {
    final list = [
      machine('pve-1', 'A', tags: ['dev'], favorite: true),
      machine('pve-2', 'B', tags: ['dev'], power: 'stopped', favorite: false),
      machine('pve-3', 'C', power: 'stopped', favorite: false),
      machine('pve-4', 'D', favorite: false),
    ];

    test('counts 전체, 실행 중 and 꺼짐', () {
      final counts = DesktopStatusCounts.of(list);
      expect((counts.all, counts.running, counts.stopped), (4, 2, 2));
      expect(counts.of(DesktopStatus.stopped), 2);
      final none = DesktopStatusCounts.of(const []);
      expect((none.all, none.running, none.stopped), (0, 0, 0));
    });

    test('an unknown power state is neither running nor stopped', () {
      final unknown = DesktopMachine(
        id: 'pve-9',
        name: 'U',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.awaitingHost,
        specification: '',
        accent: Colors.blue,
        powerState: 'unknown',
        powerStale: true,
      );
      final counts = DesktopStatusCounts.of([unknown, ...list]);
      expect((counts.all, counts.running, counts.stopped), (5, 2, 2));
      expect(ids(organizeDesktops([unknown], status: DesktopStatus.all)), [
        'pve-9',
      ]);
      expect(
        organizeDesktops([unknown], status: DesktopStatus.running),
        isEmpty,
      );
      expect(
        organizeDesktops([unknown], status: DesktopStatus.stopped),
        isEmpty,
      );
    });

    test('filters by state and ANDs with the chip and the search', () {
      expect(ids(organizeDesktops(list, status: DesktopStatus.running)), [
        'pve-1',
        'pve-4',
      ]);
      expect(ids(organizeDesktops(list, status: DesktopStatus.stopped)), [
        'pve-2',
        'pve-3',
      ]);
      const dev = DesktopChip(DesktopChipKind.tag, 'dev');
      expect(
        ids(organizeDesktops(list, status: DesktopStatus.stopped, chip: dev)),
        ['pve-2'],
      );
      expect(
        ids(
          organizeDesktops(
            list,
            status: DesktopStatus.stopped,
            chip: dev,
            query: 'a',
          ),
        ),
        isEmpty,
      );
      expect(
        ids(organizeDesktops(list, status: DesktopStatus.running, query: '4')),
        ['pve-4'],
      );
    });

    test('a status with no members counts as 전체', () {
      expect(
        effectiveStatus(DesktopStatus.running, list),
        DesktopStatus.running,
      );
      final calm = [machine('a', 'A'), machine('b', 'B')];
      expect(effectiveStatus(DesktopStatus.stopped, calm), DesktopStatus.all);
      expect(effectiveStatus(DesktopStatus.all, calm), DesktopStatus.all);
      expect(
        effectiveStatus(DesktopStatus.running, const []),
        DesktopStatus.all,
      );
    });

    test('ids round-trip and unknown ids mean 전체', () {
      for (final status in DesktopStatus.values) {
        expect(DesktopStatus.byId(status.id), status);
      }
      expect(DesktopStatus.byId('zzz'), DesktopStatus.all);
      expect(DesktopStatus.byId(null), DesktopStatus.all);
    });

    test('the empty-result texts name the active conditions', () {
      expect(noMatchTitle(''), '조건에 맞는 데스크톱이 없어요');
      expect(noMatchTitle(' abc '), "'abc'에 맞는 데스크톱이 없어요");
      expect(noMatchTitle('가' * 30), "'${'가' * 24}…'에 맞는 데스크톱이 없어요");
      expect(
        noMatchDetail(DesktopStatus.all, DesktopChip.all),
        '이름, 번호, 분류로 찾을 수 있어요.',
      );
      expect(
        noMatchDetail(DesktopStatus.running, DesktopChip.all),
        "'실행 중' 조건 안에서 찾았어요.",
      );
      expect(
        noMatchDetail(
          DesktopStatus.stopped,
          const DesktopChip(DesktopChipKind.tag, '업무'),
        ),
        "'꺼짐' · '업무' 조건 안에서 찾았어요.",
      );
    });

    test('older stored chips 실행 중 / 꺼짐 migrate to the status', () {
      final migrated = const OrganizePreference(
        chip: 'running',
        sort: 'name',
      ).migrated();
      expect(
        (migrated.chip, migrated.status, migrated.sort),
        ('all', 'running', 'name'),
      );
      final stopped = const OrganizePreference(chip: 'stopped').migrated();
      expect((stopped.chip, stopped.status), ('all', 'stopped'));
      // A newer pair is left alone; an unknown status means 전체.
      final both = const OrganizePreference(
        chip: 'tag:dev',
        status: 'stopped',
      ).migrated();
      expect((both.chip, both.status), ('tag:dev', 'stopped'));
      expect(const OrganizePreference(status: '??').migrated().status, 'all');
    });
  });

  group('sort', () {
    final list = [
      machine('pve-10', 'VM 10', used: DateTime.utc(2026, 10, 1)),
      machine(
        'pve-2',
        'vm 2',
        power: 'stopped',
        used: DateTime.utc(2026, 10, 3),
      ),
      machine('pve-3', 'Alpha'),
      machine('pve-4', '가나다', used: DateTime.utc(2026, 10, 2)),
    ];

    test('번호순 keeps the broker order (numeric VM IDs)', () {
      expect(ids(organizeDesktops(list)), [
        'pve-10',
        'pve-2',
        'pve-3',
        'pve-4',
      ]);
    });

    test('이름순 is case-insensitive and reads numbers by value', () {
      expect(ids(organizeDesktops(list, sort: DesktopSort.name)), [
        'pve-3',
        'pve-2',
        'pve-10',
        'pve-4',
      ]);
    });

    test('최근 사용순 puts the latest first and never-used last by number', () {
      expect(ids(organizeDesktops(list, sort: DesktopSort.recent)), [
        'pve-2',
        'pve-4',
        'pve-10',
        'pve-3',
      ]);
    });

    test('실행 중 먼저 keeps number order inside each group', () {
      expect(ids(organizeDesktops(list, sort: DesktopSort.running)), [
        'pve-10',
        'pve-3',
        'pve-4',
        'pve-2',
      ]);
    });

    test('favourites come first within every sort', () {
      final starred = [
        machine('pve-10', 'VM 10', favorite: false),
        machine('pve-2', 'vm 2', favorite: true, power: 'stopped'),
        machine('pve-3', 'Alpha', favorite: false),
        machine('pve-4', 'Zed', favorite: true),
      ];
      expect(ids(organizeDesktops(starred)), [
        'pve-2',
        'pve-4',
        'pve-10',
        'pve-3',
      ]);
      expect(ids(organizeDesktops(starred, sort: DesktopSort.name)), [
        'pve-2',
        'pve-4',
        'pve-3',
        'pve-10',
      ]);
      expect(ids(organizeDesktops(starred, sort: DesktopSort.running)), [
        'pve-4',
        'pve-2',
        'pve-10',
        'pve-3',
      ]);
    });

    test('sort names round-trip', () {
      for (final sort in DesktopSort.values) {
        expect(DesktopSort.byName(sort.name), sort);
      }
      expect(DesktopSort.byName('zzz'), DesktopSort.number);
      expect(DesktopSort.byName(null), DesktopSort.number);
    });
  });

  group('broker fields', () {
    Map<String, dynamic> json(Map<String, dynamic> extra) => {
      'id': 'pve-149',
      'name': 'TESTER',
      'kind': 'vm',
      'description': '',
      'os': 'Windows 11',
      'status': 'online',
      'canConnect': true,
      ...extra,
    };

    test('an older broker sends none of them', () {
      final desktop = BrokerDesktop.fromJson(json({}));
      final machine = machineFromBroker(desktop);
      expect(machine.organize.supported, false);
      expect(machine.organize.favorite, false);
      expect(machine.organize.tags, isEmpty);
      expect(machine.organize.pool, isNull);
      expect(machine.organize.lastUsedAt, isNull);
    });

    test('a current broker sends tags, pool, favorite and lastUsedAt', () {
      final machine = machineFromBroker(
        BrokerDesktop.fromJson(
          json({
            'tags': ['업무', ' dev ', '', 5, '업무'],
            'pool': '영업팀',
            'favorite': true,
            'lastUsedAt': '2026-10-26T12:00:00.000Z',
          }),
        ),
      );
      expect(machine.organize.supported, true);
      expect(machine.organize.favorite, true);
      expect(machine.organize.tags, ['업무', 'dev']);
      expect(machine.organize.pool, '영업팀');
      expect(machine.organize.lastUsedAt, DateTime.utc(2026, 10, 26, 12));
    });

    test('malformed values are ignored, never thrown', () {
      final machine = machineFromBroker(
        BrokerDesktop.fromJson(
          json({'tags': 'x', 'pool': 3, 'favorite': 'yes', 'lastUsedAt': 'no'}),
        ),
      );
      expect(machine.organize.supported, false);
      expect(machine.organize.tags, isEmpty);
      expect(machine.organize.pool, isNull);
      expect(machine.organize.lastUsedAt, isNull);
    });
  });

  test('chip and sort are kept per account, not the search text', () {
    final base = const LauncherPreferences(lastDesktop: 'pve-1');
    final next = base.withOrganize(
      'tester',
      const OrganizePreference(
        chip: 'tag:dev',
        status: 'stopped',
        sort: 'name',
      ),
    );
    expect(next.lastDesktop, 'pve-1');
    expect(next.organizeFor('tester').chip, 'tag:dev');
    expect(next.organizeFor('tester').status, 'stopped');
    expect(next.organizeFor('someone').status, 'all');
    expect(next.organizeFor('someone').chip, 'all');
    expect(next.organizeFor('someone').sort, 'number');
  });

  group('launcher file', () {
    late Directory dir;
    setUp(() => dir = Directory.systemTemp.createTempSync('totb-organize'));
    tearDown(() => dir.deleteSync(recursive: true));

    test('the status round-trips and an older file reads as 전체', () async {
      final file = File('${dir.path}/launcher.json');
      final memory = FileLauncherMemory(file);
      await memory.save(
        'https://s',
        const LauncherPreferences().withOrganize(
          'tester',
          const OrganizePreference(
            chip: 'tag:dev',
            status: 'running',
            sort: 'name',
          ),
        ),
      );
      final back = (await memory.load('https://s')).organizeFor('tester');
      expect(
        (back.chip, back.status, back.sort),
        ('tag:dev', 'running', 'name'),
      );
      // A file written before the status existed, with the old chips.
      await file.writeAsString(
        '{"https://s":{"organize":{"old":{"chip":"stopped","sort":"recent"},'
        '"older":{"chip":"pool:A","sort":"name"},'
        '"bad":{"chip":"all","status":"zzz","sort":"number"}}}}',
      );
      final old = await memory.load('https://s');
      expect(
        (old.organizeFor('old').chip, old.organizeFor('old').status),
        ('all', 'stopped'),
      );
      expect(
        (old.organizeFor('older').chip, old.organizeFor('older').status),
        ('pool:A', 'all'),
      );
      expect(old.organizeFor('bad').status, 'all');
      // A file from before the 구분 filter, or an unknown kind, reads as 전체.
      expect(old.organizeFor('old').kind, 'all');
      await memory.save(
        'https://s',
        const LauncherPreferences().withOrganize(
          'tester',
          const OrganizePreference(kind: 'pc'),
        ),
      );
      expect((await memory.load('https://s')).organizeFor('tester').kind, 'pc');
      await file.writeAsString(
        '{"https://s":{"organize":{"t":{"chip":"all","kind":"zzz"}}}}',
      );
      expect((await memory.load('https://s')).organizeFor('t').kind, 'all');
    });
  });

  group('구분 (kind)', () {
    DesktopMachine machine(String id, MachineKind kind) => DesktopMachine(
      id: id,
      name: id,
      description: '',
      kind: kind,
      status: MachineStatus.ready,
      specification: 'Windows 11',
      accent: Colors.blue,
    );
    final mixed = [
      machine('pve-1', MachineKind.vm),
      machine('pc-1', MachineKind.pc),
      machine('pve-2', MachineKind.vm),
    ];

    test('admits by kind and counts', () {
      expect(DesktopKind.vm.count(mixed), 2);
      expect(DesktopKind.pc.count(mixed), 1);
      expect(DesktopKind.all.count(mixed), 3);
      expect(organizeDesktops(mixed, kind: DesktopKind.pc).map((m) => m.id), [
        'pc-1',
      ]);
      expect(organizeDesktops(mixed, kind: DesktopKind.vm).map((m) => m.id), [
        'pve-1',
        'pve-2',
      ]);
      expect(DesktopKind.byId('pc'), DesktopKind.pc);
      expect(DesktopKind.byId('nope'), DesktopKind.all);
    });

    test('useful only with both kinds; otherwise it counts as 전체', () {
      expect(kindFilterUseful(mixed), isTrue);
      final vms = [mixed[0], mixed[2]];
      expect(kindFilterUseful(vms), isFalse);
      expect(effectiveKind(DesktopKind.pc, vms), DesktopKind.all);
      expect(effectiveKind(DesktopKind.vm, vms), DesktopKind.all);
      expect(effectiveKind(DesktopKind.pc, mixed), DesktopKind.pc);
    });

    test('the empty result names it with the other conditions', () {
      expect(
        noMatchDetail(
          DesktopStatus.stopped,
          DesktopChip.all,
          kind: DesktopKind.pc,
        ),
        "'로컬 PC' · '꺼짐' 조건 안에서 찾았어요.",
      );
    });
  });
}
