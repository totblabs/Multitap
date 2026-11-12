// Texture-path measurement harness (not product code): a real Flutter
// Windows view showing the actual RemoteEngine texture fed with synthetic
// H.264; reports decode vs texture fps, frame timings and process CPU.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

const channel = MethodChannel('qa');
final options = <String, String>{};

void main(List<String> args) {
  for (final arg in args) {
    if (!arg.startsWith('--')) continue;
    final eq = arg.indexOf('=');
    if (eq > 2) options[arg.substring(2, eq)] = arg.substring(eq + 1);
  }
  runApp(const QaApp());
}

int opt(String key, int fallback) => int.tryParse(options[key] ?? '') ?? fallback;

class QaApp extends StatefulWidget {
  const QaApp({super.key});
  @override
  State<QaApp> createState() => _QaAppState();
}

class _Dist {
  final values = <double>[];
  void add(double v) => values.add(v);
  Map<String, Object> summary() {
    if (values.isEmpty) return {'n': 0};
    final sorted = [...values]..sort();
    double at(double q) => sorted[(q * (sorted.length - 1)).round()];
    final mean = sorted.reduce((a, b) => a + b) / sorted.length;
    return {
      'n': sorted.length,
      'mean': double.parse(mean.toStringAsFixed(2)),
      'p50': double.parse(at(.5).toStringAsFixed(2)),
      'p95': double.parse(at(.95).toStringAsFixed(2)),
      'max': double.parse(sorted.last.toStringAsFixed(2)),
    };
  }
}

class _QaAppState extends State<QaApp> {
  int? texture;
  int width = opt('width', 1264), height = opt('height', 760);
  String line = '-- ms';
  Timer? ticker;
  bool measuring = false;
  final raster = _Dist(), builds = _Dist(), vsync = _Dist(), total = _Dist();

  @override
  void initState() {
    super.initState();
    unawaited(run());
  }

  void timings(List<FrameTiming> list) {
    if (!measuring) return;
    for (final t in list) {
      raster.add(t.rasterDuration.inMicroseconds / 1000);
      builds.add(t.buildDuration.inMicroseconds / 1000);
      vsync.add(t.vsyncOverhead.inMicroseconds / 1000);
      total.add(t.totalSpan.inMicroseconds / 1000);
    }
  }

  Future<void> run() async {
    final out = options['out'];
    try {
      final started = await channel.invokeMapMethod<String, Object?>('start', {
        'width': width,
        'height': height,
        'fps': opt('fps', 60),
        'burst': opt('burst', 0),
        'timer': opt('timer', 0),
        'jitter': opt('jitter', 4),
        'drift': opt('drift', 0),
      });
      setState(() => texture = started!['texture'] as int);
      // The toolbar's measured-latency line changes about once a second.
      ticker = Timer.periodic(const Duration(seconds: 1), (t) {
        if (opt('overlay', 1) == 1) setState(() => line = '${4 + t.tick % 3} ms');
      });
      SchedulerBinding.instance.addTimingsCallback(timings);
      await Future<void>.delayed(Duration(seconds: opt('warmup', 3)));
      final s0 = await channel.invokeMapMethod<String, Object?>('stats');
      measuring = true;
      await Future<void>.delayed(Duration(seconds: opt('seconds', 15)));
      measuring = false;
      final s1 = await channel.invokeMapMethod<String, Object?>('stats');
      await channel.invokeMethod('stop');
      num d(String k) => (s1![k] as num) - (s0![k] as num);
      final wall = d('qaWallMs') / 1000;
      final notify = d('textureNotifyCalls');
      final result = <String, Object?>{
        'label': options['label'] ?? '',
        'width': width,
        'height': height,
        'fps': opt('fps', 60),
        'burst': opt('burst', 0),
        'timer': opt('timer', 0),
        'overlay': opt('overlay', 1),
        'hz': opt('hz', 0),
        'jitter': opt('jitter', 4),
        'drift': opt('drift', 0),
        'seconds': double.parse(wall.toStringAsFixed(2)),
        'pushedFps': double.parse((d('qaPushed') / wall).toStringAsFixed(1)),
        'rejected': d('qaRejected'),
        'decodeFps': double.parse((d('decodedFrames') / wall).toStringAsFixed(1)),
        'textureFps': double.parse((d('textureFrames') / wall).toStringAsFixed(1)),
        'skipped': d('skippedDecodedFrames'),
        'decodeMs': d('decodeCalls') > 0
            ? double.parse((d('decodeTotalMs') / d('decodeCalls')).toStringAsFixed(2))
            : null,
        'publishMs': notify > 0
            ? double.parse((d('textureNotifyTotalMs') / notify).toStringAsFixed(3))
            : null,
        'cpuPercentOfOneCore': double.parse((d('qaCpuMs') / 10 / wall).toStringAsFixed(1)),
        'mainThreadCpuPct': double.parse((d('qaMainCpuMs') / 10 / wall).toStringAsFixed(1)),
        // Flutter's pixel-buffer callback -> release (its glTexImage2D upload).
        'uploadMs': d('qaTakes') > 0
            ? double.parse((d('qaTakeTotalMs') / d('qaTakes')).toStringAsFixed(3))
            : null,
        'uploadMaxMsLifetime': s1!['qaTakeMaxMs'],
        'buildFrameRasterMs': raster.summary(),
        'buildMs': builds.summary(),
        'vsyncOverheadMs': vsync.summary(),
        'totalSpanMs': total.summary(),
        'rasterFramesPerSec': double.parse((raster.values.length / wall).toStringAsFixed(1)),
        'engine': {
          for (final k in s1!.keys)
            if (k.startsWith('texture') || k.startsWith('gpu') || k.startsWith('display'))
              k: s1[k],
        },
      };
      if (out != null) {
        File(out).writeAsStringSync('${jsonEncode(result)}\n', mode: FileMode.append);
      }
    } catch (error) {
      if (out != null) {
        File(out).writeAsStringSync(
          '${jsonEncode({'label': options['label'], 'error': '$error'})}\n',
          mode: FileMode.append,
        );
      }
    }
    exit(0);
  }

  @override
  Widget build(BuildContext context) {
    final id = texture;
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      home: Scaffold(
        backgroundColor: const Color(0xFF080D16),
        body: Stack(
          fit: StackFit.expand,
          children: [
            if (id != null)
              Center(
                child: AspectRatio(
                  aspectRatio: width / height,
                  child: Texture(
                    textureId: id,
                    filterQuality: opt('filter', 1) == 0 ? FilterQuality.none : FilterQuality.low,
                  ),
                ),
              ),
            if (opt('overlay', 1) == 1)
              Positioned(
                top: 6,
                left: 0,
                right: 0,
                child: Center(
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: const Color(0xF01B1E24),
                      borderRadius: BorderRadius.circular(16),
                      boxShadow: const [
                        BoxShadow(color: Color(0x80000000), blurRadius: 30, spreadRadius: -8, offset: Offset(0, 14)),
                      ],
                    ),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                      child: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          for (final icon in [Icons.drag_indicator, Icons.fit_screen, Icons.monitor, Icons.content_paste, Icons.bar_chart, Icons.close])
                            Padding(padding: const EdgeInsets.symmetric(horizontal: 6), child: Icon(icon, size: 18, color: const Color(0xFFE8B04A))),
                          const SizedBox(width: 6),
                          Text(line, style: const TextStyle(color: Color(0xCCFFFFFF), fontSize: 11)),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }
}
