import 'desktop_machine.dart';
import '../l10n/l10n.dart';

/// How the broker files one assigned desktop (Proxmox tags and pool, this
/// account's favourite, when this account last connected). All absent from a
/// broker before desktop organising; [supported] then stays false and the
/// launcher behaves as it always did.
class DesktopOrganize {
  const DesktopOrganize({
    this.tags = const [],
    this.pool,
    this.favorite = false,
    this.lastUsedAt,
    this.supported = false,
  });

  final List<String> tags;
  final String? pool;
  final bool favorite;
  final DateTime? lastUsedAt;

  /// The broker reports favourites (the `favorite` field is present), so the
  /// star and the favourite menu item are offered.
  final bool supported;

  static const none = DesktopOrganize();

  factory DesktopOrganize.fromJson(Map<String, dynamic> data) {
    final rawTags = data['tags'];
    final tags = <String>[];
    if (rawTags is List) {
      for (final tag in rawTags) {
        if (tag is! String) continue;
        final clean = tag.trim();
        if (clean.isEmpty || clean.length > 64 || tags.contains(clean)) {
          continue;
        }
        tags.add(clean);
        if (tags.length >= 32) break;
      }
    }
    final rawPool = data['pool'];
    final pool = rawPool is String && rawPool.trim().isNotEmpty
        ? (rawPool.trim().length > 64
              ? rawPool.trim().substring(0, 64)
              : rawPool.trim())
        : null;
    final used = data['lastUsedAt'];
    return DesktopOrganize(
      tags: List.unmodifiable(tags),
      pool: pool,
      favorite: data['favorite'] == true,
      lastUsedAt: used is String ? DateTime.tryParse(used)?.toUtc() : null,
      supported: data['favorite'] is bool,
    );
  }

  DesktopOrganize withFavorite(bool value) => DesktopOrganize(
    tags: tags,
    pool: pool,
    favorite: value,
    lastUsedAt: lastUsedAt,
    supported: supported,
  );
}

enum DesktopSort {
  number,
  name,
  recent,
  running;

  const DesktopSort();
  String get label => switch (this) {
    DesktopSort.number => l10n.number,
    DesktopSort.name => l10n.name,
    DesktopSort.recent => l10n.recent,
    DesktopSort.running => l10n.runningFirst,
  };

  static DesktopSort byName(String? name) => DesktopSort.values.firstWhere(
    (value) => value.name == name,
    orElse: () => DesktopSort.number,
  );
}

/// 분류 chips file desktops by what they are; [all] stands for "no chip".
/// (실행 중 / 꺼짐 used to be chips; they are the [DesktopStatus] filter now.)
enum DesktopChipKind { all, favorites, tag, pool }

/// One 분류 chip. [id] is what is remembered ('all', 'favorites',
/// 'tag:NAME', 'pool:NAME').
class DesktopChip {
  const DesktopChip(this.kind, [this.name = '']);
  final DesktopChipKind kind;
  final String name;

  static const all = DesktopChip(DesktopChipKind.all);

  String get id => switch (kind) {
    DesktopChipKind.all => 'all',
    DesktopChipKind.favorites => 'favorites',
    DesktopChipKind.tag => 'tag:$name',
    DesktopChipKind.pool => 'pool:$name',
  };

  String get label => switch (kind) {
    DesktopChipKind.all => l10n.all,
    DesktopChipKind.favorites => l10n.favorites,
    _ => name,
  };

  static DesktopChip parse(String? id) {
    if (id == null) return all;
    if (id.startsWith('tag:') && id.length > 4) {
      return DesktopChip(DesktopChipKind.tag, id.substring(4));
    }
    if (id.startsWith('pool:') && id.length > 5) {
      return DesktopChip(DesktopChipKind.pool, id.substring(5));
    }
    return switch (id) {
      'favorites' => const DesktopChip(DesktopChipKind.favorites),
      _ => all,
    };
  }

  @override
  bool operator ==(Object other) =>
      other is DesktopChip && other.kind == kind && other.name == name;
  @override
  int get hashCode => Object.hash(kind, name);

  bool admits(DesktopMachine machine) => switch (kind) {
    DesktopChipKind.all => true,
    DesktopChipKind.favorites => machine.organize.favorite,
    DesktopChipKind.tag => machine.organize.tags.contains(name),
    DesktopChipKind.pool => machine.organize.pool == name,
  };
}

