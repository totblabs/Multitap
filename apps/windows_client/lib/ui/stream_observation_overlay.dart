import 'package:flutter/material.dart';

import '../data/remote_session.dart';
import '../l10n/l10n.dart';

/// Passive diagnostic surface: never owns focus or pointer input.
class StreamObservationOverlay extends StatelessWidget {
  const StreamObservationOverlay({super.key, required this.session});
  final RemoteSession? session;

  @override
  Widget build(BuildContext context) {
    final current = session;
    if (current == null) return const SizedBox.shrink();
    return ListenableBuilder(
      listenable: current,
      builder: (context, _) {
        if (!current.observabilityEnabled) return const SizedBox.shrink();
        String ms(double? value) =>
            value == null ? '—' : value.toStringAsFixed(1);
        return IgnorePointer(
          child: ExcludeFocus(
            child: Align(
              alignment: Alignment.bottomLeft,
              child: Container(
                constraints: const BoxConstraints(maxWidth: 310),
                margin: const EdgeInsets.all(12),
                padding: const EdgeInsets.symmetric(
                  horizontal: 10,
                  vertical: 7,
                ),
                decoration: BoxDecoration(
                  color: const Color(0xdd202020),
                  borderRadius: BorderRadius.circular(9),
                ),
                child: Text(
                  l10n.viaTcpServerAppRound(ms(current.rttMs), ms(current.clientDecodeMs), ms(current.clientQueueMs), current.connection.observability ? l10n.collectingDetailedDiagnostics : l10n.hostDetailsApplyFromNext),
                  style: const TextStyle(
                    color: Color(0xffffdc9b),
                    fontSize: 12,
                    height: 1.4,
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}
