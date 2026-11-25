/// decode-path-v1: which way this PC decodes a stream soonest.
///
/// A stream can be decoded on the GPU and handed to Flutter as a GPU texture
/// (the hardware path), or decoded on the CPU and handed over as pixels (the
/// CPU path). Which one is faster depends on the PC and cannot be told from
/// its parts: field 2026-11-25, one game on both paths of each PC:
///
///  * a Ryzen 7800X3D on its integrated graphics: a picture was ready 8-26 ms
///    after its packet on the hardware path (the GPU also draws the window,
///    and pictures waited for it) and 4.3-5.9 ms on the CPU path;
///  * a PC with an NVIDIA card: 2.5 ms on the hardware path, 10 ms on the
///    CPU path with the decoder busy up to 80 % of the time.
///
/// So the app measures. The runner reports, for every picture, the time from
/// its packet entering the engine to the picture being ready for Flutter
/// (`publishDelayTotalMs` / `publishDelayFrames`). While a stream runs at
/// [DecodePathRule.fastFps] or more, each statistics reading adds to the
/// record of the path in use. The next connection reads the records:
///
///  1. the hardware path, until it has [DecodePathRule.enoughSeconds] of
///     fast stream on record;
///  2. the hardware path while it keeps up: a picture is ready within one
///     picture's time in all but a [DecodePathRule.behindShare] of that time;
///  3. otherwise the CPU path, to measure it;
///  4. then the CPU path if it is clearly sooner and has room, else the
///     hardware path.
///
/// The share of pictures shown is deliberately not the measure: it follows
/// how pictures arrive (bursts) as much as the path, and it ranked the NVIDIA
/// PC's hardware path (89 % shown, 2.5 ms) below its CPU path (95 %, 10 ms).
///
/// A path is fixed for a connection: the picture sizes a connection may use
/// are admitted for the decoder it starts with.
library;

/// What one path cost on this PC, summed over the seconds it was observed.
class DecodePathRecord {
  DecodePathRecord({
    this.seconds = 0,
    this.delay = 0,
    this.behind = 0,
    this.busy = 0,
  });

  /// Observed time, and the sums over it: mean ready delay (ms) x seconds,
  /// seconds in which a picture was ready later than one picture's time,
  /// seconds in which the decoder was busy [DecodePathRule.busyShare] or more.
  double seconds, delay, behind, busy;

  double get delayMs => seconds > 0 ? delay / seconds : 0;
  double get behindShare => seconds > 0 ? behind / seconds : 0;
  double get busyShare => seconds > 0 ? busy / seconds : 0;

  void add(double forSeconds, double delayMs, double frameMs, bool decoderBusy) {
    if (!(forSeconds > 0) ||
        !forSeconds.isFinite ||
        !delayMs.isFinite ||
        delayMs < 0 ||
        !(frameMs > 0)) {
      return;
    }
    seconds += forSeconds;
    delay += delayMs * forSeconds;
    if (delayMs > frameMs) behind += forSeconds;
    if (decoderBusy) busy += forSeconds;
    // Older streams count less: another monitor, driver or habit.
    if (seconds > DecodePathRule.keepSeconds) {
      seconds /= 2;
      delay /= 2;
      behind /= 2;
      busy /= 2;
    }
  }

  Map<String, Object> toJson() => {
    'seconds': _round(seconds),
    'delay': _round(delay),
    'behind': _round(behind),
    'busy': _round(busy),
  };

  static double _round(double value) => (value * 1000).roundToDouble() / 1000;

  /// A stored record; anything malformed is an empty one.
  static DecodePathRecord parse(Object? value) {
    if (value is! Map) return DecodePathRecord();
    double? number(String key) {
      final v = value[key];
      return v is num && v.isFinite && v >= 0 && v <= 1e9 ? v.toDouble() : null;
    }

    final seconds = number('seconds'),
        delay = number('delay'),
        behind = number('behind'),
        busy = number('busy');
    if (seconds == null || delay == null || behind == null || busy == null) {
      return DecodePathRecord();
    }
    if (behind > seconds || busy > seconds) return DecodePathRecord();
    return DecodePathRecord(
      seconds: seconds,
      delay: delay,
      behind: behind,
      busy: busy,
    );
  }
}

/// Why a connection decodes the way it does. [code] is the diagnostics value.
enum DecodePathReason {
  /// The user's switch is off.
  cpuOnly(0),

  /// Not enough fast stream on the hardware path yet.
  hardwareUnmeasured(1),

  /// The hardware path keeps up.
  hardwareKeepsUp(2),

  /// The hardware path falls behind; the CPU path is being measured.
  cpuMeasuring(3),

  /// The CPU path is sooner here.
  cpuSooner(4),

  /// The hardware path falls behind, and the CPU path is no better.
  hardwareStill(5),

  /// The graphics adapter is not known, so nothing is measured or remembered.
  hardwareNoAdapter(6);

  const DecodePathReason(this.code);
  final int code;
  bool get hardware =>
      this == hardwareUnmeasured ||
      this == hardwareKeepsUp ||
      this == hardwareStill ||
      this == hardwareNoAdapter;
}

class DecodePathRule {
  /// A stream this fast is where the path matters (a work screen sends a few
  /// pictures a second and either path has time to spare).
  static const fastFps = 50.0;

  /// A decision needs this much fast stream on a path.
  static const enoughSeconds = 60.0;

  /// A record is halved when it holds this much.
  static const keepSeconds = 1800.0;

  /// The decoder counts as busy from this share of the time (0..1).
  static const busyShare = 0.8;

  /// The hardware path keeps up while it is behind in less than this share
  /// of its fast time.
  static const behindShare = 0.2;

  /// The CPU path is taken when its delay is at most this share of the
  /// hardware path's and it was busy in less than [overloadedShare].
  static const soonerShare = 0.75, overloadedShare = 0.1;

  static DecodePathReason decide(
    DecodePathRecord hardware,
    DecodePathRecord cpu,
  ) {
    if (hardware.seconds < enoughSeconds) {
      return DecodePathReason.hardwareUnmeasured;
    }
    if (hardware.behindShare < behindShare) {
      return DecodePathReason.hardwareKeepsUp;
    }
    if (cpu.seconds < enoughSeconds) return DecodePathReason.cpuMeasuring;
    final sooner =
        cpu.busyShare < overloadedShare &&
        cpu.behindShare < hardware.behindShare &&
        cpu.delayMs <= hardware.delayMs * soonerShare;
    return sooner ? DecodePathReason.cpuSooner : DecodePathReason.hardwareStill;
  }
}

/// The two records of one graphics adapter.
class DecodePathMemory {
  DecodePathMemory({DecodePathRecord? hardware, DecodePathRecord? cpu})
    : hardware = hardware ?? DecodePathRecord(),
      cpu = cpu ?? DecodePathRecord();
  final DecodePathRecord hardware, cpu;

  DecodePathReason get reason => DecodePathRule.decide(hardware, cpu);

  Map<String, Object> toJson() => {
    'hardware': hardware.toJson(),
    'cpu': cpu.toJson(),
  };

  static DecodePathMemory parse(Object? value) => value is Map
      ? DecodePathMemory(
          hardware: DecodePathRecord.parse(value['hardware']),
          cpu: DecodePathRecord.parse(value['cpu']),
        )
      : DecodePathMemory();
}
