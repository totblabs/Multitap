import 'dart:async';

import 'package:flutter_test/flutter_test.dart';

import 'usb_redirection_test.dart' show settle, shared, started;

void main() {
  test(
    'USB channel retirement is visible while native release is pending',
    () async {
      final (session, socket, backend) = await started();
      final (_, capture) = await shared(session, socket, backend);
      final release = Completer<void>();
      capture.releaseGate = release;
      var notifiedClosedBeforeRelease = false;
      session.addListener(() {
        if (session.closed && !capture.released) {
          notifiedClosedBeforeRelease = true;
        }
      });
      final closing = session.close();
      try {
        expect(session.closed, isTrue);
        expect(capture.released, isFalse);
        expect(notifiedClosedBeforeRelease, isTrue);
      } finally {
        release.complete();
        await closing;
        await settle();
      }
      expect(capture.released, isTrue);
    },
  );
}
