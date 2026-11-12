/// display-scale-v1 (docs/remote-session-protocol.md "Display scale"): a
/// client that streams the remote output at its own physical pixels (a phone)
/// keeps text readable by asking the Host for a Windows display scale on the
/// owned output it streams, like Microsoft Remote Desktop's desktop scale
/// factor. The Host clamps it to what Windows allows for the output size and
/// restores the previous value when the session ends. Only a connection whose
/// broker granted the protocol (a Host that advertised it) may send
/// `{"type":"display.scale","scale":N}`; released peers never see it.
library;

const displayScaleProtocol = 'display-scale-v1';

/// Independent outputs (two monitors, tracks-v1): the same request for every
/// owned output, each clamped by Windows; results carry each output's values.
const displayScaleOutputsProtocol = 'display-scale-outputs-v1';

/// Windows' display scale steps (percent).
const displayScalePercents = [
  100,
  125,
  150,
  175,
  200,
  225,
  250,
  300,
  350,
  400,
  450,
  500,
];

bool validDisplayScale(Object? value) =>
    value is int && displayScalePercents.contains(value);

/// The step nearest to [ratio] (1.0 = 100 %); ties go to the larger step.
int nearestDisplayScale(double ratio) {
  if (!ratio.isFinite || ratio <= 1) return displayScalePercents.first;
  final wanted = ratio * 100;
  var best = displayScalePercents.first;
  for (final percent in displayScalePercents) {
    if ((percent - wanted).abs() <= (best - wanted).abs()) best = percent;
  }
  return best;
}

/// The Host's result for this session's output (with display-scale-outputs-v1
/// a summary plus each output's own result in [outputs]).
class DisplayScaleApplied {
  const DisplayScaleApplied({
    required this.ok,
    required this.scale,
    required this.maximum,
    required this.code,
    this.id,
    this.outputs = const [],
  });
  final bool ok;

  /// The scale Windows now uses (0 = unknown) and the largest it allows for
  /// the output's current size (0 = unknown). For independent outputs the
  /// summary holds output "0"'s scale and the largest maximum.
  final int scale, maximum;

  /// 0 applied, 1 output not found, 2 unreadable, 3 refused by Windows,
  /// 4 no recovery record, 5 unsupported output.
  final int code;

  /// The output ('0'/'1') of an entry in [outputs]; null for a summary.
  final String? id;

  /// Each owned output's result (independent outputs only, else empty).
  final List<DisplayScaleApplied> outputs;

  /// The scale every output now uses; null when unknown or when they differ.
  int? get commonScale {
    if (outputs.isEmpty) return ok ? scale : null;
    final first = outputs.first;
    return outputs.every((o) => o.ok && o.scale == first.scale)
        ? first.scale
        : null;
  }

  static DisplayScaleApplied? _values(Map event, {String? id}) {
    final ok = event['ok'], scale = event['scale'], maximum = event['maximum'];
    final code = event['code'];
    if (ok is! bool ||
        !(scale == 0 || validDisplayScale(scale)) ||
        !(maximum == 0 || validDisplayScale(maximum)) ||
        code is! int ||
        code < 0 ||
        code > 5 ||
        (ok && (code != 0 || scale == 0))) {
      return null;
    }
    return DisplayScaleApplied(
      ok: ok,
      scale: scale as int,
      maximum: maximum as int,
      code: code,
      id: id,
    );
  }

  /// The relay's exact shape (display-scale.mjs); null when malformed.
  /// [outputs]: the session was granted display-scale-outputs-v1, so the
  /// result must carry one entry per output ('0'/'1', once each).
  static DisplayScaleApplied? parse(
    Map<String, dynamic> event, {
    bool outputs = false,
  }) {
    if (event['protocol'] != displayScaleProtocol) return null;
    final summary = _values(event);
    if (summary == null || !outputs) return summary;
    final list = event['outputs'];
    if (list is! List || list.isEmpty || list.length > 2) return null;
    final entries = <DisplayScaleApplied>[];
    for (final item in list) {
      final id = item is Map ? item['id'] : null;
      final entry = item is Map && item.length == 5 && (id == '0' || id == '1')
          ? _values(item, id: id as String)
          : null;
      if (entry == null || entries.any((e) => e.id == id)) return null;
      entries.add(entry);
    }
    return DisplayScaleApplied(
      ok: summary.ok,
      scale: summary.scale,
      maximum: summary.maximum,
      code: summary.code,
      outputs: List.unmodifiable(entries),
    );
  }
}