/// The 상태 filter beside the search: 전체 · 실행 중 · 꺼짐. It is independent
/// of the 분류 chip; both AND with each other and with the search.
enum DesktopStatus {
  all,
  running,
  stopped;

  const DesktopStatus();
  String get label => switch (this) {
    DesktopStatus.all => l10n.all,
    DesktopStatus.running => l10n.running,
    DesktopStatus.stopped => l10n.off,
  };

  /// What is remembered ('all', 'running', 'stopped').
  String get id => name;

  static DesktopStatus byId(String? id) => DesktopStatus.values.firstWhere(
    (value) => value.name == id,
    orElse: () => DesktopStatus.all,
  );

  bool admits(DesktopMachine machine) => switch (this) {
    DesktopStatus.all => true,
    DesktopStatus.running => isRunning(machine),
    DesktopStatus.stopped => isStopped(machine),
  };

  /// What the segment's tooltip says while it can be chosen.
  String get action => switch (this) {
    DesktopStatus.all => l10n.showAllDesktops,
    DesktopStatus.running => l10n.showRunningDesktopsOnly,
    DesktopStatus.stopped => l10n.showOffDesktopsOnly,
  };

  /// Why the segment cannot be chosen while it has no members.
  String get emptyReason => switch (this) {
    DesktopStatus.all => l10n.noDesktopsAssigned,
    DesktopStatus.running => l10n.noRunningDesktops,
    DesktopStatus.stopped => l10n.noDesktopsOff,
  };
}

/// How many desktops each [DesktopStatus] holds (a stale or unknown power
/// state counts only toward 전체).
class DesktopStatusCounts {
  const DesktopStatusCounts(this.all, this.running, this.stopped);
  final int all, running, stopped;

  factory DesktopStatusCounts.of(List<DesktopMachine> machines) =>
      DesktopStatusCounts(
        machines.length,
        machines.where(isRunning).length,
        machines.where(isStopped).length,
      );

  int of(DesktopStatus status) => switch (status) {
    DesktopStatus.all => all,
    DesktopStatus.running => running,
    DesktopStatus.stopped => stopped,
  };
}

/// The 상태 filter that applies now: a remembered or selected one that no
/// longer has members counts as 전체.
DesktopStatus effectiveStatus(
  DesktopStatus status,
  List<DesktopMachine> machines,
) =>
    status == DesktopStatus.all ||
        DesktopStatusCounts.of(machines).of(status) == 0
    ? DesktopStatus.all
    : status;

/// The 구분 filter beside the 상태 filter: 전체 · VM · 로컬 PC.
/// Shown only while the list holds both kinds; it ANDs with the 상태 filter,
/// the 분류 chip and the search.
enum DesktopKind {
  all,
  vm,
  pc;

  const DesktopKind();
  String get label => switch (this) {
    DesktopKind.all => l10n.all,
    DesktopKind.vm => 'VM',
    DesktopKind.pc => l10n.localPc,
  };

  /// What is remembered ('all', 'vm', 'pc').
  String get id => name;

  static DesktopKind byId(String? id) => DesktopKind.values.firstWhere(
    (value) => value.name == id,
    orElse: () => DesktopKind.all,
  );

  bool admits(DesktopMachine machine) => switch (this) {
    DesktopKind.all => true,
    DesktopKind.vm => machine.kind == MachineKind.vm,
    DesktopKind.pc => machine.kind == MachineKind.pc,
  };

  int count(List<DesktopMachine> machines) =>
      this == DesktopKind.all ? machines.length : machines.where(admits).length;

  String get action => switch (this) {
    DesktopKind.all => l10n.showAllDesktops,
    DesktopKind.vm => l10n.showVmsOnly,
    DesktopKind.pc => l10n.showLocalPcsOnly,
  };

  String get emptyReason => switch (this) {
    DesktopKind.all => l10n.noDesktopsAssigned,
    DesktopKind.vm => l10n.noVmsAssigned,
    DesktopKind.pc => l10n.noLocalPcsAssigned,
  };
}

