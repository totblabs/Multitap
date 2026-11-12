// ChannelRedial (channel_redial.dart): the bounded attempts of one side
// channel after a stream resumed (channel-resume-v1), and what counts as a
// transient end. Fake time; nothing is dialled for real.
import 'dart:async';
import 'dart:io';

import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/channel_redial.dart';

class _Channel {
  _Channel({this.enabled = true});
  bool enabled, away = false, started = true;
  int dials = 0, changes = 0;
  late final redial = ChannelRedial(
    enabled: () => enabled,
    away: () => away,
    dial: () async {
      dials++;
      return started;
    },
    changed: () => changes++,
  );
}

void main() {
  test('only a lost transport or an absent stream is a transient end', () {
    for (final kind in ['print', 'drive', 'usb']) {
      for (final suffix in [
        'transport_closed',
        'pair_timeout',
        'stream_away',
        'timeout',
      ]) {
        expect(channelEndTransient(kind, '${kind}_$suffix'), isTrue);
      }
      for (final code in [
        '${kind}_policy_changed',
        '${kind}_invalid_message',
        '${kind}_rate_limit',
        '${kind}_unavailable',
        'authorization_revoked',
        'session_ended',
        null,
      ]) {
        expect(channelEndTransient(kind, code), isFalse, reason: '$code');
      }
    }
    // Another channel's reason is not this channel's.
    expect(channelEndTransient('print', 'usb_stream_away'), isFalse);
  });

  test('a refused dial is final, "not now" and a lost network are not', () {
    WebSocketException http(int status) =>
        WebSocketException('refused', status);
    for (final status in [400, 401, 403, 404, 409]) {
      expect(channelDialRefused(http(status)), isTrue, reason: '$status');
    }
    for (final status in [408, 502, 503, 504]) {
      expect(channelDialRefused(http(status)), isFalse, reason: '$status');
    }
    expect(channelDialRefused(const WebSocketException('reset')), isFalse);
    expect(channelDialRefused(const SocketException('offline')), isFalse);
    expect(channelDialRefused(TimeoutException('slow')), isFalse);
  });

  test('the grant is exactly the marker', () {
    expect(
      channelResumeGranted({'channelResumeProtocol': 'channel-resume-v1'}),
      isTrue,
    );
    expect(channelResumeGranted({'channelResumeProtocol': true}), isFalse);
    expect(channelResumeGranted({'channelResumeProtocol': 'v2'}), isFalse);
    expect(channelResumeGranted(const {}), isFalse);
  });

  test('three attempts after 1, 2 and 4 seconds, then nothing by itself', () {
    fakeAsync((time) {
      final c = _Channel();
      for (final seconds in [1, 2, 4]) {
        c.redial.ended();
        expect(c.redial.recovering, isTrue);
        final before = c.dials;
        time.elapse(Duration(milliseconds: seconds * 1000 - 1));
        expect(c.dials, before);
        time.elapse(const Duration(milliseconds: 1));
        expect(c.dials, before + 1);
        expect(c.redial.recovering, isFalse, reason: 'the fresh one is up');
      }
      c.redial.ended();
      time.elapse(const Duration(seconds: 30));
      expect(c.dials, 3);
      expect(c.redial.recovering, isFalse);
      expect(c.redial.gaveUp, isTrue);
    });
  });

  test('a pair that worked starts the count over', () {
    fakeAsync((time) {
      final c = _Channel();
      for (var round = 0; round < 5; round++) {
        c.redial.ended();
        time.elapse(const Duration(seconds: 1));
        expect(c.dials, round + 1, reason: 'always the first delay');
        c.redial.paired();
        expect(c.redial.gaveUp, isFalse);
      }
    });
  });

  test('nothing is spent while the stream is away, and the resumed stream gets '
      'one fresh set', () {
    fakeAsync((time) {
      final c = _Channel()..away = true;
      for (var n = 0; n < 5; n++) {
        c.redial.ended();
      }
      time.elapse(const Duration(seconds: 60));
      expect(c.dials, 0);
      expect(c.redial.recovering, isTrue, reason: 'it waits for the stream');
      c.away = false;
      c.redial.resumed();
      for (final seconds in [1, 2, 4]) {
        time.elapse(Duration(seconds: seconds));
        c.redial.ended();
      }
      expect(c.dials, 3);
      time.elapse(const Duration(seconds: 30));
      expect(c.dials, 3);
      expect(c.redial.gaveUp, isTrue);
      // Attempts that ran out during an outage come back with the next resume.
      c.redial.resumed();
      time.elapse(const Duration(seconds: 1));
      expect(c.dials, 4);
    });
  });

  test('a delay that ends while the stream is away is not spent', () {
    fakeAsync((time) {
      final c = _Channel();
      c.redial.ended();
      c.away = true;
      time.elapse(const Duration(seconds: 5));
      expect(c.dials, 0);
      expect(c.redial.recovering, isTrue);
      c.away = false;
      c.redial.resumed();
      time.elapse(const Duration(seconds: 1));
      expect(c.dials, 1);
    });
  });

  test('a refusal ends it for good, away or not, and no resume brings it '
      'back', () {
    fakeAsync((time) {
      for (final away in [false, true]) {
        final c = _Channel()..away = away;
        c.redial.ended();
        c.redial.refused();
        expect(c.redial.recovering, isFalse);
        expect(c.redial.gaveUp, isTrue);
        c.away = false;
        c.redial.resumed();
        c.redial.ended();
        time.elapse(const Duration(seconds: 30));
        expect(c.dials, 0);
        expect(c.redial.recovering, isFalse);
      }
    });
  });

  test('a resume with a live channel starts nothing', () {
    fakeAsync((time) {
      final c = _Channel();
      c.redial.resumed();
      time.elapse(const Duration(seconds: 30));
      expect(c.dials, 0);
      expect(c.redial.recovering, isFalse);
      expect(c.redial.gaveUp, isFalse);
    });
  });

  test('an old channel that has not let go counts as an attempt and is tried '
      'again', () {
    fakeAsync((time) {
      final c = _Channel()..started = false;
      c.redial.ended();
      time.elapse(const Duration(seconds: 1));
      expect(c.dials, 1);
      expect(c.redial.recovering, isTrue);
      time.elapse(const Duration(seconds: 2));
      expect(c.dials, 2);
      c.started = true;
      time.elapse(const Duration(seconds: 4));
      expect(c.dials, 3);
      expect(c.redial.recovering, isFalse);
    });
  });

  test('where the protocol was not negotiated nothing is dialled: the channel '
      'only waits a moment for the stream to say it is away', () {
    fakeAsync((time) {
      // The stream goes on: after the moment the channel is simply ended.
      var c = _Channel(enabled: false);
      c.redial.ended();
      expect(c.redial.recovering, isTrue);
      time.elapse(const Duration(milliseconds: 1999));
      expect(c.redial.recovering, isTrue);
      time.elapse(const Duration(milliseconds: 1));
      expect(c.redial.recovering, isFalse);
      expect(c.redial.gaveUp, isTrue);
      expect(c.dials, 0);
      // The stream turns out to be away: quiet until it continues, then ended.
      c = _Channel(enabled: false);
      c.redial.ended();
      c.away = true;
      time.elapse(const Duration(seconds: 20));
      expect(c.redial.recovering, isTrue);
      c.away = false;
      c.redial.resumed();
      expect(c.redial.recovering, isFalse);
      expect(c.redial.gaveUp, isTrue);
      time.elapse(const Duration(seconds: 30));
      expect(c.dials, 0);
    });
  });

  test('reset forgets everything: nothing pending, a later channel counts '
      'from the start', () {
    fakeAsync((time) {
      final c = _Channel();
      c.redial.ended();
      c.redial.reset();
      time.elapse(const Duration(seconds: 30));
      expect(c.dials, 0);
      expect(c.redial.recovering, isFalse);
      expect(c.redial.gaveUp, isFalse);
      c.redial.ended();
      time.elapse(const Duration(seconds: 1));
      expect(c.dials, 1);
    });
  });
}
