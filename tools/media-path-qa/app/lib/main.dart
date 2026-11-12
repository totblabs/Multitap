// Media path measurement harness (not product code). Replays the product
// RemoteSession receive loop - a WebSocket `await for`, one awaited platform
// channel `push` per packet into the actual RemoteEngine - against a local
// WebSocket source running in another isolate, and times every step on the
// process-wide monotonic clock (dart:developer Timeline.now, microseconds):
//   send -> arrival     source write -> the receive loop reached the packet
//   arrival -> pushed   the loop's awaited push returned (product framePush)
//   queued -> published native decode queue -> frame handed to the texture
//   arrival -> published  framePush + frameDecode (the product's client share)
// Loopback only: no broker, relay, VM or network; plain ws (the product uses
// TLS). Results describe the machine they ran on.
import 'dart:async';
import 'dart:convert';
import 'dart:developer';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

const channel = MethodChannel('qa');
final options = <String, String>{};
int opt(String key, int fallback) => int.tryParse(options[key] ?? '') ?? fallback;

void main(List<String> args) {
  for (final arg in args) {
    if (!arg.startsWith('--')) continue;
    final eq = arg.indexOf('=');
    if (eq > 2) options[arg.substring(2, eq)] = arg.substring(eq + 1);
  }
  runApp(const QaApp());
}

class _Dist {
  final values = <double>[];
  void add(double v) => values.add(v);
  Map<String, Object> summary() {
    if (values.isEmpty) return {'n': 0};
    final s = [...values]..sort();
    double at(double q) => s[(q * (s.length - 1)).round()];
    double r(double v) => double.parse(v.toStringAsFixed(2));
    return {'n': s.length, 'mean': r(s.reduce((a, b) => a + b) / s.length), 'p50': r(at(.5)), 'p95': r(at(.95)), 'max': r(s.last)};
  }
}

/// Source isolate: a local WebSocket that sends the fixture's video packets at
/// `fps` (pts = Timeline.now at send) and 1932-byte audio packets at `audio`
/// per second, like a host's media channel.
class _SourceConfig {
  _SourceConfig(this.reply, this.packets, this.fps, this.audio);
  final SendPort reply;
  final List<TransferableTypedData> packets;
  final int fps, audio;
}

Future<void> _source(_SourceConfig config) async {
  final payloads = [for (final p in config.packets) p.materialize().asUint8List()];
  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
  config.reply.send(server.port);
  await for (final request in server) {
    final socket = await WebSocketTransformer.upgrade(request);
    final audio = Uint8List(12 + 1920)..[0] = 2;
    var n = 0, sentAudio = 0;
    final began = Timeline.now;
    var running = true;
    socket.done.whenComplete(() => running = false);
    while (running) {
      final now = Timeline.now;
      final dueVideo = began + n * 1000000 ~/ config.fps;
      if (now >= dueVideo) {
        final payload = payloads[n % payloads.length];
        final packet = Uint8List(12 + payload.length)..[0] = 1..[1] = n % payloads.length == 0 ? 1 : 0;
        packet.buffer.asByteData().setUint64(4, Timeline.now, Endian.little);
        packet.setRange(12, packet.length, payload);
        socket.add(packet);
        n++;
      }
      while (config.audio > 0 && began + sentAudio * 1000000 ~/ config.audio <= Timeline.now) {
        audio.buffer.asByteData().setUint64(4, Timeline.now, Endian.little);
        socket.add(Uint8List.fromList(audio));
        sentAudio++;
      }
      await Future<void>.delayed(const Duration(milliseconds: 1));
    }
  }
}

class QaApp extends StatefulWidget {
  const QaApp({super.key});
  @override
  State<QaApp> createState() => _QaAppState();
}

class _QaAppState extends State<QaApp> with SingleTickerProviderStateMixin {
  int? texture;
  final width = opt('width', 1580), height = opt('height', 952);
  bool measuring = false;
  late final AnimationController spin = AnimationController(vsync: this, duration: const Duration(seconds: 1));
  final sendToArrival = _Dist(), arrivalToPushed = _Dist(), queuedToPublished = _Dist();
  final arrivalToPublished = _Dist(), sendToPublished = _Dist(), audioPush = _Dist();
  final builds = _Dist(), rasters = _Dist();
  final pending = <int, (int, int, int)>{}; // pts -> (sent, arrival, pushed)

  @override
  void initState() {
    super.initState();
    if (opt('animate', 0) == 1) spin.repeat();
    unawaited(run());
  }

  void timings(List<FrameTiming> list) {
    if (!measuring) return;
    for (final t in list) {
      builds.add(t.buildDuration.inMicroseconds / 1000);
      rasters.add(t.rasterDuration.inMicroseconds / 1000);
    }
  }

  Future<void> push(Uint8List packet, {bool watch = false}) async {
    var delayMs = 1;
    while (true) {
      try {
        await channel.invokeMethod<void>('push', {'lease': 1, 'bytes': packet, if (watch) 'watch': true});
        return;
      } on PlatformException catch (error) {
        if (error.code != 'media_queue_full') rethrow;
        await Future<void>.delayed(Duration(milliseconds: delayMs));
        if (delayMs < 8) delayMs *= 2;
      }
    }
  }

