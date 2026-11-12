import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';

import 'user_data_path.dart';
import '../l10n/l10n.dart';

/// A printer of this PC.
class LocalPrinter {
  const LocalPrinter(this.name, {this.isDefault = false});
  final String name;
  final bool isDefault;
}

/// The outcome of one native print or save (fixed codes, never paths).
class LocalPrintResult {
  const LocalPrintResult(this.ok, {this.code = '', this.pages = 0});
  final bool ok;

  /// '' on success; print_cancelled when the user closed the save dialog;
  /// otherwise a fixed print_* failure code.
  final String code;
  final int pages;
  bool get cancelled => code == 'print_cancelled';
}

/// Pages of the running print handed to the printer so far (copies counted).
class LocalPrintProgress {
  const LocalPrintProgress(this.done, this.total);
  final int done, total;
  bool get known => total > 0;
}

/// Original PDF page, rendered locally in memory without starting a print job.
class LocalPrintPreview {
  const LocalPrintPreview({
    required this.png,
    required this.width,
    required this.height,
    required this.page,
    required this.pages,
  });
  final Uint8List png;
  final double width, height;
  final int page, pages;
}

/// Optional capability, so existing platform print bridges remain compatible.
abstract interface class LocalPrintPreviewBridge {
  Future<LocalPrintPreview?> preview(String path, {required int page});
}

/// Printing on this PC (windows/runner/client_print.cpp): the PDF is opened
/// with Windows' own PDF renderer (Windows.Data.Pdf) and sent to the chosen
/// printer through the Direct2D print control (XPS print path, vector).
abstract interface class LocalPrintBridge {
  Future<List<LocalPrinter>> printers();

  /// The number of pages, or null when Windows cannot open the document.
  Future<int?> pages(String path);
  Future<LocalPrintResult> print(
    String path, {
    required String printer,
    required int copies,
    required String title,
  });

  /// Asks where to save (Windows' save dialog, [suggestedName]) and copies.
  Future<LocalPrintResult> save(String path, {required String suggestedName});

  /// Page progress of the print now running, or null when none is known
  /// (nothing running, or a build without progress).
  Future<LocalPrintProgress?> progress();

  /// What is wrong with [printer] right now: '' when nothing is known to be
  /// (or it cannot be asked), else print_printer_offline, print_printer_paused,
  /// print_printer_attention (paper, toner, cover) or print_printer_error.
  /// Printing is never blocked by it: the job waits in the printer's queue.
  Future<String> status(String printer);
}

class NativeLocalPrint implements LocalPrintBridge, LocalPrintPreviewBridge {
  const NativeLocalPrint();
  static const _channel = MethodChannel('multitap/print');
  static const maximumCopies = 20;

  static LocalPrintResult _result(Object? value) {
    if (value is! Map) {
      return const LocalPrintResult(false, code: 'print_failed');
    }
    final code = value['code'];
    return LocalPrintResult(
      value['ok'] == true,
      code: code is String && RegExp(r'^(|print_[a-z_]{1,40})$').hasMatch(code)
          ? code
          : 'print_failed',
      pages: value['pages'] is int ? value['pages'] as int : 0,
    );
  }

  @override
  Future<List<LocalPrinter>> printers() async {
    try {
      final value = await _channel.invokeMethod<List<Object?>>('printers');
      return [
        for (final item in value ?? const <Object?>[])
          if (item is Map &&
              item['name'] is String &&
              (item['name'] as String).isNotEmpty)
            LocalPrinter(
              item['name'] as String,
              isDefault: item['default'] == true,
            ),
      ];
    } on PlatformException {
      return const [];
    } on MissingPluginException {
      return const [];
    }
  }

  @override
  Future<int?> pages(String path) async {
    try {
      final value = await _channel.invokeMethod<int>('pages', {'path': path});
      return value != null && value > 0 ? value : null;
    } on PlatformException {
      return null;
    } on MissingPluginException {
      return null;
    }
  }

  @override
  Future<LocalPrintPreview?> preview(String path, {required int page}) async {
    if (page < 0 || page >= 10000) return null;
    try {
      final value = await _channel.invokeMethod<Object?>('preview', {
        'path': path,
        'page': page,
      });
      if (value is! Map) return null;
      final png = value['png'],
          width = value['width'],
          height = value['height'];
      final pages = value['pages'];
      if (png is! Uint8List ||
          png.isEmpty ||
          png.length > 5 * 1024 * 1024 ||
          width is! num ||
          height is! num ||
          !width.isFinite ||
          !height.isFinite ||
          width <= 1 ||
          height <= 1 ||
          width >= 100000 ||
          height >= 100000 ||
          value['page'] != page ||
          pages is! int ||
          pages <= page ||
          pages > 10000) {
        return null;
      }
      return LocalPrintPreview(
        png: png,
        width: width.toDouble(),
        height: height.toDouble(),
        page: page,
        pages: pages,
      );
    } on PlatformException {
      return null;
    } on MissingPluginException {
      return null;
    }
  }

