import 'dart:async';

import 'package:flutter/foundation.dart';

import 'broker_api.dart' show BrokerException;
import '../l10n/l10n.dart';

/// 공지 (notice-v1, server/src/notices.mjs): a short notice an administrator
/// sent to the people who are signed in or connected.
///
/// A notice is text. Nothing in it is parsed, linked, opened or run here or in
/// the widgets that show it (lib/ui/notice_widgets.dart).
const noticeProtocol = 'notice-v1';

/// What the client asks the live channel and a stream's control socket for.
/// A released broker ignores the word; a released client never sends it.
const noticeLiveFeature = 'notices';

enum NoticeKind { general, maintenance }

@immutable
class BrokerNotice {
  const BrokerNotice({
    required this.id,
    required this.kind,
    required this.title,
    required this.body,
    required this.sentAt,
    required this.expiresAt,
    this.scheduledAt,
    this.seen = false,
  });
  final String id, title, body;
  final NoticeKind kind;
  final DateTime sentAt, expiresAt;

  /// 점검 예고: the stated time.
  final DateTime? scheduledAt;

  /// The person closed it by hand or opened the list (on any of their apps).
  final bool seen;

  BrokerNotice withSeen() => seen
      ? this
      : BrokerNotice(
          id: id,
          kind: kind,
          title: title,
          body: body,
          sentAt: sentAt,
          expiresAt: expiresAt,
          scheduledAt: scheduledAt,
          seen: true,
        );

  static final _id = RegExp(r'^[a-f0-9]{32}$');

  /// The same bounds the broker sends within (notices.mjs), with room to
  /// spare; anything else is not a notice and is left out.
  static BrokerNotice? parse(Object? value) {
    if (value is! Map) return null;
    final id = value['id'], title = value['title'], body = value['body'];
    final sent = _time(value['sentAt']), expires = _time(value['expiresAt']);
    if (id is! String ||
        !_id.hasMatch(id) ||
        title is! String ||
        title.isEmpty ||
        title.length > 240 ||
        body is! String ||
        body.length > 2400 ||
        sent == null ||
        expires == null ||
        (value['scheduledAt'] != null && _time(value['scheduledAt']) == null)) {
      return null;
    }
    return BrokerNotice(
      id: id,
      kind: value['kind'] == 'maintenance'
          ? NoticeKind.maintenance
          : NoticeKind.general,
      title: title,
      body: body,
      sentAt: sent,
      expiresAt: expires,
      scheduledAt: _time(value['scheduledAt']),
      seen: value['seen'] == true,
    );
  }

  static DateTime? _time(Object? value) =>
      value is String && value.length <= 40 ? DateTime.tryParse(value) : null;

  /// The answer of GET /api/v1/notices, newest first.
  static List<BrokerNotice> parseList(Map<String, dynamic> value) {
    final list = value['notices'];
    if (value['protocol'] != noticeProtocol || list is! List) {
      throw const BrokerException('invalid_response');
    }
    return [
      for (final item in list.take(40)) ?parse(item),
    ];
  }

  /// Whether the broker that gave this list answer can remove a notice from
  /// the account's list (지우기). An older broker's answer has no `features`.
  static bool removalOffered(Map<String, dynamic> value) {
    final features = value['features'];
    final remove = features is Map ? features['remove'] : null;
    return remove is int && remove >= 1;
  }
}

/// The two broker calls of notice-v1. A broker without them answers 404.
abstract interface class NoticeBrokerApi {
  /// The notices this account's app shows now. Reading is the delivery.
  Future<List<BrokerNotice>> notices(String token);

  /// The person saw these (closed by hand, or opened the list).
  Future<void> markNoticesSeen(String token, List<String> ids);
}

/// 지우기: removing notices from this account's own list, where the broker
/// can do it. The broker keeps it per account, so a removed notice stays gone
/// on the person's other devices and after the next sign-in.
abstract interface class NoticeRemovalBrokerApi {
  /// Whether the broker's latest list answer for [token] offered it. False
  /// with an older broker: nothing then shows a 지우기 control.
  bool noticeRemovalOffered(String token);

