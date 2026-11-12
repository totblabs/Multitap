import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_watch.dart';
import 'package:multitap_client/data/notices.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, flush, info;

// 공지 (notice-v1) in the client's data layer: what a notice is, when it is
// read, what is raised as a banner, what counts as seen, and the two places
// the broker says "read now" (the live channel and a stream's control
// socket). The broker side is server/test/notices.test.mjs.

final base = DateTime.utc(2026, 11, 10, 9);
String hex(int n) => n.toRadixString(16).padLeft(32, '0');
Map<String, Object?> wire(
  int n, {
  String kind = 'general',
  String title = '오늘 오후 6시 서버 점검 안내',
  String body = '저장하고 연결을 끊어 주세요.',
  Duration sent = Duration.zero,
  Duration lifetime = const Duration(hours: 24),
  Duration? scheduled,
  bool seen = false,
}) => {
  'id': hex(n),
  'kind': kind,
  'title': title,
  'body': body,
  'sentAt': base.add(sent).toIso8601String(),
  'expiresAt': base.add(sent).add(lifetime).toIso8601String(),
  'scheduledAt': scheduled == null
      ? null
      : base.add(scheduled).toIso8601String(),
  'seen': seen,
};
BrokerNotice notice(
  int n, {
  String kind = 'general',
  Duration sent = Duration.zero,
  Duration lifetime = const Duration(hours: 24),
  Duration? scheduled,
  bool seen = false,
}) => BrokerNotice.parse(
  wire(
    n,
    kind: kind,
    sent: sent,
    lifetime: lifetime,
    scheduled: scheduled,
    seen: seen,
  ),
)!;

/// A broker that answers what the test put in [list].
class FakeNotices implements NoticeBrokerApi {
  List<BrokerNotice> list = [];
  Object? readError, seenError;
  int reads = 0;
  final seen = <List<String>>[];
  final tokens = <String>{};
  @override
  Future<List<BrokerNotice>> notices(String token) async {
    reads++;
    tokens.add(token);
    if (readError != null) throw readError!;
    return List.of(list);
  }

  @override
  Future<void> markNoticesSeen(String token, List<String> ids) async {
    seen.add(List.of(ids));
    if (seenError != null) throw seenError!;
    // The broker remembers: the next read says seen.
    list = [
      for (final n in list) ids.contains(n.id) ? n.withSeen() : n,
    ];
  }
}

/// The same broker with 지우기 (a newer server): [offered] is
/// what its list answer says.
class FakeRemovableNotices extends FakeNotices
    implements NoticeRemovalBrokerApi {
  FakeRemovableNotices({this.offered = true});
  bool offered;
  Object? removeError;
  final removed = <List<String>>[];
  @override
  bool noticeRemovalOffered(String token) => offered && reads > 0;
  @override
  Future<void> removeNotices(String token, List<String> ids) async {
    removed.add(List.of(ids));
    if (removeError != null) throw removeError!;
    // The broker remembers: the next read leaves them out.
    list = [
      for (final n in list)
        if (!ids.contains(n.id)) n,
    ];
  }
}

// A scripted HTTP client for HttpBrokerApi (answers in order).
class _Headers extends Fake implements HttpHeaders {
  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) {}
  @override
  String? value(String name) => null;
  @override
  ContentType? contentType;
}

class _Response extends Fake implements HttpClientResponse {
  _Response(this.statusCode, this.value);
  @override
  final int statusCode;
  final Map<String, Object?> value;
  @override
  HttpHeaders get headers => _Headers();
  @override
  Stream<List<int>> timeout(
    Duration timeLimit, {
    void Function(EventSink<List<int>>)? onTimeout,
  }) => Stream<List<int>>.value(
    utf8.encode(jsonEncode(value)),
  ).timeout(timeLimit, onTimeout: onTimeout);
}