/// What one filter counts: the
/// machines the other filters leave, so with 로컬 PC chosen the 상태 numbers
/// describe local PCs only, and the reverse.
List<DesktopMachine> facetMachines(
  List<DesktopMachine> machines, {
  DesktopStatus status = DesktopStatus.all,
  DesktopKind kind = DesktopKind.all,
  DesktopChip? chip,
}) => [
  for (final machine in machines)
    if (status.admits(machine) &&
        kind.admits(machine) &&
        (chip?.admits(machine) ?? true))
      machine,
];

/// The 구분 filter is shown only while both kinds are assigned.
bool kindFilterUseful(List<DesktopMachine> machines) =>
    DesktopKind.vm.count(machines) > 0 && DesktopKind.pc.count(machines) > 0;

/// The 구분 filter that applies now: without both kinds (the filter is not
/// shown) or without members it counts as 전체.
DesktopKind effectiveKind(DesktopKind kind, List<DesktopMachine> machines) =>
    kind == DesktopKind.all ||
        !kindFilterUseful(machines) ||
        kind.count(machines) == 0
    ? DesktopKind.all
    : kind;

/// Powered off as the tile shows it (a stale or unknown state is neither).
bool isStopped(DesktopMachine machine) =>
    machine.status == MachineStatus.poweredOff;

bool isRunning(DesktopMachine machine) =>
    !isStopped(machine) &&
    (machine.powerState == 'running' || machine.status == MachineStatus.ready);

/// The 분류 chips for [machines], each with its member count: 즐겨찾기, pools
/// and tags, only while they have members (no chip at all means no row). Tags
/// and pools sort alphabetically (case-insensitive, numbers by value).
List<(DesktopChip, int)> chipsFor(List<DesktopMachine> machines) {
  int count(DesktopChip chip) => machines.where(chip.admits).length;
  final tags = <String>{}, pools = <String>{};
  for (final machine in machines) {
    tags.addAll(machine.organize.tags);
    final pool = machine.organize.pool;
    if (pool != null) pools.add(pool);
  }
  final result = <(DesktopChip, int)>[];
  void add(DesktopChip chip) {
    final members = count(chip);
    if (members > 0) result.add((chip, members));
  }

  add(const DesktopChip(DesktopChipKind.favorites));
  for (final pool in pools.toList()..sort(naturalCompare)) {
    add(DesktopChip(DesktopChipKind.pool, pool));
  }
  for (final tag in tags.toList()..sort(naturalCompare)) {
    add(DesktopChip(DesktopChipKind.tag, tag));
  }
  return result;
}

/// Whether the sort button and the 분류 chips are worth showing: more than
/// three desktops, or any tag, pool or favourite to filter by. (The 상태
/// filter and the search are always there.)
bool organizeUseful(List<DesktopMachine> machines) =>
    machines.length > 3 ||
    machines.any(
      (m) =>
          m.organize.tags.isNotEmpty ||
          m.organize.pool != null ||
          m.organize.favorite,
    );

/// The chip that applies now: a remembered or selected one that no longer has
/// members counts as 전체.
DesktopChip effectiveChip(DesktopChip chip, List<DesktopMachine> machines) =>
    chip == DesktopChip.all ||
        !chipsFor(machines).any((entry) => entry.$1 == chip)
    ? DesktopChip.all
    : chip;

/// The empty-result texts shared by Windows and Android.
String noMatchTitle(String query) {
  final text = query.trim();
  if (text.isEmpty) return l10n.noDesktopsMatch;
  final short = text.length > 24 ? '${text.substring(0, 24)}…' : text;
  return l10n.noDesktopsMatch2(short);
}

String noMatchDetail(
  DesktopStatus status,
  DesktopChip chip, {
  DesktopKind kind = DesktopKind.all,
}) {
  final parts = [
    if (kind != DesktopKind.all) kind.label,
    if (status != DesktopStatus.all) status.label,
    if (chip != DesktopChip.all) chip.label,
  ];
  return parts.isEmpty
      ? l10n.searchByNameNumberGroup
      : l10n.searchedWithin(parts.map((part) => "'$part'").join(' · '));
}