  /// The person removed these from their list.
  Future<void> removeNotices(String token, List<String> ids);
}

/// Which notices of the first read are raised as a banner.
enum NoticeGreeting {
  /// Everything not yet seen: the launcher after a sign-in (the person was
  /// away when it was sent).
  unseen,

  /// Only what is fresh (sent in the last ten minutes) or a 점검 예고 whose
  /// time has not come: a stream window opened later does not repeat an old
  /// notice the launcher already raised.
  recent,
}

/// One sign-in's notices in one window: what to show, what is unread, and the
/// banner to raise. It reads the broker at [start], whenever [signal] is
/// called (the live channel or the stream's control socket said `notices`),
/// and on a timer: every [pollInterval] while no push channel is connected,
/// every [quietInterval] while one is.
class NoticeCenter extends ChangeNotifier {
  NoticeCenter({
    required this._api,
    required this._token,
    this.greeting = NoticeGreeting.unseen,
    DateTime Function()? clock,
    this.pollInterval = const Duration(seconds: 60),
    this.quietInterval = const Duration(minutes: 5),
    bool Function()? pushConnected,
  }) : _clock = clock ?? DateTime.now,
       _pushConnected = pushConnected ?? (() => false);
  final NoticeBrokerApi _api;
  final String _token;
  final DateTime Function() _clock;
  final bool Function() _pushConnected;
  final NoticeGreeting greeting;
  final Duration pollInterval, quietInterval;

  List<BrokerNotice> _notices = const [];
  final _known = <String>{}, _seenLocally = <String>{}, _unsent = <String>{};
  final _removedLocally = <String>{}, _unsentRemovals = <String>{};
  bool _canRemove = false;
  final _raised = <BrokerNotice>[];
  Timer? _timer;
  DateTime? _lastRead;
  bool _started = false, _closed = false, _reading = false, _again = false;
  bool _first = true, _supported = true;

  /// What shows now, newest first (expired ones are gone).
  List<BrokerNotice> get notices => _notices;
  int get unread => _notices.where((notice) => !notice.seen).length;

  /// False once the broker said it has no notices (an older broker).
  bool get supported => _supported;

  /// Whether a notice can be removed from the list (지우기): the broker's
  /// latest answer offered it. False with an older broker, and then nothing
  /// offers the control.
  bool get canRemove => _canRemove;

  /// The banner to show now: the oldest notice that arrived and was not yet
  /// taken down ([dismiss], [lower]); null when there is none.
  BrokerNotice? get banner => _raised.isEmpty ? null : _raised.first;

  /// How many arrived banners wait behind [banner].
  int get waiting => _raised.isEmpty ? 0 : _raised.length - 1;

  void start() {
    if (_started || _closed) return;
    _started = true;
    unawaited(refresh());
    // One short tick decides whether a read is due and drops expired notices.
    _timer = Timer.periodic(const Duration(seconds: 15), (_) => _tick());
  }

  void _tick() {
    if (_closed) return;
    _dropExpired();
    final last = _lastRead;
    final every = _pushConnected() ? quietInterval : pollInterval;
    if (_supported && (last == null || _clock().difference(last) >= every)) {
      unawaited(refresh());
    }
  }

  /// The broker said there is something to read, or the live channel came
  /// back. Either can be a broker that was updated after an older one answered
  /// that it has no notices (a sign-in outlives a server update), so it is
  /// asked again.
  void signal() {
    if (_closed || !_started) return;
    _supported = true;
    unawaited(refresh());
  }