class _Request extends Fake implements HttpClientRequest {
  _Request(this.method, this.uri, this.response);
  @override
  final String method;
  @override
  final Uri uri;
  final _Response response;
  final written = StringBuffer();
  @override
  final _Headers headers = _Headers();
  @override
  bool followRedirects = true;
  @override
  void write(Object? value) => written.write(value);
  @override
  void abort([Object? exception, StackTrace? stackTrace]) {}
  @override
  Future<HttpClientResponse> close() async => response;
}

class _ApiClient extends Fake implements HttpClient {
  final responses = <(int, Map<String, Object?>)>[];
  final requests = <_Request>[];
  @override
  Duration? connectionTimeout;
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final response = responses.removeAt(0);
    final request = _Request(method, uri, _Response(response.$1, response.$2));
    requests.add(request);
    return request;
  }

  @override
  void close({bool force = false}) {}
}

void main() {
  test('a notice is parsed as text within bounds; anything else is left out', () {
    final parsed = BrokerNotice.parse(
      wire(1, kind: 'maintenance', scheduled: const Duration(hours: 2)),
    )!;
    expect(parsed.id, hex(1));
    expect(parsed.kind, NoticeKind.maintenance);
    expect(parsed.title, '오늘 오후 6시 서버 점검 안내');
    expect(parsed.scheduledAt, base.add(const Duration(hours: 2)));
    expect(parsed.seen, isFalse);
    // An unknown kind is an ordinary notice; markup stays characters.
    final plain = BrokerNotice.parse(
      wire(2, kind: 'urgent', title: '<b>굵게</b>', body: 'https://a.invalid'),
    )!;
    expect(plain.kind, NoticeKind.general);
    expect(plain.title, '<b>굵게</b>');
    expect(plain.body, 'https://a.invalid');
    for (final bad in <Object?>[
      null,
      'text',
      {...wire(3), 'id': 'short'},
      {...wire(3), 'title': ''},
      {...wire(3), 'title': 5},
      {...wire(3), 'title': '가' * 241},
      {...wire(3), 'body': '나' * 2401},
      {...wire(3), 'sentAt': 'yesterday'},
      {...wire(3), 'expiresAt': null},
      {...wire(3), 'scheduledAt': 'soon'},
    ]) {
      expect(BrokerNotice.parse(bad), isNull, reason: '$bad');
    }
    expect(
      BrokerNotice.parseList({
        'protocol': 'notice-v1',
        'notices': [wire(1), 'junk', wire(2)],
      }).map((n) => n.id),
      [hex(1), hex(2)],
    );
    for (final bad in [
      {'protocol': 'other', 'notices': <Object>[]},
      {'protocol': 'notice-v1'},
      {'protocol': 'notice-v1', 'notices': 'none'},
    ]) {
      expect(
        () => BrokerNotice.parseList(bad),
        throwsA(isA<BrokerException>()),
      );
    }
  });

  test('after a sign-in everything unseen is raised, oldest first; seen ones only fill the list', () {
    fakeAsync((async) {
      final api = FakeNotices()
        ..list = [
          notice(3, sent: const Duration(minutes: -1)),
          notice(2, sent: const Duration(hours: -3), seen: true),
          notice(1, sent: const Duration(hours: -5)),
        ];
      var now = base;
      var changes = 0;
      final center = NoticeCenter(
        api: api,
        token: 'token-a',
        clock: () => now,
      )..addListener(() => changes++);
      expect(center.notices, isEmpty);
      center.start();
      async.flushMicrotasks();
      expect(api.tokens, {'token-a'});
      expect(center.notices.map((n) => n.id), [hex(3), hex(2), hex(1)]);
      expect(center.unread, 2);
      expect(center.banner!.id, hex(1), reason: 'the oldest unseen first');
      expect(center.waiting, 1);
      expect(changes, greaterThan(0));
      // Closed by hand: seen here at once and told to the broker.
      center.dismiss(hex(1));
      async.flushMicrotasks();
      expect(api.seen, [
        [hex(1)],
      ]);
      expect(center.unread, 1);
      expect(center.banner!.id, hex(3));
      // Gone by itself: it stays unread in the list and nothing is told.
      center.lower(hex(3));
      async.flushMicrotasks();
      expect(center.banner, isNull);
      expect(center.unread, 1);
      expect(api.seen.length, 1);
      // Opening the list sees the rest.
      center.markAllSeen();
      async.flushMicrotasks();
      expect(api.seen.last, [hex(3)]);
      expect(center.unread, 0);
      center.markAllSeen();
      async.flushMicrotasks();
      expect(api.seen.length, 2, reason: 'nothing left to say');
      center.dispose();
      now = now.add(const Duration(seconds: 1));
    });
  });

  test('a stream window raises only what is fresh or a 점검 예고 still ahead', () {
    fakeAsync((async) {
      final api = FakeNotices()
        ..list = [
          notice(4, sent: const Duration(minutes: -2)),
          notice(
            3,
            kind: 'maintenance',
            sent: const Duration(hours: -2),
            scheduled: const Duration(hours: 1),
          ),
          notice(
            2,
            kind: 'maintenance',
            sent: const Duration(hours: -2),
            scheduled: const Duration(minutes: -5),
          ),
          notice(1, sent: const Duration(hours: -4)),
        ];
      final center = NoticeCenter(
        api: api,
        token: 't',
        greeting: NoticeGreeting.recent,
        clock: () => base,
      )..start();
      async.flushMicrotasks();
      expect(center.notices.length, 4);
      expect(center.unread, 4, reason: 'all of them are in the list');
      expect(center.banner!.id, hex(3));
      center.lower(hex(3));
      expect(center.banner!.id, hex(4));
      center.lower(hex(4));
      expect(center.banner, isNull, reason: 'the old ones are not raised');
      // What arrives while the window is open is raised whatever its age.
      api.list = [notice(5, sent: const Duration(hours: -1)), ...api.list];
      center.signal();
      async.flushMicrotasks();
      expect(center.banner!.id, hex(5));
      center.dispose();
    });
  });

  test('reads: at start, on a push, on a timer while no push channel is up, rarely while one is', () {
    fakeAsync((async) {
      final api = FakeNotices();
      var pushed = false;
      var now = base;
      final center = NoticeCenter(
        api: api,
        token: 't',
        clock: () => now,
        pushConnected: () => pushed,
      )..start();
      // The clock and the timers move together, a second at a time.
      void pass(Duration time) {
        for (var second = 0; second < time.inSeconds; second++) {
          now = now.add(const Duration(seconds: 1));
          async.elapse(const Duration(seconds: 1));
        }
      }

      async.flushMicrotasks();
      expect(api.reads, 1);
      pass(const Duration(seconds: 50));
      expect(api.reads, 1);
      pass(const Duration(seconds: 15));
      expect(api.reads, 2, reason: 'about a minute without a push channel');
      pass(const Duration(minutes: 3));
      expect(api.reads, 5);
      // The live channel is up: it says when to read.
      pushed = true;
      pass(const Duration(minutes: 4));
      expect(api.reads, 5);
      center.signal();
      async.flushMicrotasks();
      expect(api.reads, 6);
      pass(const Duration(minutes: 6));
      expect(api.reads, 7, reason: 'a rare read even so');
      // Two pushes during one read are one more read, not two.
      center.signal();
      center.signal();
      center.signal();
      async.flushMicrotasks();
      expect(api.reads, 9);
      center.dispose();
      center.signal();
      pass(const Duration(minutes: 10));
      expect(api.reads, 9, reason: 'a closed sign-in reads nothing');
    });
  });

  test('a withdrawn notice leaves the banner and the list; an expired one too, without a read', () {
    fakeAsync((async) {
      final api = FakeNotices()
        ..list = [
          notice(2, lifetime: const Duration(minutes: 5)),
          notice(1),
        ];
      var now = base;
      final center = NoticeCenter(api: api, token: 't', clock: () => now)
        ..start();
      async.flushMicrotasks();
      expect(center.banner!.id, hex(1));
      api.list = [notice(2, lifetime: const Duration(minutes: 5))];
      center.signal();
      async.flushMicrotasks();
      expect(center.notices.map((n) => n.id), [hex(2)]);
      expect(center.banner!.id, hex(2), reason: 'the withdrawn one went');
      // The server is away; the notice still ends at its time.
      api.readError = const BrokerException('network_error');
      now = now.add(const Duration(minutes: 6));
      async.elapse(const Duration(seconds: 16));
      expect(center.notices, isEmpty);
      expect(center.banner, isNull);
      center.dispose();
    });
  });

  test('an older broker (404) is not asked on the timer, and is asked again once it is updated; a failed read or seen report is tried again', () {
    fakeAsync((async) {
      final old = FakeNotices()
        ..readError = const BrokerException('not_found', 404);
      var now = base;
      final quiet = NoticeCenter(api: old, token: 't', clock: () => now)
        ..start();
      async.flushMicrotasks();
      expect(quiet.supported, isFalse);
      now = now.add(const Duration(minutes: 30));
      async.elapse(const Duration(minutes: 30));
      expect(old.reads, 1, reason: 'the timer does not ask an older broker');
      // The live channel came back and the broker is still the older one.
      quiet.signal();
      async.flushMicrotasks();
      expect(old.reads, 2);
      expect(quiet.supported, isFalse);
      // The server was updated while this sign-in went on (2026-11-10: the
      // launcher's bell stayed empty until the next sign-in). Its push, or
      // the channel coming back, asks again and the notice shows.
      old
        ..readError = null
        ..list = [notice(1)];
      quiet.signal();
      async.flushMicrotasks();
      expect(quiet.supported, isTrue);
      expect(quiet.unread, 1);
      expect(quiet.banner!.id, hex(1));
      now = now.add(const Duration(minutes: 6));
      async.elapse(const Duration(minutes: 6));
      expect(old.reads, greaterThan(3), reason: 'the timer reads again');
      quiet.dispose();

      final api = FakeNotices()
        ..list = [notice(1)]
        ..readError = const BrokerException('server_restarting', 503);
      final center = NoticeCenter(api: api, token: 't', clock: () => now)
        ..start();
      async.flushMicrotasks();
      expect(center.supported, isTrue);
      expect(center.notices, isEmpty);
      api.readError = null;
      now = now.add(const Duration(seconds: 75));
      async.elapse(const Duration(seconds: 75));
      expect(center.banner!.id, hex(1));
      // The seen report fails: seen here all the same, and sent with the next read.
      api.seenError = const BrokerException('network_error');
      center.dismiss(hex(1));
      async.flushMicrotasks();
      expect(center.unread, 0);
      expect(api.seen.length, 1);
      api.seenError = null;
      center.signal();
      async.flushMicrotasks();
      expect(api.seen.length, 2);
      expect(api.seen.last, [hex(1)]);
      expect(center.unread, 0, reason: 'a stale answer does not unsee it');
      center.signal();
      async.flushMicrotasks();
      expect(api.seen.length, 2, reason: 'delivered once');
      center.dispose();
    });
  });

  test('how long a banner stays and how its time is said', () {
    final short = notice(1), long = BrokerNotice.parse(wire(2, body: '가' * 400))!;
    final planned = notice(
      3,
      kind: 'maintenance',
      scheduled: const Duration(hours: 2),
    );
    expect(noticeBannerDuration(short).inSeconds, inInclusiveRange(10, 12));
    expect(noticeBannerDuration(long).inSeconds, 25);
    expect(noticeBannerDuration(planned).inSeconds, greaterThanOrEqualTo(30));
    expect(
      noticeBannerDuration(planned),
      greaterThan(noticeBannerDuration(short)),
      reason: '점검 예고 stays longer',
    );
    BrokerNotice at(Duration ahead) =>
        notice(9, kind: 'maintenance', scheduled: ahead);
    expect(noticeWhen(at(const Duration(seconds: 20)), base), '곧');
    expect(noticeWhen(at(const Duration(seconds: 60)), base), '1분 뒤');
    expect(noticeWhen(at(const Duration(seconds: 290)), base), '5분 뒤');
    expect(noticeWhen(at(const Duration(minutes: 59)), base), '59분 뒤');
    expect(noticeWhen(at(const Duration(seconds: -30)), base), '곧');
    expect(noticeWhen(at(const Duration(minutes: -3)), base), isNull);
    expect(noticeWhen(short, base), isNull);
    final local = base.toLocal();
    final evening = DateTime(local.year, local.month, local.day, 23, 30);
    final tomorrow = DateTime(local.year, local.month, local.day + 1, 9, 5);
    final later = DateTime(local.year, local.month, local.day + 3, 18);
    final morning = DateTime(local.year, local.month, local.day, 1);
    BrokerNotice on(DateTime time) => BrokerNotice.parse({
      ...wire(9, kind: 'maintenance'),
      'scheduledAt': time.toUtc().toIso8601String(),
    })!;
    expect(noticeWhen(on(evening), morning), '오늘 오후 11:30');
    expect(noticeWhen(on(tomorrow), morning), '내일 오전 9:05');
    expect(
      noticeWhen(on(later), morning),
      '${later.month}월 ${later.day}일 오후 6:00',
    );
    expect(noticeClock(DateTime(2026, 11, 10, 0, 7)), '오전 12:07');
    expect(noticeClock(DateTime(2026, 11, 10, 12)), '오후 12:00');
    expect(noticeAge(base, base.add(const Duration(seconds: 20))), '방금');
    expect(noticeAge(base, base.add(const Duration(minutes: 12))), '12분 전');
    expect(noticeAge(base, base.add(const Duration(hours: 3))), '3시간 전');
    expect(
      noticeAge(base, base.add(const Duration(days: 2))),
      '${base.toLocal().month}월 ${base.toLocal().day}일',
    );
  });

  testWidgets('the live channel asks for notices and passes each push on once', (
    t,
  ) async {
    final socket = SessionSocket();
    Uri? dialled;
    final watch = BrokerDesktopWatch(
      origin: Uri.parse('https://cloud.example.com:18443/'),
      token: 'fixture-memory-only-bearer',
      dialer: (uri, _, _) async {
        dialled = uri;
        return socket;
      },
    );
    final pushes = <int>[];
    var count = 0;
    final listening = watch.notices.listen((_) => pushes.add(++count));
    watch.start();
    await t.pump();
    expect(dialled!.path, '/api/v1/events');
    socket.incoming.add('{"type":"desktops.ready"}');
    await t.pump();
    expect(watch.connected, isTrue);
    socket.incoming.add(jsonEncode({'type': 'notices', 'revision': 1}));
    await t.pump();
    expect(pushes, [1]);
    // A repeated or older revision is the same news.
    socket.incoming.add(jsonEncode({'type': 'notices', 'revision': 1}));
    await t.pump();
    expect(pushes, [1]);
    socket.incoming.add(jsonEncode({'type': 'notices', 'revision': 4}));
    await t.pump();
    expect(pushes, [1, 2]);
    expect(watch.connected, isTrue, reason: 'a notice never ends the channel');
    // The push carries no text: one with content is not what was agreed.
    socket.incoming.add(jsonEncode({'type': 'notices', 'revision': 'x'}));
    await t.pump();
    expect(watch.connected, isFalse);
    unawaited(listening.cancel());
    var done = false;
    unawaited(watch.close().then((_) => done = true));
    await t.pump();
    await t.pump(const Duration(seconds: 5));
    expect(done, isTrue, reason: 'the watch closes within bounded fake time');
  });

  test('the live channel and a stream socket ask with the header a released broker ignores', () {
    expect(noticeLiveFeature, 'notices');
    expect(noticeProtocol, 'notice-v1');
  });

  test('a stream hears `notices` on its control socket and keeps streaming', () async {
    final socket = SessionSocket(), media = SessionMedia();
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'test-session',
        webSocketUrl: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/test-session',
        ),
      ),
      token: 'test-session-token',
      media: media,
      dialer: (uri, token) async => socket,
    );
    var heard = 0;
    session.onBrokerNotices = () => heard++;
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(info));
    await flush();
    socket.incoming.add(jsonEncode({'type': 'notices', 'revision': 1}));
    await flush();
    expect(heard, 1);
    expect(session.closed, isFalse, reason: 'a notice is not a stream event');
    // Without a listener it is simply nothing.
    session.onBrokerNotices = null;
    socket.incoming.add(jsonEncode({'type': 'notices', 'revision': 2}));
    await flush();
    expect(session.closed, isFalse);
    socket.incoming.add(jsonEncode({'type': 'session.ended'}));
    await flush();
    expect(session.closed, isTrue);
    await media.events.close();
  });

  // ---- 지우기: removing a notice from one's own list.
  test('지우기 is offered only by a list answer that says so; an older answer has no such word', () {
    expect(
      BrokerNotice.removalOffered({
        'protocol': 'notice-v1',
        'features': {'remove': 1},
        'notices': <Object>[],
      }),
      isTrue,
    );
    // Exactly what an older server answers.
    expect(
      BrokerNotice.removalOffered({
        'protocol': 'notice-v1',
        'serverTime': '2026-11-10T09:00:00.000Z',
        'notices': [wire(1)],
      }),
      isFalse,
    );
    for (final features in <Object?>[
      null,
      true,
      'remove',
      <Object>[],
      {'remove': 0},
      {'remove': true},
      {'remove': '1'},
      {'other': 1},
    ]) {
      expect(
        BrokerNotice.removalOffered({'features': features}),
        isFalse,
        reason: '$features',
      );
    }
  });

  test('지우기: gone from the list and the banner at once, told to the broker once, and a stale answer does not bring it back', () {
    fakeAsync((async) {
      final api = FakeRemovableNotices()
        ..list = [notice(3), notice(2, seen: true), notice(1)];
      final center = NoticeCenter(api: api, token: 't', clock: () => base);
      expect(center.canRemove, isFalse, reason: 'nothing was read yet');
      center.start();
      async.flushMicrotasks();
      expect(center.canRemove, isTrue);
      expect(center.unread, 2);
      expect(center.banner!.id, hex(1));
      var changes = 0;
      center.addListener(() => changes++);
      center.remove([hex(1)]);
      // Before the broker answered anything: already gone here.
      expect(center.notices.map((n) => n.id), [hex(3), hex(2)]);
      expect(center.unread, 1, reason: 'the bell count follows');
      expect(center.banner!.id, hex(3), reason: 'its banner went with it');
      expect(changes, 1);
      async.flushMicrotasks();
      expect(api.removed, [
        [hex(1)],
      ]);
      expect(api.seen, isEmpty, reason: 'removing is not a seen report');
      // An answer that was on its way before the removal still lists it.
      api.list = [notice(3), notice(2, seen: true), notice(1)];
      center.signal();
      async.flushMicrotasks();
      expect(center.notices.map((n) => n.id), [hex(3), hex(2)]);
      expect(api.removed.length, 1, reason: 'told once');
      // Unknown ids and repeats do nothing.
      center.remove([hex(1), hex(9)]);
      async.flushMicrotasks();
      expect(api.removed.length, 1);
      // 모두 지우기: everything left, in one request.
      center.removeAll();
      expect(center.notices, isEmpty);
      expect(center.unread, 0);
      expect(center.banner, isNull);
      async.flushMicrotasks();
      expect(api.removed.last, [hex(3), hex(2)]);
      center.removeAll();
      async.flushMicrotasks();
      expect(api.removed.length, 2, reason: 'an empty list sends nothing');
      // A new notice still arrives.
      api.list = [notice(4)];
      center.signal();
      async.flushMicrotasks();
      expect(center.notices.map((n) => n.id), [hex(4)]);
      center.dispose();
    });
  });

  test('지우기: a failed send keeps the notice gone here and is sent again with the next read', () {
    fakeAsync((async) {
      final api = FakeRemovableNotices()
        ..list = [notice(2), notice(1)]
        ..removeError = const BrokerException('network_error');
      final center = NoticeCenter(api: api, token: 't', clock: () => base)
        ..start();
      async.flushMicrotasks();
      center.remove([hex(1)]);
      async.flushMicrotasks();
      expect(api.removed.length, 1);
      expect(center.notices.map((n) => n.id), [hex(2)], reason: 'gone at once');
      // The broker never heard: its next answer still has it. It stays gone
      // here, and the removal goes out again.
      api.removeError = null;
      center.signal();
      async.flushMicrotasks();
      expect(center.notices.map((n) => n.id), [hex(2)]);
      expect(api.removed, [
        [hex(1)],
        [hex(1)],
      ]);
      center.signal();
      async.flushMicrotasks();
      expect(api.removed.length, 2, reason: 'delivered once');
      expect(api.list.map((n) => n.id), [hex(2)]);
      center.dispose();
    });
  });

  test('지우기 with an older broker: never offered, and asking removes nothing and sends nothing', () {
    fakeAsync((async) {
      // An older server: the client's API object can remove, the broker's answer does not offer it.
      final old = FakeRemovableNotices(offered: false)
        ..list = [notice(2), notice(1)];
      final center = NoticeCenter(api: old, token: 't', clock: () => base)
        ..start();
      async.flushMicrotasks();
      expect(center.canRemove, isFalse);
      center.remove([hex(1)]);
      center.removeAll();
      async.flushMicrotasks();
      expect(center.notices.length, 2);
      expect(old.removed, isEmpty);
      // The server is updated while this sign-in lives: the next answer offers it.
      old.offered = true;
      center.signal();
      async.flushMicrotasks();
      expect(center.canRemove, isTrue);
      center.remove([hex(1)]);
      async.flushMicrotasks();
      expect(old.removed, [
        [hex(1)],
      ]);
      // And taken back (a rollback): the control goes again.
      old.offered = false;
      center.signal();
      async.flushMicrotasks();
      expect(center.canRemove, isFalse);
      center.dispose();

      // An API object without the capability at all (a test double, another client build).
      final plain = FakeNotices()..list = [notice(1)];
      final none = NoticeCenter(api: plain, token: 't', clock: () => base)
        ..start();
      async.flushMicrotasks();
      expect(none.canRemove, isFalse);
      none.removeAll();
      expect(none.notices.length, 1);
      none.dispose();
    });
  });

  test('HttpBrokerApi: 지우기 goes to /api/v1/notices/remove only after an answer that offered it', () async {
    Map<String, Object?> answer({bool features = true}) => {
      'protocol': 'notice-v1',
      'serverTime': DateTime.now().toUtc().toIso8601String(),
      if (features) 'features': {'remove': 1},
      'notices': [wire(1)],
    };
    final client = _ApiClient()
      ..responses.add((200, answer(features: false))) // an older server
      ..responses.add((200, answer()))
      ..responses.add((200, {'ok': true, 'removed': 1}))
      ..responses.add((200, answer(features: false)));
    final api = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: client,
    );
    expect(api.noticeRemovalOffered('qa-token'), isFalse);
    expect((await api.notices('qa-token')).single.id, hex(1));
    expect(api.noticeRemovalOffered('qa-token'), isFalse);
    // Not offered: refused here, nothing is sent to the old broker.
    await expectLater(
      api.removeNotices('qa-token', [hex(1)]),
      throwsA(isA<BrokerException>()),
    );
    expect(client.requests.length, 1);
    // The broker was updated: its answer offers it, for this sign-in only.
    await api.notices('qa-token');
    expect(api.noticeRemovalOffered('qa-token'), isTrue);
    expect(api.noticeRemovalOffered('another-token'), isFalse);
    await api.removeNotices('qa-token', [hex(1)]);
    final sent = client.requests.last;
    expect(sent.method, 'POST');
    expect(sent.uri.path, '/api/v1/notices/remove');
    expect(jsonDecode(sent.written.toString()), {
      'ids': [hex(1)],
    });
    await api.removeNotices('qa-token', []);
    expect(client.requests.length, 3, reason: 'nothing to remove, nothing sent');
    // Rolled back: the next answer takes the offer away.
    await api.notices('qa-token');
    expect(api.noticeRemovalOffered('qa-token'), isFalse);
    api.close();
  });
}
