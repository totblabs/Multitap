import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_settings_api.dart';

void main() {
  testWidgets(
    'Enrollment and display repair have bounded deadlines for native work',
    (tester) async {
      final failures = <String>[];
      for (final action in ['requestConnection', 'repairDisplay']) {
        unawaited(
          Completer<void>().future
              .timeout(hostSettingsRequestTimeout(action))
              .catchError((Object _) {
                failures.add(action);
              }),
        );
      }
      await tester.pump(const Duration(seconds: 89));
      expect(failures, isEmpty);
      await tester.pump(const Duration(seconds: 2));
      expect(failures, ['requestConnection']);
      await tester.pump(const Duration(seconds: 90));
      expect(failures, ['requestConnection', 'repairDisplay']);
    },
  );
  testWidgets(
    '75-second native display recovery survives the previous 35-second deadline',
    (tester) async {
      String? completed;
      Object? failure;
      unawaited(
        Future<String>.delayed(const Duration(seconds: 75), () => 'recovered')
            .timeout(hostSettingsRequestTimeout('recoverDisplay'))
            .then(
              (value) {
                completed = value;
              },
              onError: (Object error) {
                failure = error;
              },
            ),
      );
      await tester.pump(const Duration(seconds: 36));
      expect(completed, isNull);
      expect(failure, isNull);
      await tester.pump(const Duration(seconds: 40));
      expect(completed, 'recovered');
      expect(failure, isNull);
    },
  );

  testWidgets(
    'Hung recovery still has a bounded timeout and short health requests keep their limit',
    (tester) async {
      final recovery = Completer<void>();
      Object? recoveryFailure;
      Object? healthFailure;
      unawaited(
        recovery.future
            .timeout(hostSettingsRequestTimeout('recoverDisplay'))
            .catchError((Object error) {
              recoveryFailure = error;
            }),
      );
      unawaited(
        Completer<void>().future
            .timeout(hostSettingsRequestTimeout('testBroker'))
            .catchError((Object error) {
              healthFailure = error;
            }),
      );
      await tester.pump(const Duration(seconds: 36));
      expect(healthFailure, isA<TimeoutException>());
      expect(recoveryFailure, isNull);
      await tester.pump(const Duration(seconds: 85));
      expect(recoveryFailure, isA<TimeoutException>());
      recovery.complete();
      await tester.pump();
    },
  );
}
