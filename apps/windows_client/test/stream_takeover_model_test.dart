import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/stream_takeover.dart';
import 'package:multitap_client/l10n/l10n.dart';

Map<String, Object?> _offer() => {
  'protocol': streamTakeoverProtocol,
  'context': 'C' * 43,
  'expiresInSeconds': 120,
  'desktop': <String, Object?>{'id': 'pve-170', 'name': 'WorkPC'},
  'sessions': [
    {
      'username': 'alice',
      'createdAt': '2026-11-06T03:00:00.000Z',
      'deviceLabel': null,
    },
  ],
};

void main() {
  test(
    'offer binds actual desktop/account context and bounded local lifetime',
    () {
      final now = DateTime.utc(2026, 11, 6, 3);
      final raw = _offer();
      final offer = StreamTakeoverOffer.parse(
        raw,
        expectedDesktopId: 'pve-170',
        receivedAt: now,
      );
      expect(offer.desktopId, 'pve-170');
      expect(offer.desktopName, 'WorkPC');
      expect(offer.sessions.single.username, 'alice');
      expect(offer.sessions.single.createdAt, now);
      expect(offer.sessions.single.deviceLabel, isNull);
      expect(offer.currentAt(now), isTrue);
      expect(offer.currentAt(now.add(const Duration(seconds: 119))), isTrue);
      expect(offer.currentAt(now.add(const Duration(seconds: 120))), isFalse);
      expect(
        offer.currentAt(now.subtract(const Duration(seconds: 1))),
        isFalse,
      );
      expect(() => offer.sessions.clear(), throwsUnsupportedError);
      expect(
        () => StreamTakeoverOffer.parse(raw, expectedDesktopId: 'pve-171'),
        throwsFormatException,
      );
    },
  );

  test(
    'unknown legacy start time stays unknown and never becomes request time',
    () {
      final raw = _offer();
      ((raw['sessions']! as List).single as Map)['createdAt'] = null;
      final offer = StreamTakeoverOffer.parse(raw);
      expect(offer.sessions.single.createdAt, isNull);
      expect(offer.sessions.single.deviceLabel, isNull);
    },
  );

  test('the card lines name each account with its local start time, and '
      'never invent an unknown one', () {
    final raw = _offer();
    raw['sessions'] = [
      for (final (name, at) in [
        ('kim.minji', DateTime(2026, 11, 11, 10, 20)),
        ('lee.jun', DateTime(2026, 1, 5, 7, 3)),
        ('legacy', null),
      ])
        {
          'username': name,
          'createdAt': at?.toUtc().toIso8601String(),
          'deviceLabel': null,
        },
    ];
    // The date by locale (docs/i18n.md; review 2026-11-12: it read
    // "11-11 10:20부터" / "since 11-11 10:20").
    expect(streamTakeoverSessionLines(StreamTakeoverOffer.parse(raw)), [
      '연결 중인 계정 kim.minji · 11월 11일 10:20부터',
      '연결 중인 계정 lee.jun · 1월 5일 07:03부터',
      '연결 중인 계정 legacy',
    ]);
    AppLanguageController.instance.debugReset(AppLanguage.en);
    expect(streamTakeoverSessionLines(StreamTakeoverOffer.parse(raw)), [
      'Connected account kim.minji · since Nov 11, 10:20',
      'Connected account lee.jun · since Jan 5, 07:03',
      'Connected account legacy',
    ]);
  });

  test('bounded non-BMP names use Unicode scalar counts like the broker', () {
    final raw = _offer();
    ((raw['sessions']! as List).single as Map)['username'] =
        String.fromCharCode(0x10400) * 64;
    expect(
      StreamTakeoverOffer.parse(raw).sessions.single.username.runes.length,
      64,
    );
  });

  final invalid = <String, void Function(Map<String, Object?>)>{
    'wrong protocol': (v) => v['protocol'] = 'legacy',
    'bad opaque context': (v) => v['context'] = 'short',
    'oversized TTL': (v) => v['expiresInSeconds'] = 121,
    'expired TTL': (v) => v['expiresInSeconds'] = 0,
    'fraction TTL': (v) => v['expiresInSeconds'] = 1.5,
    'extra top field': (v) => v['force'] = true,
    'empty sessions': (v) => v['sessions'] = [],
    'too many sessions': (v) => v['sessions'] = List.filled(6, {
      'username': 'alice',
      'createdAt': null,
      'deviceLabel': null,
    }),
    'wrong desktop shape': (v) => (v['desktop']! as Map)['extra'] = true,
    'control name': (v) => (v['desktop']! as Map)['name'] = 'PC\nname',
    'unsafe id': (v) => (v['desktop']! as Map)['id'] = '../other',
    'bidi account': (v) =>
        ((v['sessions']! as List).single as Map)['username'] = 'alice\u202ebob',
    'blank account': (v) =>
        ((v['sessions']! as List).single as Map)['username'] = '  ',
    'extra session field': (v) =>
        ((v['sessions']! as List).single as Map)['ip'] = 'synthetic',
    'unobserved device label': (v) =>
        ((v['sessions']! as List).single as Map)['deviceLabel'] = 'guessed PC',
    'invalid date': (v) =>
        ((v['sessions']! as List).single as Map)['createdAt'] =
            '2026-02-30T03:00:00.000Z',
    'non UTC date': (v) =>
        ((v['sessions']! as List).single as Map)['createdAt'] =
            '2026-11-06T03:00:00.000+09:00',
    'lone surrogate': (v) =>
        ((v['sessions']! as List).single as Map)['username'] =
            String.fromCharCode(0xd800),
  };
  for (final entry in invalid.entries) {
    test('offer rejects ${entry.key}', () {
      final raw = _offer();
      entry.value(raw);
      expect(() => StreamTakeoverOffer.parse(raw), throwsFormatException);
    });
  }
}
