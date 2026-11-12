import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/window_presentation.dart';

void main() {
  testWidgets(
    'A native maximize request reaches the opted-in presentation, and disposal opts out',
    (t) async {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const window = MethodChannel('multitap/window'),
          events = MethodChannel('multitap/window/events');
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(window, (call) async {
        calls.add(call);
        return call.method == 'fullscreen' ? call.arguments : null;
      });
      messenger.setMockMethodCallHandler(events, (_) async => null);
      addTearDown(() {
        messenger.setMockMethodCallHandler(window, null);
        messenger.setMockMethodCallHandler(events, null);
      });
      final presentation = WindowPresentation();
      var requested = 0;
      presentation.onMaximizeRequested = () => requested++;
      await presentation.enter(fullscreen: false);
      await presentation.maximizeToFullscreen(true);
      expect(
        calls.map((c) => '${c.method}:${c.arguments}'),
        containsAllInOrder(['fullscreen:false', 'maximizeToFullscreen:true']),
      );
      Future<void> emit(Object event) => messenger.handlePlatformMessage(
        'multitap/window/events',
        const StandardMethodCodec().encodeSuccessEnvelope(event),
        (_) {},
      );
      await emit({'type': 'maximizeRequested'});
      await t.pump();
      expect(requested, 1);
      // A fullscreen state event is still a plain boolean.
      await emit(true);
      await t.pump();
      expect(presentation.fullscreen, isTrue);
      expect(requested, 1);
      presentation.dispose();
      await t.pump();
      expect(
        calls.map((c) => '${c.method}:${c.arguments}'),
        containsAllInOrder(['restore:null', 'maximizeToFullscreen:false']),
      );
      await emit({'type': 'maximizeRequested'});
      await t.pump();
      expect(requested, 1, reason: 'a disposed presentation ignores requests');
    },
  );
}