/// The visible list: kind AND status AND chip AND search, sorted, favourites
/// first.
List<DesktopMachine> organizeDesktops(
  List<DesktopMachine> machines, {
  String query = '',
  DesktopChip chip = DesktopChip.all,
  DesktopStatus status = DesktopStatus.all,
  DesktopKind kind = DesktopKind.all,
  DesktopSort sort = DesktopSort.number,
}) {
  final indexed = [for (var i = 0; i < machines.length; i++) (i, machines[i])];
  final matcher = DesktopQuery(query);
  final kept = [
    for (final entry in indexed)
      if (kind.admits(entry.$2) &&
          status.admits(entry.$2) &&
          chip.admits(entry.$2) &&
          matcher.matches(entry.$2))
        entry,
  ];
  int byIndex((int, DesktopMachine) a, (int, DesktopMachine) b) =>
      a.$1.compareTo(b.$1);
  int order((int, DesktopMachine) a, (int, DesktopMachine) b) {
    final favorite = (b.$2.organize.favorite ? 1 : 0).compareTo(
      a.$2.organize.favorite ? 1 : 0,
    );
    if (favorite != 0) return favorite;
    final inner = switch (sort) {
      DesktopSort.number => 0,
      DesktopSort.name => naturalCompare(a.$2.name, b.$2.name),
      DesktopSort.recent => _recent(a.$2, b.$2),
      DesktopSort.running => (isRunning(b.$2) ? 1 : 0).compareTo(
        isRunning(a.$2) ? 1 : 0,
      ),
    };
    return inner != 0 ? inner : byIndex(a, b);
  }

  kept.sort(order);
  return [for (final entry in kept) entry.$2];
}

int _recent(DesktopMachine a, DesktopMachine b) {
  final x = a.organize.lastUsedAt, y = b.organize.lastUsedAt;
  if (x == null && y == null) return 0;
  if (x == null) return 1;
  if (y == null) return -1;
  return y.compareTo(x);
}

/// Case-insensitive comparison that reads digit runs as numbers ("VM 2"
/// before "VM 10").
int naturalCompare(String a, String b) {
  final x = a.toLowerCase(), y = b.toLowerCase();
  var i = 0, j = 0;
  bool digit(int c) => c >= 0x30 && c <= 0x39;
  while (i < x.length && j < y.length) {
    final c = x.codeUnitAt(i), d = y.codeUnitAt(j);
    if (digit(c) && digit(d)) {
      var ie = i, je = j;
      while (ie < x.length && digit(x.codeUnitAt(ie))) {
        ie++;
      }
      while (je < y.length && digit(y.codeUnitAt(je))) {
        je++;
      }
      final left = x.substring(i, ie).replaceFirst(RegExp(r'^0+(?=.)'), '');
      final right = y.substring(j, je).replaceFirst(RegExp(r'^0+(?=.)'), '');
      if (left.length != right.length) {
        return left.length.compareTo(right.length);
      }
      final cmp = left.compareTo(right);
      if (cmp != 0) return cmp;
      i = ie;
      j = je;
    } else {
      if (c != d) return c.compareTo(d);
      i++;
      j++;
    }
  }
  return (x.length - i).compareTo(y.length - j);
}

// --- search ---------------------------------------------------------------

/// A parsed search box entry. Every space-separated word must match
/// somewhere in the desktop's visible text (name, VM ID as "149" or "#149",
/// OS label, description, tags and pool). Korean initial consonants stand for
/// any syllable that starts with them ("ㅅㅁㅅ" finds "사무실"), mixed with
/// full syllables ("사ㅁ실"); loose jamo typed one by one ("ㅅㅏ") compose into
/// syllables first.
class DesktopQuery {
  DesktopQuery(String raw) : words = _words(raw);
  final List<String> words;

  bool get isEmpty => words.isEmpty;

  static List<String> _words(String raw) {
    final composed = composeJamo(raw.toLowerCase());
    return [
      for (final word in composed.split(RegExp(r'\s+')))
        if (word.replaceAll('#', '').isNotEmpty)
          word.startsWith('#') && word.length > 1 ? word.substring(1) : word,
    ];
  }

  bool matches(DesktopMachine machine) {
    if (words.isEmpty) return true;
    final fields = searchFields(machine);
    return words.every((word) => fields.any((field) => _inField(field, word)));
  }

  /// The visible text a search looks in, one entry per field (a word never
  /// spans two fields).
  static List<String> searchFields(DesktopMachine machine) {
    final digits = RegExp(r'(\d+)$').firstMatch(machine.id)?.group(1);
    return [
      for (final part in <String?>[
        machine.name,
        machine.id,
        digits,
        machine.specification,
        machine.description,
        ...machine.organize.tags,
        machine.organize.pool,
      ])
        if (part != null && part.isNotEmpty) part.toLowerCase(),
    ];
  }

