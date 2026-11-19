import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// The bits of an IME observation: what the Windows runner saw in the Flutter
/// view when the sign-in screen's ID field got the focus (PasswordIme::Observe
/// in windows/runner/password_ime.h). A wire format: the sum is the `error`
/// value of a `signIn` stage 3 diagnostics row.
abstract final class InputMethodState {
  /// The view had an input context when asked.
  static const context = 1;

  /// The IME is open; Hangul (native) mode. Both 0 without a context.
  static const open = 2, hangul = 4;

  /// The view has the Win32 keyboard focus; its window is the foreground one.
  static const viewFocused = 8, windowForeground = 16;

  /// Password mode is requested; the context is held away by it.
  static const passwordRequested = 32, passwordHeld = 64;

  /// The thread's keyboard layout is Korean.
  static const koreanLayout = 128;

  /// The context was missing outside password mode and the runner put it back.
  static const repaired = 256;

  /// Added by the sign-in screen, not the runner: it showed an error line
  /// when the field got the focus, which is how it looks after a forced
  /// sign-out (a failed sign-in leaves one too, until the next edit).
  static const afterSignOut = 512;

  /// What the IME did with the keys since the previous observation: a
  /// composition began or changed; it took a key (VK_PROCESSKEY); a Latin
  /// letter arrived while the context said open and Hangul (the keys went
  /// past the IME: the field records of 2026-11-19 said Hangul yet the owner
  /// typed Latin only).
  static const composed = 1024, keyTaken = 2048, latinWhileHangul = 4096;

  /// Added by the sign-in screen: observed when the ID field lost the focus
  /// (the typing bits then cover the time it had it).
  static const onLeaving = 8192;

  static const typing = composed | keyTaken | latinWhileHangul;
}

/// Asks the runner once; it also puts a missing input context back. Null
/// where there is nothing to ask: not Windows, no runner (tests), a failed
/// call or an answer that is not a number.
Future<int?> readInputMethodState() async {
  if (defaultTargetPlatform != TargetPlatform.windows) return null;
  try {
    final Object? bits = await const MethodChannel(
      'multitap/input',
    ).invokeMethod<Object?>('inputMethodState');
    return bits is int ? bits : null;
  } on MissingPluginException {
    return null;
  } on PlatformException {
    return null;
  }
}

/// One observation and when it was made (UTC).
class InputMethodObservation {
  InputMethodObservation(this.bits, DateTime time) : time = time.toUtc();
  final int bits;
  final DateTime time;

  /// How many wait for a sign-in at most. The first ones are the evidence
  /// (the state right after the sign-out), so a full list replaces its newest.
  static const kept = 4;

  Map<String, Object> toJson() => {
    'bits': bits,
    'time': time.toIso8601String(),
  };

  /// What a saved file may hold: at most [kept] well-formed entries.
  static List<InputMethodObservation> listFrom(Object? raw) {
    final result = <InputMethodObservation>[];
    if (raw is! List) return result;
    for (final item in raw) {
      if (result.length >= kept) break;
      final bits = item is Map ? item['bits'] : null;
      final text = item is Map ? item['time'] : null;
      final time = text is String && text.length <= 40
          ? DateTime.tryParse(text)
          : null;
      if (bits is int && bits >= 0 && bits <= 0xffff && time != null) {
        result.add(InputMethodObservation(bits, time));
      }
    }
    return result;
  }
}

/// Keeps the observations no sign-in has uploaded yet across a restart: when
/// Korean cannot be typed the app is closed and opened again, and that run
/// never signs in.
abstract interface class InputMethodObservationStore {
  Future<List<InputMethodObservation>> loadInputMethodObservations();

  /// Replaces what is kept; an empty list keeps nothing.
  Future<void> saveInputMethodObservations(List<InputMethodObservation> seen);
}
