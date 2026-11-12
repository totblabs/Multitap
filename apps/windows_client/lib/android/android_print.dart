import 'dart:async';
import 'dart:math';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart'
    show
        BuildContext,
        WidgetsBinding,
        WidgetsBindingObserver,
        AppLifecycleState;

import '../data/native_print.dart' show printCodeMessage;
import '../data/print_session.dart';
import '../data/product_log.dart';
import 'android_print_progress.dart';
import '../l10n/l10n.dart';

/// Android's own print dialog (android/.../AndroidPrint.kt, PrintManager)
/// for a PDF the VM printed: the user picks a printer or "Save as PDF".
abstract interface class AndroidPrintBridge {
  /// True when Android accepted the document into its print queue.
  /// This does not establish that a physical printer finished the job.
  Future<bool> print(PrintDocument document);
}

/// A fixed native failure, distinct from the user dismissing Android's dialog.
class AndroidPrintFailure implements Exception {
  const AndroidPrintFailure(this.code);
  final String code;
}

class NativeAndroidPrint implements AndroidPrintBridge {
  const NativeAndroidPrint();
  static const _channel = MethodChannel('multitap/android/print');
  static final _random = Random.secure();
  static final _pending = <String, int>{};

  static Future<Object?> _event(MethodCall call) async {
    if (call.method != 'print.event') return null;
    final value = call.arguments;
    if (value is! Map ||
        value.length != 3 ||
        !value.keys.toSet().containsAll(['requestId', 'sequence', 'stage'])) {
      return null;
    }
    final id = value['requestId'],
        sequence = value['sequence'],
        stage = value['stage'];
    if (id is! String ||
        !_pending.containsKey(id) ||
        sequence is! int ||
        sequence <= _pending[id]! ||
        sequence > 64 ||
        stage is! int ||
        stage < 1 ||
        stage > 9) {
      return null;
    }
    _pending[id] = sequence;
    ProductLog.instance.record(LogEvent.print, {
      'stage': stage,
      'ok': stage != 9,
    });
    return null;
  }

  @override
  Future<bool> print(PrintDocument document) async {
    if (_pending.length >= 16) throw const AndroidPrintFailure('print_failed');
    final id = List.generate(
      16,
      (_) => _random.nextInt(256).toRadixString(16).padLeft(2, '0'),
    ).join();
    _pending[id] = 0;
    _channel.setMethodCallHandler(_event);
    ProductLog.instance.record(LogEvent.print, {'stage': 0});
    try {
      final value = await _channel.invokeMethod<Object?>('print', {
        'path': document.file.path,
        'title': document.title,
        'requestId': id,
      });
      if (value is Map && value['ok'] == true && value['code'] == '') {
        ProductLog.instance.record(LogEvent.print, {'stage': 10, 'ok': true});
        return true;
      }
      if (value is Map && value['ok'] == false) {
        final code = value['code'];
        if (code == 'print_cancelled') {
          ProductLog.instance.record(LogEvent.print, {'stage': 11, 'ok': true});
          return false;
        }
        if (const {
          'print_document_invalid',
          'print_unavailable',
          'print_failed',
        }.contains(code)) {
          throw AndroidPrintFailure(code as String);
        }
      }
      throw const AndroidPrintFailure('print_failed');
    } on PlatformException {
      ProductLog.instance.record(LogEvent.print, {'stage': 12, 'ok': false});
      throw const AndroidPrintFailure('print_failed');
    } on MissingPluginException {
      ProductLog.instance.record(LogEvent.print, {'stage': 12, 'ok': false});
      throw const AndroidPrintFailure('print_unavailable');
    } catch (error) {
      ProductLog.instance.record(LogEvent.print, {'stage': 12, 'ok': false});
      rethrow;
    } finally {
      _pending.remove(id);
      if (_pending.isEmpty) _channel.setMethodCallHandler(null);
    }
  }
}

