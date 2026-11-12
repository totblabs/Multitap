import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'android_stream_preparation_test.dart' show FakeApi;

class _SigninApi extends FakeApi implements SigninStreamBrokerApi {
  final pending = Completer<BrokerConnection>();
  int starts = 0;

  _SigninApi() {
    inventory = () async => [
      BrokerDesktop(
        id: 'pve-170',
        name: 'Synthetic',
        kind: 'vm',
        description: '',
        os: 'Windows',
        status: 'awaiting_host',
        canConnect: false,
        canStream: false,
        hostConnected: true,
        signinAvailability: SigninAvailability(
          id: 'a' * 32,
          windowsState: 'no-user',
          expiresAt: DateTime.now().add(const Duration(seconds: 5)),
        ),
      ),
    ];
  }

  @override
  Future<BrokerConnection> connectSignin(
    String token,
    String desktopId, {
    required String availabilityId,
    StreamOptions? options,
    InitialViewport? initialViewport,
  }) {
    starts++;
    requested = options;
    return pending.future;
  }
}

BrokerConnection _grant(Future<void> Function() release) => BrokerConnection(
  sessionId: 'synthetic-signin-reservation',
  webSocketUrl: Uri.parse('wss://fixture.invalid/signin'),
  reservation: BrokerSessionReservation(release),
);

void main() {
  testWidgets('Android sign-in hands off exactly one current reservation', (
    t,
  ) async {
    final api = _SigninApi();
    var releases = 0;
    final grant = _grant(() async {
      releases++;
    });
    final pending = AndroidStreamPreparation(
      api: api,
      authorized: () => true,
    ).connect('synthetic', 'pve-170', const StreamOptions(audio: true));
    await t.pump();
    expect(api.starts, 1);
    api.pending.complete(grant);
    expect(await pending, same(grant));
    expect(releases, 0);
    expect(
      api.requested!.audio,
      isTrue,
      reason: 'the broker masks this grant; user intent remains unchanged',
    );
    await grant.release();
    await grant.release();
    expect(releases, 1);
    expect(api.starts, 1);
  });

  testWidgets(
    'Android retired auth releases exact returned sign-in grant once',
    (t) async {
      final api = _SigninApi();
      var current = true;
      var releases = 0;
      Object? error;
      unawaited(
        AndroidStreamPreparation(api: api, authorized: () => current)
            .connect('synthetic', 'pve-170', const StreamOptions())
            .then<void>(
              (_) {},
              onError: (Object value) {
                error = value;
              },
            ),
      );
      await t.pump();
      expect(api.starts, 1);
      current = false;
      api.pending.complete(
        _grant(() async {
          releases++;
        }),
      );
      await t.pump();
      expect(error, isA<BrokerException>());
      expect((error as BrokerException).code, 'session_expired');
      expect(releases, 1);
      await t.pump(const Duration(seconds: 1));
      expect(api.starts, 1);
      expect(releases, 1);
    },
  );

  testWidgets(
    'Android timeout releases late sign-in grant without another POST',
    (t) async {
      final api = _SigninApi();
      var releases = 0;
      Object? error;
      unawaited(
        AndroidStreamPreparation(
              api: api,
              authorized: () => true,
              timeout: const Duration(milliseconds: 50),
            )
            .connect('synthetic', 'pve-170', const StreamOptions())
            .then<void>(
              (_) {},
              onError: (Object value) {
                error = value;
              },
            ),
      );
      await t.pump();
      expect(api.starts, 1);
      await t.pump(const Duration(milliseconds: 100));
      expect(error, isA<BrokerException>());
      api.pending.complete(
        _grant(() async {
          releases++;
        }),
      );
      await t.pump();
      expect(releases, 1);
      await t.pump(const Duration(seconds: 1));
      expect(api.starts, 1);
      expect(releases, 1);
    },
  );

  testWidgets('Android retired owner never starts a sign-in reservation', (
    t,
  ) async {
    final api = _SigninApi();
    await expectLater(
      AndroidStreamPreparation(
        api: api,
        authorized: () => false,
      ).connect('synthetic', 'pve-170', const StreamOptions()),
      throwsA(isA<BrokerException>()),
    );
    expect(api.starts, 0);
    expect(api.calls, isEmpty);
  });
}
