import 'package:flutter/services.dart';
import 'package:flutter/foundation.dart';

/// Finishes this app's Activity and removes its recent-app task, never a VM.
abstract final class AndroidApplication {
  static const channel = MethodChannel('multitap/android/application');

  /// The native activity sends only input class, never key text or device IDs.
  static bool hardwareNavigation = false;

  /// Visibility, independent of the keyboard's occluding height (TV overlay IME).
  static final imeVisible = ValueNotifier<bool>(false);
  static final imeInsetVisible = ValueNotifier<bool>(false);
  static Future<void> close() => channel.invokeMethod<void>('close');
}