  Future<void> refresh() async {
    if (_closed || !_supported) return;
    if (_reading) {
      _again = true;
      return;
    }
    _reading = true;
    try {
      do {
        _again = false;
        _lastRead = _clock();
        final List<BrokerNotice> list;
        try {
          list = await _api.notices(_token);
        } on BrokerException catch (error) {
          // An older broker has no such route: the timer stops asking; a push
          // or a channel that came back asks again ([signal]).
          if (error.status == 404 || error.status == 405) {
            _supported = false;
            _canRemove = false;
          }
          return;
        } catch (_) {
          return; // The next tick or push reads again.
        }
        if (_closed) return;
        final api = _api;
        _canRemove =
            api is NoticeRemovalBrokerApi &&
            (api as NoticeRemovalBrokerApi).noticeRemovalOffered(_token);
        _apply(list);
        if (_unsent.isNotEmpty) unawaited(_send(_unsent.toList()));
        if (_unsentRemovals.isNotEmpty && _canRemove) {
          unawaited(_sendRemovals(_unsentRemovals.toList()));
        }
      } while (_again && !_closed);
    } finally {
      _reading = false;
    }
  }

  void _apply(List<BrokerNotice> list) {
    final now = _clock();
    final current = [
      for (final notice in list)
        // An answer from before a removal does not bring the notice back.
        if (notice.expiresAt.isAfter(now) &&
            !_removedLocally.contains(notice.id))
          _seenLocally.contains(notice.id) ? notice.withSeen() : notice,
    ];
    final ids = {for (final notice in current) notice.id};
    // A withdrawn or expired notice leaves the banner too.
    _raised.removeWhere((notice) => !ids.contains(notice.id));
    for (var index = 0; index < _raised.length; index++) {
      _raised[index] = current.firstWhere((n) => n.id == _raised[index].id);
    }
    _raised.removeWhere((notice) => notice.seen);
    // Oldest first, so several that arrived together are read in order.
    for (final notice in current.reversed) {
      if (!_known.add(notice.id) || notice.seen) continue;
      if (_first && greeting == NoticeGreeting.recent && !_fresh(notice, now)) {
        continue;
      }
      _raised.add(notice);
    }
    _first = false;
    _notices = current;
    notifyListeners();
  }

  bool _fresh(BrokerNotice notice, DateTime now) =>
      now.difference(notice.sentAt) < const Duration(minutes: 10) ||
      (notice.kind == NoticeKind.maintenance &&
          notice.scheduledAt != null &&
          notice.scheduledAt!.isAfter(now));

  void _dropExpired() {
    final now = _clock();
    if (_notices.every((notice) => notice.expiresAt.isAfter(now))) return;
    _notices = [
      for (final notice in _notices)
        if (notice.expiresAt.isAfter(now)) notice,
    ];
    _raised.removeWhere((notice) => !notice.expiresAt.isAfter(now));
    notifyListeners();
  }

  /// The banner went away by itself: the notice stays unread in the list.
  void lower(String id) {
    final before = _raised.length;
    _raised.removeWhere((notice) => notice.id == id);
    if (_raised.length != before) notifyListeners();
  }

  /// The person closed the banner by hand: seen.
  void dismiss(String id) => markSeen([id]);

  /// The person opened the list: everything in it is seen.
  void markAllSeen() =>
      markSeen([for (final n in _notices) if (!n.seen) n.id]);

  void markSeen(List<String> ids) {
    final fresh = [
      for (final id in ids)
        if (_notices.any((n) => n.id == id && !n.seen)) id,
    ];
    _raised.removeWhere((notice) => ids.contains(notice.id));
    if (fresh.isNotEmpty) {
      _seenLocally.addAll(fresh);
      _notices = [
        for (final notice in _notices)
          fresh.contains(notice.id) ? notice.withSeen() : notice,
      ];
      unawaited(_send(fresh));
    }
    notifyListeners();
  }

  Future<void> _send(List<String> ids) async {
    if (_closed || ids.isEmpty) return;
    _unsent.addAll(ids);
    try {
      await _api.markNoticesSeen(_token, ids.take(20).toList());
      _unsent.removeAll(ids.take(20));
    } catch (_) {
      // Kept in [_unsent]: sent again with the next read.
    }
  }

