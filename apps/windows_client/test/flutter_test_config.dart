import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/l10n/l10n.dart';

/// Every test runs in Korean, the source text the existing tests pin,
/// unless it switches the language itself (test/i18n/*). The test platform
/// reports en-US, so "auto" is pointed at Korean here and reset after each
/// test.
///
/// Text audit (off unless asked for): with
///   `--dart-define=WRAP_AUDIT=folder` (`--dart-define=WRAP_SIZE=360x780`,
///   `--dart-define=WRAP_LANG=en`)
/// every frame any test draws is searched for text that takes two lines or
/// more, or is cut short, and the findings of each test file are written to
/// that folder as JSON. Text is measured with the app's own font (Pretendard),
/// not the test font whose every glyph is a square. A test that sets no screen
/// size of its own gets the phone size given. Tests may fail in this mode (another size, another
/// language); it is for reading the findings, not for passing.
Future<void> testExecutable(FutureOr<void> Function() testMain) async {
  const audit = String.fromEnvironment('WRAP_AUDIT');
  const language = String.fromEnvironment('WRAP_LANG', defaultValue: 'ko');
  AppLanguageController.debugSystemLanguage = audit.isEmpty ? 'ko' : language;
  setUp(() {
    AppLanguageController.debugSystemLanguage = audit.isEmpty ? 'ko' : language;
    AppLanguageController.instance.debugReset();
  });
  if (audit.isNotEmpty) _TextAudit(audit).install();
  await testMain();
}

class _TextAudit {
  _TextAudit(this.folder);
  final String folder;
  final _found = <String, Map<String, Object?>>{};
  bool _watching = false;

  static Size get _phone {
    final parts = const String.fromEnvironment(
      'WRAP_SIZE',
      defaultValue: '360x780',
    ).split('x');
    return Size(double.parse(parts[0]), double.parse(parts[1]));
  }

  void install() {
    setUpAll(() async {
      final root =
          Platform.environment['FLUTTER_ROOT'] ??
          '${Directory.current.path}/../../.tools/flutter';
      final families = {
        'Pretendard': [
          for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold'])
            'assets/fonts/Pretendard-$weight.otf',
        ],
        'MaterialIcons': [
          '$root/bin/cache/artifacts/material_fonts/MaterialIcons-Regular.otf',
        ],
      };
      for (final family in families.entries) {
        final loader = FontLoader(family.key);
        for (final path in family.value) {
          final bytes = File(path).readAsBytesSync();
          loader.addFont(Future.value(ByteData.sublistView(bytes)));
        }
        await loader.load();
      }
    });
    setUp(() {
      final binding = TestWidgetsFlutterBinding.ensureInitialized();
      final view = binding.platformDispatcher.views.first;
      view.devicePixelRatio = 1;
      view.physicalSize = _phone;
      addTearDown(view.reset);
      if (!_watching) {
        _watching = true;
        // After the binding's own callback: this frame is laid out.
        binding.addPersistentFrameCallback((_) => _scan(binding));
      }
    });
    tearDownAll(_write);
  }

  void _scan(TestWidgetsFlutterBinding binding) {
    String test = '';
    try {
      test = (binding as dynamic).testDescription as String;
    } catch (_) {}
    for (final view in binding.renderViews) {
      final screen = view.size;
      void visit(RenderObject object) {
        if (object is RenderParagraph && object.hasSize && object.attached) {
          _paragraph(object, screen, test);
        }
        object.visitChildren(visit);
      }

      visit(view);
    }
  }