  @override
  Future<LocalPrintResult> print(
    String path, {
    required String printer,
    required int copies,
    required String title,
  }) async {
    try {
      return _result(
        await _channel.invokeMethod<Object?>('print', {
          'path': path,
          'printer': printer,
          'copies': copies.clamp(1, maximumCopies),
          'title': title,
        }),
      );
    } on PlatformException catch (error) {
      return LocalPrintResult(false, code: _code(error.code));
    } on MissingPluginException {
      return const LocalPrintResult(false, code: 'print_unavailable');
    }
  }

  @override
  Future<LocalPrintResult> save(
    String path, {
    required String suggestedName,
  }) async {
    try {
      return _result(
        await _channel.invokeMethod<Object?>('save', {
          'path': path,
          'name': suggestedName,
        }),
      );
    } on PlatformException catch (error) {
      return LocalPrintResult(false, code: _code(error.code));
    } on MissingPluginException {
      return const LocalPrintResult(false, code: 'print_unavailable');
    }
  }

  @override
  Future<String> status(String printer) async {
    try {
      final value = await _channel
          .invokeMethod<Object?>('status', {'printer': printer})
          .timeout(const Duration(seconds: 6));
      return value is String &&
              RegExp(r'^(|print_printer_[a-z]{1,20})$').hasMatch(value)
          ? value
          : '';
    } on PlatformException {
      return '';
    } on MissingPluginException {
      return '';
    } on TimeoutException {
      return '';
    }
  }

  @override
  Future<LocalPrintProgress?> progress() async {
    try {
      final value = await _channel.invokeMethod<Object?>('progress');
      if (value is! Map) return null;
      final done = value['done'], total = value['total'];
      if (done is! int || total is! int || total < 1 || done < 0) return null;
      return LocalPrintProgress(done > total ? total : done, total);
    } on PlatformException {
      return null;
    } on MissingPluginException {
      return null;
    }
  }

  static String _code(String code) =>
      RegExp(r'^print_[a-z_]{1,40}$').hasMatch(code) ? code : 'print_failed';
}

/// The printer last used for a VM document, per Windows user
/// (%LOCALAPPDATA%\Multitap\Client\print.json, a name only).
class PrintMemory {
  PrintMemory(this.file);
  final File file;

  factory PrintMemory.windows() {
    final appData = Platform.environment['LOCALAPPDATA'];
    if (appData == null || appData.isEmpty) {
      throw StateError('Windows app data directory is unavailable.');
    }
    return PrintMemory(File('$appData\\Multitap\\Client\\print.json'));
  }

  Future<String?> load() async {
    try {
      if (await file.length() > 4096) return null;
      final value = jsonDecode(await file.readAsString());
      if (value is! Map || value.length != 1 || value['printer'] is! String) {
        return null;
      }
      final name = value['printer'] as String;
      return name.isEmpty || name.length > 512 ? null : name;
    } on FileSystemException {
      return null;
    } on FormatException {
      return null;
    }
  }

  Future<void> save(String printer) async {
    if (printer.isEmpty || printer.length > 512) return;
    try {
      await writeUserDataFile(file, jsonEncode({'printer': printer}));
    } on FileSystemException {
      /* Remembering is a convenience. */
    }
  }
}

/// Korean text for the fixed print codes the user can meet.
String printCodeMessage(String code) => switch (code) {
  'print_job_too_large' => l10n.documentTooLargeOver256,
  'print_too_many_waiting' => l10n.tooManyPrintJobsStill,
  // The Host could not create or write its temporary copy (disk full, or
  // the protected folder refused): not only a space problem.
  'print_storage_unavailable' =>
    l10n.couldntSaveDocumentTemporarilyDesktop,
  'print_job_failed' => l10n.couldntReceiveDocumentPrintAgain,
  // Each fix is in the VM, then a new connection (the Host checks at connect).
  'print_driver_unavailable' =>
    l10n.microsoftPrintPdfMissingDesktop,
  'print_spooler_unavailable' =>
    l10n.printSpoolerServiceOffDesktop,
  'print_name_conflict' =>
    l10n.anotherPrinterSameNameExists,
  'print_setup_failed' => l10n.couldntCreatePrinterDesktop,
  'print_policy_changed' => l10n.administratorTurnedOffPrinterPermission,
  'print_printer_unavailable' =>
    l10n.cantFindSelectedPrinterCheck,
  'print_printer_offline' => l10n.printerOfflineQueuedDocumentsPrint,
  'print_printer_paused' => l10n.printerPausedResumeWindowsPrint,
  'print_printer_attention' => l10n.printerNeedsAttentionPaperToner,
  'print_printer_error' => l10n.printerHasProblemCheckPrinter,
  'print_local_storage_failed' => l10n.storageFullNotWritableSo,
  'print_cancelled' => l10n.printingCanceled,
  'print_document_invalid' => l10n.couldntOpenDocumentSoWasnt,
  'print_unavailable' => l10n.printingIsntAvailablePc,
  'print_save_failed' => l10n.couldntSaveFile,
  _ => l10n.couldntPrint,
};
