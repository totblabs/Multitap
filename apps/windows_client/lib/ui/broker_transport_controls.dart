import 'package:flutter/material.dart';

import 'app_dialog.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// HTTPS or HTTP as two equal halves of one row at every width and text
/// size (2026-11-02: two chips of their own widths wrapped unevenly). The
/// labels are the scheme alone (2026-11-03: "HTTP · 암호화 없음" was cut in
/// narrow windows); the line above the choice says what it means. No icons
/// (2026-11-11: a lock beside both read as if HTTP were encrypted too).
class BrokerTransportChoice extends StatelessWidget {
  const BrokerTransportChoice({
    super.key,
    required this.scheme,
    required this.onChanged,
  });
  final String scheme;
  final ValueChanged<String>? onChanged;
  @override
  Widget build(BuildContext context) => SegmentedButton<String>(
    expandedInsets: EdgeInsets.zero,
    showSelectedIcon: false,
    segments: [
      for (final value in const ['https', 'http'])
        ButtonSegment(
          value: value,
          label: KeepAllText(
            value == 'https' ? 'HTTPS' : 'HTTP',
            key: Key('broker-scheme-$value'),
            textAlign: TextAlign.center,
            style: const TextStyle(fontSize: 12.5),
          ),
        ),
    ],
    selected: {scheme},
    onSelectionChanged: onChanged == null
        ? null
        : (values) => onChanged!(values.single),
  );
}

Future<bool> confirmInsecureBroker(BuildContext context, Uri origin) =>
    showAppConfirm(
      context,
      title: l10n.connectWithoutEncryption,
      subtitle: origin.origin,
      message:
          l10n.signDetailsScreenInputArent,
      icon: Icons.lock_open_rounded,
      tone: AppTone.warn,
      confirmLabel: l10n.useHttpAddress,
      dialogKey: const Key('broker-http-consent'),
      confirmKey: const Key('confirm-http-consent'),
      cancelKey: const Key('cancel-http-consent'),
    );