  /// 지우기: the person removed these from their list. They leave the list
  /// (and the banner) at once; the broker is told, and told again with the
  /// next read if that failed. Nothing happens where the broker cannot do it.
  void remove(List<String> ids) {
    if (_closed || !_canRemove) return;
    final gone = [
      for (final id in ids)
        if (_notices.any((notice) => notice.id == id)) id,
    ];
    if (gone.isEmpty) return;
    _removedLocally.addAll(gone);
    _notices = [
      for (final notice in _notices)
        if (!gone.contains(notice.id)) notice,
    ];
    _raised.removeWhere((notice) => gone.contains(notice.id));
    notifyListeners();
    unawaited(_sendRemovals(gone));
  }

  /// 모두 지우기: everything in the list.
  void removeAll() => remove([for (final notice in _notices) notice.id]);

  Future<void> _sendRemovals(List<String> ids) async {
    final api = _api;
    if (_closed || ids.isEmpty || api is! NoticeRemovalBrokerApi) return;
    _unsentRemovals.addAll(ids);
    final batch = ids.take(20).toList();
    try {
      await (api as NoticeRemovalBrokerApi).removeNotices(_token, batch);
      _unsentRemovals.removeAll(batch);
    } catch (_) {
      // Kept in [_unsentRemovals]: sent again with the next read.
    }
  }

  /// Ends this sign-in's notices (sign-out, a closed window).
  void close() {
    if (_closed) return;
    _closed = true;
    _timer?.cancel();
    _raised.clear();
    _notices = const [];
  }

  @override
  void dispose() {
    close();
    super.dispose();
  }
}

/// How long a banner stays up by itself: long enough to read slowly, 점검 예고
/// longer; never less than [minimum].
Duration noticeBannerDuration(BrokerNotice notice) {
  final characters =
      (notice.title + notice.body).replaceAll(RegExp(r'\s'), '').length;
  final reading = 6000 + 120 * characters;
  final (low, high) = notice.kind == NoticeKind.maintenance
      ? (30000, 60000)
      : (10000, 25000);
  return Duration(milliseconds: reading.clamp(low, high));
}

String _two(int value) => value.toString().padLeft(2, '0');

/// "오후 6:00" in the device's own time.
String noticeClock(DateTime time) {
  final local = time.toLocal(), hour = local.hour;
  if (AppLanguageController.instance.code != 'ko') return formatClock(local);
  final twelve = hour % 12 == 0 ? 12 : hour % 12;
  return '${hour < 12 ? l10n.am : l10n.pm} $twelve:${_two(local.minute)}';
}

/// When a 점검 예고 happens, as its reader needs it now: "곧", "5분 뒤",
/// "오늘 오후 6:00", "내일 오전 9:00", "11월 12일 오후 6:00"; null once the
/// time has passed by more than a minute.
String? noticeWhen(BrokerNotice notice, DateTime now) {
  final at = notice.scheduledAt;
  if (at == null) return null;
  final left = at.difference(now);
  if (left < const Duration(minutes: -1)) return null;
  if (left < const Duration(seconds: 45)) return l10n.soon;
  if (left < const Duration(minutes: 60)) {
    return l10n.min((left.inSeconds / 60).ceil());
  }
  final local = at.toLocal(), today = now.toLocal();
  final days = DateTime(local.year, local.month, local.day)
      .difference(DateTime(today.year, today.month, today.day))
      .inDays;
  final day = days == 0
      ? l10n.today
      : days == 1
      ? l10n.tomorrow
      : formatMonthDay(local);
  return '$day ${noticeClock(at)}';
}

/// "방금", "12분 전", "3시간 전", then the date.
String noticeAge(DateTime sentAt, DateTime now) {
  final passed = now.difference(sentAt);
  if (passed < const Duration(seconds: 60)) return l10n.justNow;
  if (passed < const Duration(minutes: 60)) return l10n.minAgo2(passed.inMinutes);
  if (passed < const Duration(hours: 24)) return l10n.text11(passed.inHours);
  final local = sentAt.toLocal();
  return formatMonthDay(local);
}