  void delivered(Object? frames) {
    if (frames is! List) return;
    for (final item in frames) {
      if (item is! List || item.length != 2) continue;
      final pts = item[0] as int, us = item[1] as int;
      final entry = pending.remove(pts);
      if (entry == null || !measuring) continue;
      final (sent, arrival, pushed) = entry;
      queuedToPublished.add(us / 1000);
      arrivalToPublished.add((pushed - arrival + us) / 1000);
      sendToPublished.add((pushed - sent + us) / 1000);
    }
  }

  Future<void> run() async {
    final out = options['out'];
    try {
      final started = await channel.invokeMapMethod<String, Object?>('start', {
        'width': width,
        'height': height,
        'audio': opt('audioDevice', 0),
        'frames': opt('frames', 120),
        'bitrate': opt('bitrate', 8000000),
      });
      setState(() => texture = started!['texture'] as int);
      final packets = [for (final p in started!['packets'] as List) TransferableTypedData.fromList([p as Uint8List])];
      if (opt('burn', 0) > 0) await channel.invokeMethod('burn', {'threads': opt('burn', 0)});
      if (opt('boost', 0) == 1) await channel.invokeMethod('priority', {'raise': true});
      final port = ReceivePort();
      await Isolate.spawn(_source, _SourceConfig(port.sendPort, packets, opt('fps', 18), opt('audio', 0)));
      final sourcePort = await port.first as int;
      final socket = await WebSocket.connect('ws://127.0.0.1:$sourcePort', compression: CompressionOptions.compressionOff);
      SchedulerBinding.instance.addTimingsCallback(timings);
      final awaitAudio = opt('awaitAudio', 1) == 1;
      final stats = Timer.periodic(const Duration(milliseconds: 250), (_) async {
        try {
          final s = await channel.invokeMapMethod<String, Object?>('stats');
          delivered(s?['inputFrames']);
        } catch (_) {}
      });
      var stopAt = 0;
      unawaited(Future<void>.delayed(Duration(seconds: opt('warmup', 3)), () {
        measuring = true;
        stopAt = Timeline.now + opt('seconds', 20) * 1000000;
      }));
      Map<String, Object?>? s0;
      await for (final message in socket) {
        if (message is! List<int>) continue;
        final arrival = Timeline.now;
        if (measuring && s0 == null) s0 = await channel.invokeMapMethod<String, Object?>('stats');
        if (stopAt != 0 && arrival > stopAt) break;
        final packet = message is Uint8List ? message : Uint8List.fromList(message);
        final sent = packet.buffer.asByteData(packet.offsetInBytes).getUint64(4, Endian.little);
        if (packet[0] == 1) {
          if (measuring) sendToArrival.add((arrival - sent) / 1000);
          await push(packet, watch: true);
          final pushed = Timeline.now;
          if (measuring) arrivalToPushed.add((pushed - arrival) / 1000);
          pending[sent] = (sent, arrival, pushed);
          if (pending.length > 256) pending.remove(pending.keys.first);
        } else if (awaitAudio) {
          await push(packet);
          if (measuring) audioPush.add((Timeline.now - arrival) / 1000);
        } else {
          unawaited(push(packet).catchError((Object _) {}));
        }
      }
      await Future<void>.delayed(const Duration(milliseconds: 600));
      final s1 = await channel.invokeMapMethod<String, Object?>('stats');
      delivered(s1?['inputFrames']);
      stats.cancel();
      measuring = false;
      await socket.close();
      num d(String k) => (s1![k] as num) - (s0![k] as num);
      final result = <String, Object?>{
        'label': options['label'] ?? '',
        'width': width,
        'height': height,
        'fps': opt('fps', 18),
        'audio': opt('audio', 0),
        'awaitAudio': awaitAudio,
        'burn': opt('burn', 0),
        'boost': opt('boost', 0),
        'animate': opt('animate', 0),
        'priorityClass': s1!['qaPriorityClass'],
        'logicalCpus': Platform.numberOfProcessors,
        'sendToArrivalMs': sendToArrival.summary(),
        'arrivalToPushedMs': arrivalToPushed.summary(),
        'queuedToPublishedMs': queuedToPublished.summary(),
        'arrivalToPublishedMs': arrivalToPublished.summary(),
        'sendToPublishedMs': sendToPublished.summary(),
        'audioPushMs': audioPush.summary(),
        'decodeMs': d('decodeCalls') > 0 ? double.parse((d('decodeTotalMs') / d('decodeCalls')).toStringAsFixed(2)) : null,
        'textureWaitMs': d('textureFrames') > 0 ? double.parse((d('textureWaitTotalMs') / d('textureFrames')).toStringAsFixed(2)) : null,
        'decodedFrames': d('decodedFrames'),
        'textureFrames': d('textureFrames'),
        'buildMs': builds.summary(),
        'rasterMs': rasters.summary(),
      };
      if (out != null) File(out).writeAsStringSync('${jsonEncode(result)}\n', mode: FileMode.append);
    } catch (error) {
      if (out != null) {
        File(out).writeAsStringSync('${jsonEncode({'label': options['label'], 'error': '$error'})}\n', mode: FileMode.append);
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
            if (id != null) Center(child: AspectRatio(aspectRatio: width / height, child: Texture(textureId: id))),
            if (opt('animate', 0) == 1)
              Positioned(top: 8, left: 8, child: RotationTransition(turns: spin, child: const Icon(Icons.sync, color: Colors.amber))),
          ],
        ),
      ),
    );
  }
}
