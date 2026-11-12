import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_transport.dart';

// Executes only when an explicitly supplied, synthetic loopback fixture exists.
// No installed application, credentials, native windows or production services.
void main() {
  const path = String.fromEnvironment('BROKER_PAIR_FIXTURE');
  test(
    'production HTTP login confirmation and idle policy pair',
    () async {
      final fixture = jsonDecode(await File(path).readAsString()) as Map;
      expect(fixture['kind'], 'isolated-session-access-http-fixture');
      final origin = Uri.parse(fixture['origin'] as String);
      expect(origin.scheme, 'http');
      expect(origin.host, '127.0.0.1');
      final approval = BrokerTransport.approve(origin, origin.origin);
      addTearDown(approval.release);
      final a = HttpBrokerApi(origin: origin, requestPcLoginReplacement: true);
      final b = HttpBrokerApi(origin: origin, requestPcLoginReplacement: true);
      addTearDown(a.close);
      addTearDown(b.close);
      final username = fixture['username'] as String;
      final password = fixture['password'] as String;
      final first = await a.loginInteractively(username, password);
      expect(first.sessionAccess, isNotNull);
      expect(first.sessionAccess!.idleEnabled, isTrue);
      expect(
        first.withExpiry(first.expiresAt).sessionAccess,
        same(first.sessionAccess),
      );
      Future<LoginConfirmation> conflict() async {
        try {
          await b.loginInteractively(username, password);
          fail('an existing login must require explicit confirmation');
        } on LoginConfirmationRequired catch (required) {
          return required.challenge;
        }
      }

      final cancelled = await conflict();
      cancelled.forget();
      expect(await a.renewSession(first.token), isA<DateTime>());
      final second = await b.confirmLogin(await conflict());
      await expectLater(
        a.renewSession(first.token),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'reason',
            'pc_login_replaced',
          ),
        ),
      );
      await expectLater(
        a.login(username, password),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'reason',
            'session_reauthentication_required',
          ),
        ),
      );
      expect(await b.renewSession(second.token), isA<DateTime>());
      final off = await b.setIdleEnabled(second.token, false);
      expect(off.idleEnabled, isFalse);
      expect(off.idleDeadline, isNull);
      expect(off.remaining, isNull);
      final stillOff = await b.recordHumanActivity(second.token);
      expect(stillOff.idleEnabled, isFalse);
      final on = await b.setIdleEnabled(second.token, true);
      expect(on.idleEnabled, isTrue);
      expect(on.remaining, isNotNull);
      expect(on.remaining!.inMilliseconds, greaterThan(0));
      final activity = await b.recordHumanActivity(second.token);
      expect(activity.idleEnabled, isTrue);
      expect(activity.idleDeadline, isNotNull);
    },
    skip: path.isEmpty ? 'explicit synthetic loopback fixture required' : false,
  );
}