  // Spaces inside a field do not break a word ("ㅁㅅㄷㅅㅋㅌ" finds
  // "민수 데스크톱").
  static bool _inField(String field, String word) =>
      _contains(field, word) ||
      _contains(field.replaceAll(RegExp(r'\s+'), ''), word);
  static bool _contains(String text, String word) {
    if (word.isEmpty) return true;
    final t = text.runes.toList(), w = word.runes.toList();
    for (var start = 0; start + w.length <= t.length; start++) {
      var ok = true;
      for (var k = 0; k < w.length; k++) {
        if (!_same(w[k], t[start + k])) {
          ok = false;
          break;
        }
      }
      if (ok) return true;
    }
    return false;
  }

  /// [query] equals [text], or is an initial consonant of the syllable.
  static bool _same(int query, int text) {
    if (query == text) return true;
    if (_isInitialJamo(query) && _isSyllable(text)) {
      return _initials[(text - 0xAC00) ~/ 588] == query;
    }
    return false;
  }
}

// Compatibility jamo (U+3131..) in the order Hangul syllables use.
const _initials = [
  0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, //
  0x3145, 0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D,
  0x314E,
];
const _vowels = [
  0x314F, 0x3150, 0x3151, 0x3152, 0x3153, 0x3154, 0x3155, 0x3156, 0x3157, //
  0x3158, 0x3159, 0x315A, 0x315B, 0x315C, 0x315D, 0x315E, 0x315F, 0x3160,
  0x3161, 0x3162, 0x3163,
];
// Index 0 is "no final".
const _finals = [
  0, 0x3131, 0x3132, 0x3133, 0x3134, 0x3135, 0x3136, 0x3137, 0x3139, 0x313A, //
  0x313B, 0x313C, 0x313D, 0x313E, 0x313F, 0x3140, 0x3141, 0x3142, 0x3144,
  0x3145, 0x3146, 0x3147, 0x3148, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E,
];

bool _isSyllable(int c) => c >= 0xAC00 && c <= 0xD7A3;
bool _isInitialJamo(int c) => _initials.contains(c);
bool _isVowelJamo(int c) => c >= 0x314F && c <= 0x3163;

/// Combines loose compatibility jamo into syllables (ㅅ+ㅏ+ㅁ → 삼) and maps
/// the conjoining initial jamo (U+1100..) to their compatibility forms. A
/// consonant that is not followed by a vowel stays an initial-consonant
/// search key.
String composeJamo(String input) {
  final runes = input.runes.map((c) {
    if (c >= 0x1100 && c <= 0x1112) return _initials[c - 0x1100];
    return c;
  }).toList();
  final out = <int>[];
  var i = 0;
  while (i < runes.length) {
    final c = runes[i];
    if (_isInitialJamo(c) &&
        i + 1 < runes.length &&
        _isVowelJamo(runes[i + 1])) {
      final initial = _initials.indexOf(c);
      final vowel = _vowels.indexOf(runes[i + 1]);
      var final_ = 0, used = 2;
      if (i + 2 < runes.length) {
        final candidate = _finals.indexOf(runes[i + 2]);
        // A final only when the next jamo is not the start of a new
        // syllable (a vowel follows it).
        final next = i + 3 < runes.length ? runes[i + 3] : null;
        if (candidate > 0 && (next == null || !_isVowelJamo(next))) {
          final_ = candidate;
          used = 3;
        }
      }
      out.add(0xAC00 + (initial * 21 + vowel) * 28 + final_);
      i += used;
    } else {
      // A cluster typed alone (ㄳ) is two initial-consonant keys (ㄱㅅ).
      out.addAll(_clusters[c] ?? [c]);
      i++;
    }
  }
  return String.fromCharCodes(out);
}

const _clusters = {
  0x3133: [0x3131, 0x3145],
  0x3135: [0x3134, 0x3148],
  0x3136: [0x3134, 0x314E],
  0x313A: [0x3139, 0x3131],
  0x313B: [0x3139, 0x3141],
  0x313C: [0x3139, 0x3142],
  0x313D: [0x3139, 0x3145],
  0x313E: [0x3139, 0x314C],
  0x313F: [0x3139, 0x314D],
  0x3140: [0x3139, 0x314E],
  0x3144: [0x3142, 0x3145],
};
