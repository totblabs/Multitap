import 'package:flutter/material.dart';

import '../data/native_print.dart';
import '../data/print_preview.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

class PrintPreview extends StatefulWidget {
  const PrintPreview({
    super.key,
    required this.path,
    required this.pages,
    required this.bridge,
    required this.enabled,
  });
  final String path;
  final int pages;
  final LocalPrintPreviewBridge bridge;
  final bool enabled;

  @override
  State<PrintPreview> createState() => _PrintPreviewState();
}

class _PrintPreviewState extends State<PrintPreview> {
  late PrintPreviewController _controller;
  MemoryImage? _image;

  void _releaseImage() {
    final image = _image;
    _image = null;
    if (image != null) {
      // Drop only this page's cached copy. The outgoing widget keeps its live
      // reference until disposal; preserve tracking of all live/other images.
      PaintingBinding.instance.imageCache.evict(image, includeLive: false);
    }
  }

  void _create() {
    _controller = PrintPreviewController(
      bridge: widget.bridge,
      path: widget.path,
      pages: widget.pages,
    )..addListener(_changed);
    _controller.load();
  }

  void _changed() {
    final png = _controller.preview?.png;
    if (!identical(png, _image?.bytes)) {
      _releaseImage();
      if (png != null) _image = MemoryImage(png);
    }
    if (mounted) setState(() {});
  }

  @override
  void initState() {
    super.initState();
    _create();
  }

  @override
  void didUpdateWidget(covariant PrintPreview oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.path != widget.path ||
        oldWidget.pages != widget.pages ||
        oldWidget.bridge != widget.bridge) {
      _controller.dispose();
      _create();
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    _releaseImage();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context), controller = _controller;
    final preview = controller.preview;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            Expanded(
              child: KeepAllText(
                l10n.previews,
                style: uiText(
                  size: 13.5,
                  weight: FontWeight.w600,
                  color: p.ink,
                ),
              ),
            ),
            IconButton(
              key: const Key('print-preview-previous'),
              tooltip: l10n.previousPage,
              onPressed: widget.enabled && controller.page > 0
                  ? () => controller.select(controller.page - 1)
                  : null,
              icon: const Icon(Icons.chevron_left_rounded),
            ),
            Text(
              '${controller.page + 1} / ${widget.pages}',
              key: const Key('print-preview-page'),
              style: uiText(size: 13, color: p.ink),
            ),
            IconButton(
              key: const Key('print-preview-next'),
              tooltip: l10n.nextPage,
              onPressed: widget.enabled && controller.page + 1 < widget.pages
                  ? () => controller.select(controller.page + 1)
                  : null,
              icon: const Icon(Icons.chevron_right_rounded),
            ),
          ],
        ),
        Container(
          key: const Key('print-preview-surface'),
          height: (MediaQuery.sizeOf(context).height * .30).clamp(120.0, 260.0),
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            color: p.quiet,
            borderRadius: BorderRadius.circular(10),
            border: Border.all(color: p.tileLine),
          ),
          child: controller.loading
              ? const Center(
                  child: SizedBox.square(
                    dimension: 24,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  ),
                )
              : preview == null
              ? _failure(p)
              : Image(
                  image: _image!,
                  key: ValueKey('print-preview-image-${preview.page}'),
                  fit: BoxFit.contain,
                  gaplessPlayback: false,
                  semanticLabel: l10n.pagePreview(preview.page + 1),
                  errorBuilder: (_, _, _) => _failure(p),
                ),
        ),
        const SizedBox(height: 6),
        KeepAllText(
          l10n.eachPageKeepsItsOriginal,
          style: uiText(size: 12, color: p.muted),
        ),
      ],
    );
  }

  Widget _failure(AppPalette p) => Center(
    child: Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        KeepAllText(l10n.couldNotOpenPreview, style: uiText(size: 13, color: p.muted)),
        TextButton(
          key: const Key('print-preview-retry'),
          onPressed: widget.enabled ? _controller.load : null,
          child: KeepAllText(l10n.tryAgain),
        ),
      ],
    ),
  );
}
