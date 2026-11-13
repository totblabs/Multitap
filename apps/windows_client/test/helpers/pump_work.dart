import 'package:flutter_test/flutter_test.dart';

/// [WidgetTester.pumpAndSettle] for a screen that shows work as under way.
///
/// A desktop that is starting, a device or folder being connected and an
/// open wait keep a mark turning for as long as the work lasts, so such a
/// screen never settles and `pumpAndSettle` runs into its time limit. This
/// lets everything else finish (routes, menus, dialogs, answers already on
/// their way) and returns as soon as no frame is scheduled, exactly like
/// `pumpAndSettle`, or after [limit] while a mark keeps turning.
Future<void> pumpWork(
  WidgetTester tester, {
  Duration limit = const Duration(seconds: 1),
}) async {
  const step = Duration(milliseconds: 100);
  await tester.pump();
  for (
    var waited = Duration.zero;
    tester.binding.hasScheduledFrame && waited < limit;
    waited += step
  ) {
    await tester.pump(step);
  }
}
