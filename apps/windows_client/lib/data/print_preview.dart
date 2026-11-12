import 'dart:async';

import 'package:flutter/foundation.dart';

import 'native_print.dart';

/// Keeps only the selected page and one actual request in flight. A late page
/// cannot replace a newer selection, and closing never waits for the renderer.
class PrintPreviewController extends ChangeNotifier {
  PrintPreviewController({
    required this.bridge,
    required this.path,
    required this.pages,
  });
  final LocalPrintPreviewBridge bridge;
  final String path;
  final int pages;
  int page = 0;
  bool loading = false, failed = false;
  LocalPrintPreview? preview;
  int _revision = 0;
  bool _inFlight = false, _disposed = false;

  void load() => _request(page);

  void select(int index) {
    if (index < 0 || index >= pages || index == page) return;
    _request(index);
  }

  void _request(int index) {
    if (_disposed || pages < 1) return;
    page = index;
    _revision++;
    loading = true;
    failed = false;
    preview = null;
    notifyListeners();
    if (!_inFlight) unawaited(_drain());
  }

  Future<void> _drain() async {
    final revision = _revision, index = page;
    _inFlight = true;
    LocalPrintPreview? value;
    try {
      value = await bridge.preview(path, page: index);
    } catch (_) {
      // Preview failure never starts or cancels printing, and carries no text
      // or document contents into diagnostics.
    }
    _inFlight = false;
    if (_disposed) return;
    if (revision != _revision) {
      unawaited(_drain());
      return;
    }
    preview = value?.page == index && value?.pages == pages ? value : null;
    failed = preview == null;
    loading = false;
    notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    preview = null;
    super.dispose();
  }
}