  void _paragraph(RenderParagraph paragraph, Size screen, String test) {
    final text = paragraph.text.toPlainText().trim();
    if (text.isEmpty) return;
    final constraints = paragraph.constraints;
    if (!constraints.maxWidth.isFinite || constraints.maxWidth <= 0) return;
    final painter = TextPainter(
      text: paragraph.text,
      textAlign: paragraph.textAlign,
      textDirection: paragraph.textDirection,
      textScaler: paragraph.textScaler,
      maxLines: paragraph.maxLines,
      ellipsis: paragraph.overflow == TextOverflow.ellipsis ? '…' : null,
      locale: paragraph.locale,
      strutStyle: paragraph.strutStyle,
      textWidthBasis: paragraph.textWidthBasis,
      textHeightBehavior: paragraph.textHeightBehavior,
    )..layout(minWidth: constraints.minWidth, maxWidth: constraints.maxWidth);
    final lines = painter.computeLineMetrics().length;
    final cut = painter.didExceedMaxLines;
    painter.dispose();
    if (lines < 2 && !cut) return;
    final place = _place(paragraph);
    final size = paragraph.text.style?.fontSize;
    // The person's text size setting (1.0 unless a test enlarges it).
    final scale = (paragraph.textScaler.scale(100)).round() / 100;
    final key =
        '${place.join('>')}|$text|${constraints.maxWidth.round()}|'
        '${screen.width.round()}x${screen.height.round()}|$scale';
    final entry = _found.putIfAbsent(
      key,
      () => {
        'text': text.length > 160 ? '${text.substring(0, 160)}…' : text,
        'lines': lines,
        'cut': cut,
        'maxLines': paragraph.maxLines,
        'width': constraints.maxWidth.round(),
        'fontSize': size,
        'scale': scale,
        'screen': '${screen.width.round()}x${screen.height.round()}',
        'place': place,
        'tests': <String>[],
      },
    );
    final tests = entry['tests']! as List<String>;
    if (tests.length < 3 && !tests.contains(test)) tests.add(test);
  }

  /// Where the text sits: the kinds of widget around it that say what it is
  /// (a button, a row of a menu, a dialog...) and the nearest keys.
  List<String> _place(RenderParagraph paragraph) {
    final creator = paragraph.debugCreator;
    if (creator is! DebugCreator) return const [];
    final kinds = <String>[];
    final keys = <String>[];
    creator.element.visitAncestorElements((element) {
      final widget = element.widget;
      final key = widget.key;
      if (key is ValueKey && keys.length < 2) keys.add('#${key.value}');
      final kind = switch (widget) {
        ButtonStyleButton() => widget.runtimeType.toString(),
        IconButton() => 'IconButton',
        ListTile() => 'ListTile',
        AlertDialog() => 'AlertDialog',
        Dialog() => 'Dialog',
        SnackBar() => 'SnackBar',
        AppBar() => 'AppBar',
        Chip() || RawChip() => 'Chip',
        Tooltip() => 'Tooltip',
        PopupMenuItem() => 'PopupMenuItem',
        InputDecorator() => 'Input',
        SegmentedButton() => 'SegmentedButton',
        BottomSheet() => 'BottomSheet',
        _ =>
          widget.runtimeType.toString().startsWith('Android') ||
                  widget.runtimeType.toString().startsWith('_Android') ||
                  widget.runtimeType.toString().endsWith('Panel') ||
                  widget.runtimeType.toString().endsWith('Dialog') ||
                  widget.runtimeType.toString().endsWith('Card') ||
                  widget.runtimeType.toString().endsWith('Row') ||
                  widget.runtimeType.toString().endsWith('Tile') ||
                  widget.runtimeType.toString().endsWith('Button') ||
                  widget.runtimeType.toString().endsWith('Chip')
              ? widget.runtimeType.toString()
              : null,
      };
      if (kind != null && kinds.length < 5 && !kinds.contains(kind)) {
        kinds.add(kind);
      }
      return kinds.length < 5 || keys.length < 2;
    });
    return [...kinds.reversed, ...keys.reversed];
  }

  void _write() {
    if (_found.isEmpty) return;
    final directory = Directory(folder)..createSync(recursive: true);
    final name =
        '${DateTime.now().microsecondsSinceEpoch}-${_found.length}-'
        '${identityHashCode(this)}.json';
    File(
      '${directory.path}${Platform.pathSeparator}$name',
    ).writeAsStringSync(jsonEncode(_found.values.toList()));
  }
}
