import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/connection_feedback.dart';
import 'package:multitap_client/ui/connection_wait_status.dart';

void main() {
  testWidgets(
    'elapsed redraws without advancing the observed connection step',
    (t) async {
      var elapsed = const Duration(seconds: 59);
      var step = ConnectionWaitStep.request;
      Widget panel() => MaterialApp(
        home: Scaffold(
          body: ConnectionWaitStatus(
            step: step,
            attempt: 'attempt-1',
            elapsed: () => elapsed,
          ),
        ),
      );
      await t.pumpWidget(panel());
      expect(find.text('경과 00:59'), findsOneWidget);
      elapsed = const Duration(seconds: 61);
      await t.pump(const Duration(seconds: 1));
      expect(find.text('경과 01:01'), findsOneWidget);
      expect(find.text('연결 요청'), findsOneWidget);
      step = ConnectionWaitStep.firstFrame;
      await t.pumpWidget(panel());
      expect(find.text('첫 화면 수신 대기'), findsOneWidget);
      // Every stage is listed now: the one it is at carries this key.
      expect(
        t.widget<Text>(find.byKey(const Key('connection-wait-step'))).data,
        '첫 화면 수신 대기',
      );
      expect(find.text('경과 01:01'), findsOneWidget);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
      await t.pump(const Duration(seconds: 3));
      expect(t.takeException(), isNull);
    },
  );

  // No row is left empty above the running one, or checked below it. The
  // mark only tells where the connection is: everything above it is checked,
  // nothing below it is.
  testWidgets('rows above the stage it is at are checked, rows below never', (
    t,
  ) async {
    var step = ConnectionWaitStep.request;
    Widget panel() => MaterialApp(
      home: Scaffold(
        body: ConnectionWaitStatus(step: step, attempt: 'attempt-1'),
      ),
    );
    List<String> done() => [
      for (final stage in ConnectionWaitStatus.stages)
        if (find
            .byKey(ValueKey('connection-wait-done-${stage.name}'))
            .evaluate()
            .isNotEmpty)
          stage.name,
    ];
    await t.pumpWidget(panel());
    double top(String label) => t.getTopLeft(find.text(label)).dy;
    expect(top('연결 요청'), lessThan(top('서버 연결')));
    expect(top('서버 연결'), lessThan(top('가상 화면 준비')));
    expect(top('가상 화면 준비'), lessThan(top('첫 화면 수신 대기')));
    expect(done(), isEmpty);
    // The connection was made and the Host began setting the display before
    // a frame was drawn: 서버 연결 was never the current row, and is over.
    step = ConnectionWaitStep.display;
    await t.pumpWidget(panel());
    expect(done(), ['request', 'relay']);
    step = ConnectionWaitStep.firstFrame;
    await t.pumpWidget(panel());
    expect(done(), ['request', 'relay', 'display']);
    // A connection that starts over inside the attempt: nothing stays
    // checked below the row it is at.
    step = ConnectionWaitStep.relay;
    await t.pumpWidget(panel());
    expect(done(), ['request']);
    expect(find.byKey(const Key('connection-wait-now')), findsOneWidget);
    await t.pumpWidget(const SizedBox.shrink());
  });

  testWidgets('the rows follow the order the connection gives them', (t) async {
    // A caller's own order: the Host's virtual display before the server.
    const android = [
      ConnectionWaitStep.request,
      ConnectionWaitStep.display,
      ConnectionWaitStep.relay,
      ConnectionWaitStep.firstFrame,
    ];
    var step = ConnectionWaitStep.request;
    Widget panel({List<ConnectionWaitStep>? order}) => MaterialApp(
      home: Scaffold(
        body: ConnectionWaitStatus(
          step: step,
          attempt: 'attempt-1',
          order: order ?? ConnectionWaitStatus.stages,
        ),
      ),
    );
    double top(String label) => t.getTopLeft(find.text(label)).dy;
    await t.pumpWidget(panel());
    expect(top('서버 연결'), lessThan(top('가상 화면 준비')));
    await t.pumpWidget(panel(order: android));
    expect(top('가상 화면 준비'), lessThan(top('서버 연결')));
    expect(top('서버 연결'), lessThan(top('첫 화면 수신 대기')));
    // In that order 서버 연결 is the third row: the two above it are checked.
    step = ConnectionWaitStep.relay;
    await t.pumpWidget(panel(order: android));
    expect(
      t.widget<Text>(find.byKey(const Key('connection-wait-step'))).data,
      '서버 연결',
    );
    expect(find.byKey(const ValueKey('connection-wait-done-request')), findsOneWidget);
    expect(find.byKey(const ValueKey('connection-wait-done-display')), findsOneWidget);
    expect(find.byKey(const ValueKey('connection-wait-done-firstFrame')), findsNothing);
    await t.pumpWidget(const SizedBox.shrink());
    await t.pump(const Duration(seconds: 3));
  });
}