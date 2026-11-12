import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/session_access.dart';

final contextOrigin = Uri.parse('https://login-context.fixture.invalid');
final contextNow = DateTime.utc(2026, 11, 6, 5);
Map<String, Object?> loginContextFixture() => {
  'username': 'alice',
  'sessionCount': 1,
  'sessions': [
    <String, Object?>{
      'issuer': 'windows-pc-v1',
      'createdAt': '2026-11-06T04:30:00.000Z',
      'desktops': [
        {'id': 'pve-149', 'name': 'TESTER'},
      ],
    },
  ],
};
Map<String, Object?> loginChallengeFixture({
  Object? context,
  bool includeContext = true,
}) => {
  'protocol': sessionAccessProtocol,
  'challengeToken': 'synthetic_context_challenge_00001',
  'expiresInMs': 120000,
  if (includeContext) 'existingLogin': context ?? loginContextFixture(),
};

void main() {
  test('Unicode account and desktop limits count codepoints without splitting valid surrogate pairs', () {
    final context = loginContextFixture()..['username'] = '𐐨' * 64;
    ((context['sessions'] as List).single as Map)['desktops'] = [
      {'id': 'pve-149', 'name': '🖥' * 128},
    ];
    final dynamic challenge = LoginConfirmation.parse(
      loginChallengeFixture(context: context),
      contextOrigin,
      contextNow,
    );
    expect(challenge.existingLogin.username.runes.length, 64);
    expect(
      challenge.existingLogin.sessions.single.desktops.single.name.runes.length,
      128,
    );
    context['username'] = '𐐨' * 65;
    expect(
      () => LoginConfirmation.parse(
        loginChallengeFixture(context: context),
        contextOrigin,
        contextNow,
      ),
      throwsFormatException,
    );
  });
  test('new negotiated context is display-only and keeps exact token origin expiry and one use', () {
    final wire = loginChallengeFixture();
    final dynamic challenge = LoginConfirmation.parse(
      wire,
      contextOrigin,
      contextNow,
    );
    expect(challenge.existingLogin.username, 'alice');
    expect(challenge.existingLogin.sessionCount, 1);
    expect(challenge.existingLogin.sessions.single.issuer, 'windows-pc-v1');
    expect(
      challenge.existingLogin.sessions.single.createdAt,
      DateTime.utc(2026, 11, 6, 4, 30),
    );
    expect(
      challenge.existingLogin.sessions.single.desktops.single.id,
      'pve-149',
    );
    expect(
      challenge.existingLogin.sessions.single.desktops.single.name,
      'TESTER',
    );
    expect(challenge.expiresAt, contextNow.add(const Duration(minutes: 2)));
    expect(
      challenge.consume(contextOrigin, contextNow),
      'synthetic_context_challenge_00001',
    );
    expect(challenge.forgotten, isTrue);
    expect(
      () => challenge.consume(contextOrigin, contextNow),
      throwsFormatException,
    );
  });

  test(
    'legacy three-field challenge remains accepted with no invented context',
    () {
      final dynamic challenge = LoginConfirmation.parse(
        loginChallengeFixture(includeContext: false),
        contextOrigin,
        contextNow,
      );
      expect(challenge.existingLogin, isNull);
      expect(
        challenge.consume(contextOrigin, contextNow),
        'synthetic_context_challenge_00001',
      );
    },
  );

  test('context lists are defensively copied and immutable', () {
    final context = loginContextFixture();
    final dynamic challenge = LoginConfirmation.parse(
      loginChallengeFixture(context: context),
      contextOrigin,
      contextNow,
    );
    (context['sessions'] as List).clear();
    expect(challenge.existingLogin.sessions, hasLength(1));
    expect(
      () => (challenge.existingLogin.sessions as List).clear(),
      throwsUnsupportedError,
    );
    expect(
      () => (challenge.existingLogin.sessions.single.desktops as List).clear(),
      throwsUnsupportedError,
    );
  });

  test('existing context cannot override cross-origin and expired consumption fences', () {
    for (final other in [false, true]) {
      final challenge = LoginConfirmation.parse(
        loginChallengeFixture(),
        contextOrigin,
        contextNow,
      );
      expect(
        () => challenge.consume(
          other ? Uri.parse('https://other.fixture.invalid') : contextOrigin,
          other ? contextNow : contextNow.add(const Duration(minutes: 2)),
        ),
        throwsFormatException,
      );
      expect(challenge.forgotten, isTrue);
    }
  });

  test('strict nested metadata rejects unknown fields, controls, malformed dates and inconsistent counts', () {
    final mutations = <void Function(Map<String, Object?>)>[
      (value) => value['password'] = 'must-not-be-displayed',
      (value) => value['username'] = '',
      (value) => value['username'] = 'a' * 65,
      (value) => value['username'] = 'alice\nadmin',
      (value) => value['username'] = 'alice\u0085admin',
      (value) => value['username'] = 'alice\u202eadmin',
      (value) => value['username'] = String.fromCharCode(0xd800),
      (value) => value['sessionCount'] = 0,
      (value) => value['sessionCount'] = 2,
      (value) => value['sessions'] = <Object?>[],
      (value) => value['sessions'] = List.generate(
        6,
        (_) => (loginContextFixture()['sessions'] as List).single,
      ),
      (value) =>
          ((value['sessions'] as List).single as Map)['token'] = 'forbidden',
      (value) => ((value['sessions'] as List).single as Map)['issuer'] =
          'protocol\nspoof',
      (value) =>
          ((value['sessions'] as List).single as Map)['issuer'] = 'a' * 65,
      (value) =>
          ((value['sessions'] as List).single as Map)['createdAt'] = null,
      (value) => ((value['sessions'] as List).single as Map)['createdAt'] =
          '2026-11-06T04:30:00+00:00',
      (value) => ((value['sessions'] as List).single as Map)['createdAt'] =
          '2026-02-30T04:30:00.000Z',
      (value) => ((value['sessions'] as List).single as Map)['desktops'] = [
        {'id': 'bad/id', 'name': 'TESTER'},
      ],
      (value) => ((value['sessions'] as List).single as Map)['desktops'] = [
        {'id': 'a' * 81, 'name': 'TESTER'},
      ],
      (value) => ((value['sessions'] as List).single as Map)['desktops'] = [
        {'id': 'pve-149', 'name': 'a' * 129},
      ],
      (value) => ((value['sessions'] as List).single as Map)['desktops'] = [
        {'id': 'pve-149', 'name': 'TESTER\u2066spoof'},
      ],
      (value) => ((value['sessions'] as List).single as Map)['desktops'] = [
        {'id': 'pve-149', 'name': 'TESTER', 'userToken': 'forbidden'},
      ],
    ];
    for (final mutate in mutations) {
      final context = loginContextFixture();
      mutate(context);
      expect(
        () => LoginConfirmation.parse(
          loginChallengeFixture(context: context),
          contextOrigin,
          contextNow,
        ),
        throwsFormatException,
      );
    }
    for (final invalid in [null, [], 'untyped']) {
      final wire = loginChallengeFixture()..['existingLogin'] = invalid;
      expect(
        () => LoginConfirmation.parse(wire, contextOrigin, contextNow),
        throwsFormatException,
      );
    }
    final unknown = loginChallengeFixture(includeContext: false)
      ..['unknown'] = 'not-negotiated';
    expect(
      () => LoginConfirmation.parse(unknown, contextOrigin, contextNow),
      throwsFormatException,
    );
  });

  test('five sessions and five desktops total accepted; sixth across sessions rejected', () {
    Map<String, Object?> make(int desktops) => {
      'username': 'alice',
      'sessionCount': 5,
      'sessions': List.generate(
        5,
        (index) => {
          'issuer': 'legacy',
          'createdAt': '2026-11-06T04:30:00.000Z',
          'desktops': List.generate(
            index == 4 ? desktops - 4 : 1,
            (n) => {'id': 'pve-${149 + index * 2 + n}', 'name': 'VM $index $n'},
          ),
        },
      ),
    };
    expect(
      () => LoginConfirmation.parse(
        loginChallengeFixture(context: make(5)),
        contextOrigin,
        contextNow,
      ),
      returnsNormally,
    );
    expect(
      () => LoginConfirmation.parse(
        loginChallengeFixture(context: make(6)),
        contextOrigin,
        contextNow,
      ),
      throwsFormatException,
    );
  });
}