/// Offers each document a session's VM printed, one at a time, to Android's
/// print dialog, then tells the Host what happened (a fixed outcome) and
/// deletes the local copy. Nothing prints without the user's choice there.
class AndroidPrintOffers with WidgetsBindingObserver {
  AndroidPrintOffers({
    AndroidPrintBridge? bridge,
    this.onNotice,
    this.foregroundOnly = false,
    BuildContext? Function()? progressContext,
  }) : _bridge = bridge ?? const NativeAndroidPrint(),
       _progress = progressContext == null
           ? null
           : AndroidPrintProgress(context: progressContext) {
    if (foregroundOnly) WidgetsBinding.instance.addObserver(this);
  }
  final AndroidPrintBridge _bridge;
  final AndroidPrintProgress? _progress;
  final bool foregroundOnly;

  /// Told (a short Korean sentence) when the VM's printer could not
  /// be made, a document was skipped, or printing ended while the session goes
  /// on. Null: say nothing.
  final void Function(String message)? onNotice;
  bool _queueNoticed = false, _stopNoticed = false, _lostNoticed = false;
  PrintSession? _watched;
  bool _offering = false, _disposed = false;

  /// Follows the current session's print channel (null: none).
  void watch(PrintSession? printer) {
    if (identical(printer, _watched) || _disposed) return;
    _watched?.removeListener(_changed);
    _watched = printer?..addListener(_changed);
    _progress?.watch(printer);
    _queueNoticed = _stopNoticed = _lostNoticed = false;
    _changed();
  }

  /// Actionable failures only; a ready VM printer stays silent.
  void _notices(PrintSession printer) {
    final say = onNotice;
    if (say == null) return;
    if (!_queueNoticed && printer.queueReason != null) {
      _queueNoticed = true;
      say(printCodeMessage(printer.queueReason!));
    }
    final skipped = printer.skipped;
    if (skipped != null) {
      printer.clearSkipped();
      say(printCodeMessage(skipped));
    }
    if (printer.closed && printer.error != null) {
      // Not while the printer is coming back by itself (channel-resume-v1):
      // then only what is really lost is said, without "connect again".
      if (printer.recovering) {
        if (printer.lostDocuments > 0 && !_lostNoticed) {
          _lostNoticed = true;
          say(printLostNotice(printer.lostDocuments));
        }
      } else if (!_stopNoticed) {
        _stopNoticed = true;
        say(
          printer.error == 'print_policy_changed'
              ? printCodeMessage('print_policy_changed')
              : printer.lostDocuments > 0 && !_lostNoticed
              ? l10n.printerConnectionEnded(printer.lostDocuments)
              : l10n.printerConnectionEndedReconnectPrint,
        );
      }
    }
  }

  void _changed() {
    final printer = _watched;
    if (printer == null) return;
    _notices(printer);
    if (_offering || printer.closed) return;
    if (foregroundOnly &&
        WidgetsBinding.instance.lifecycleState != AppLifecycleState.resumed) {
      return;
    }
    if (printer.pending.isEmpty) return;
    _offering = true;
    unawaited(_offer(printer));
  }

  Future<void> _offer(PrintSession printer) async {
    PrintDocument? document;
    try {
      document = printer.pending.first;
      // Android's print screen reads the file after the channel may have
      // ended: it is kept until finished here.
      printer.hold(document);
      final written = await _bridge.print(document);
      final completed = document;
      document = null;
      await printer.finish(
        completed,
        written ? PrintOutcome.printed : PrintOutcome.cancelled,
      );
    } on AndroidPrintFailure catch (failure) {
      if (document != null && !_disposed && identical(printer, _watched)) {
        onNotice?.call(switch (failure.code) {
          'print_document_invalid' => l10n.couldntOpenReceivedPrintDocument,
          'print_unavailable' => l10n.androidPrintingIsntAvailableCheck,
          _ => l10n.couldntPassPrintDocumentPrint,
        });
      }
    } catch (_) {
      if (document != null && !_disposed && identical(printer, _watched)) {
        onNotice?.call(l10n.couldntPassPrintDocumentPrint);
      }
      /* Finished below; the session folder is removed with the session. */
    } finally {
      final unfinished = document;
      if (unfinished != null) {
        unawaited(printer.finish(unfinished, PrintOutcome.failed));
      }
      _offering = false;
      if (!_disposed && identical(printer, _watched)) _changed();
    }
  }

  void dispose() {
    _disposed = true;
    if (foregroundOnly) WidgetsBinding.instance.removeObserver(this);
    _progress?.dispose();
    _watched?.removeListener(_changed);
    _watched = null;
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed && !_disposed) _changed();
  }
}
