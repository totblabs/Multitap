// Clipboard end-to-end harness (tools/clipboard-e2e-qa): the real broker relay (any server tree), the real Host ClipboardWorker
// and the real native client adapter, joined by the real ClipboardSession and NativeClipboardBridge, each pair of peers on an
// in-memory clipboard. Skipped unless MULTITAP_CLIPBOARD_E2E=1; tools/clipboard-e2e-qa/Run-ClipboardE2E.ps1 builds the helpers,
// sets the environment and runs this file once per relay.
//
//   MULTITAP_CLIPBOARD_E2E=1
//   E2E_NODE            node.exe
//   E2E_RELAY_FIXTURE   tools/clipboard-e2e-qa/relay_fixture.mjs
//   E2E_SERVERS         label=<server dir>;label=<server dir>     (each with src/ and node_modules/)
//   E2E_HOST_EXE, E2E_CLIENT_EXE, E2E_WORK
//   E2E_ONLY            optional substring of "relay/mode/scenario" to run a subset
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'e2e_harness.dart';

final _env = Platform.environment;
final _enabled = _env['MULTITAP_CLIPBOARD_E2E'] == '1';

class Rig {
  Rig(
    this.label,
    this.serverDir,
    this.sync, {
    this.policy = const ClipboardPolicy.all(),
    this.claimAfter = const Duration(milliseconds: 500),
    this.onPaste = false,
    this.dropWaiting = false,
    this.delayMs = 0,
    this.deferred = true,
  });
  final Duration claimAfter;

  /// The Host announces the deferred manifest (manifest-on-paste-v1); false starts it with --no-deferred.
  final bool deferred;
  final String label, serverDir;
  final ClipboardPolicy policy;
  final bool sync;
  final bool onPaste, dropWaiting;
  final int delayMs;
  late final Relay relay;
  late final LineProcess host, client;
  late final ClipboardSession session;
  late final Directory work;
  var _counter = 0;

  Future<void> up() async {
    work = Directory(
      '${_env['E2E_WORK']}${Platform.pathSeparator}$label-${sync ? 'sync' : 'manual'}-${DateTime.now().microsecondsSinceEpoch}',
    );
    await work.create(recursive: true);
    relay = await Relay.start(
      _env['E2E_NODE'] ?? 'node',
      _env['E2E_RELAY_FIXTURE']!,
      serverDir,
      policy: jsonEncode({
        'textToRemote': policy.textToRemote,
        'textToLocal': policy.textToLocal,
        'filesToRemote': policy.filesToRemote,
        'filesToLocal': policy.filesToLocal,
      }),
    );
    host = await LineProcess.start('host', _env['E2E_HOST_EXE']!, [
      '--url',
      relay.hostUrl,
      '--token',
      relay.info['hostToken'] as String,
      '--session',
      relay.info['sessionId'] as String,
      '--root',
      '${work.path}${Platform.pathSeparator}host',
      if (!onPaste) '--no-onpaste',
      if (dropWaiting) '--drop-waiting',
      if (!deferred) '--no-deferred',
      if (delayMs > 0) ...['--emit-delay-ms', '$delayMs'],
    ]);
    await host.waitEvent((e) => e['event'] == 'started');
    client = await LineProcess.start('client', _env['E2E_CLIENT_EXE']!, [
      '--root',
      '${work.path}${Platform.pathSeparator}client',
      if (delayMs > 0) ...['--serve-delay-ms', '$delayMs'],
    ]);
    await client.waitEvent((e) => e['event'] == 'started');
    attachNativeChannel(client);
    final sessionId = relay.info['sessionId'] as String;
    session = ClipboardSession(
      url: Uri.parse(relay.clientUrl.replaceFirst('ws:', 'wss:')),
      token: relay.info['clientToken'] as String,
      sessionId: sessionId,
      policy: policy,
      syncSupported: true,
      bridge: NativeClipboardBridge(
        allowedText: policy.textToRemote,
        allowedFiles: policy.filesToRemote,
        announceOnPaste: onPaste,
      ),
      dialer: (url, token) => WebSocket.connect(
        relay.clientUrl,
        headers: {'Authorization': 'Bearer $token', ...session.dialHeaders},
      ),
      publishCheckDelay: const Duration(milliseconds: 200),
      claimAfter: claimAfter,
    );
    await session.start();
    if (sync) await session.setSharingActive(true);
  }

  Future<void> down() async {
    detachNativeChannel();
    try {
      await session.close(reason: 'clipboard_session_closed');
    } catch (_) {}
    await client.stop();
    await host.stop();
    await relay.stop();
  }

  Directory scratch(String name) {
    final directory = Directory(
      '${work.path}${Platform.pathSeparator}${name}_${_counter++}',
    )..createSync(recursive: true);
    return directory;
  }

  LineProcess peer(bool pc) => pc ? client : host;
  Future<Map<dynamic, dynamic>> board(bool pc) async =>
      (await peer(pc).request({'cmd': 'get'}))! as Map<dynamic, dynamic>;

  /// Copies on one side and gets the content to the other: manual mode asks the session, sync mode waits for Ctrl+C sharing.
  Future<void> transfer({
    required bool toVm,
    required Future<void> Function(LineProcess source) copy,
  }) async {
    final source = peer(toVm);
    await copy(source);
    if (!sync) {
      if (toVm) {
        await session.sendLocal();
      } else {
        await session.receiveRemote();
      }
    }
  }

  /// Waits until the receiving board satisfies [check] (sync mode transfers on its own, manual mode has finished already).
  Future<Map<dynamic, dynamic>> expectArrived({
    required bool toVm,
    required bool Function(Map<dynamic, dynamic>) check,
    required String what,
    Duration timeout = const Duration(seconds: 40),
  }) async {
    Map<dynamic, dynamic>? last;
    try {
      await eventually(
        () async {
          last = await board(!toVm);
          return check(last!);
        },
        what,
        timeout: timeout,
      );
    } on TimeoutException {
      throw TestFailure(
        '$what did not arrive (receiver board: $last, session error: ${session.error}, status: ${session.status}, '
        'relay closures: ${relay.closures}, host events: ${_tail(host)}, client events: ${_tail(client)})',
      );
    }
    if (session.error != null && session.error != 'clipboard_empty') {
      throw TestFailure(
        '$what arrived but the session reports ${session.error}',
      );
    }
    return last!;
  }

  String _tail(LineProcess p) {
    final interesting = p.events
        .where(
          (e) =>
              (((e['event'] == 'emit' || e['event'] == 'recv') &&
                  !{
                    'clipboard.chunk',
                    'clipboard.chunk.ack',
                    'clipboard.item.ack',
                  }.contains(e['type']))) ||
              e['event'] == 'diag' ||
              e['event'] == 'changed' ||
              e['event'] == 'channelError',
        )
        .toList();
    final tail = interesting.length > 30
        ? interesting.sublist(interesting.length - 30)
        : interesting;
    return tail
        .map(
          (e) =>
              '${e['event']}:${e['type'] ?? e['stage'] ?? e['sequence']}${e['cause'] == null ? '' : '/cause=${e['cause']}'}${(e['reason'] ?? '') == '' ? '' : '(${e['reason']})'}',
        )
        .join(' ');
  }
}

typedef Scenario = Future<void> Function(Rig rig);

const mib = 1024 * 1024, gib = 1024 * mib;

/// A directory under [base] whose absolute path is far beyond MAX_PATH (six levels of 55 characters).
Directory _deep(Directory base) {
  var path = base.path;
  for (var i = 0; i < 6; i++) {
    path = '$path${Platform.pathSeparator}${String.fromCharCode(97 + i) * 55}';
  }
  return Directory(path)..createSync(recursive: true);
}

/// ASCII text of exactly [length] characters (so also bytes), the same on both sides of a comparison.
String _longText(int length) {
  const unit = 'The quick brown fox jumps over the lazy dog 0123456789. ';
  final out = StringBuffer();
  while (out.length + unit.length <= length) {
    out.write(unit);
  }
  return out.toString().padRight(length, '.');
}

/// A transfer that must be refused: manual mode asks the session, sync mode waits for the automatic transfer to fail.
Future<void> _refused(Rig rig, bool toVm) async {
  if (rig.sync) {
    await eventually(
      () =>
          rig.session.error != null &&
          rig.session.error != 'clipboard_empty' &&
          rig.session.error != 'clipboard_cancelled' &&
          !rig.session.busy,
      'the refusal',
      timeout: const Duration(seconds: 60),
    );
  } else if (toVm) {
    await expectLater(rig.session.sendLocal(), throwsA(anything));
  } else {
    await expectLater(rig.session.receiveRemote(), throwsA(anything));
  }
}

/// The reason of the last clipboard.error the Host sent.
String? _hostReason(Rig rig) {
  final errors = rig.host.events
      .where((e) => e['event'] == 'emit' && e['type'] == 'clipboard.error')
      .toList();
  return errors.isEmpty ? null : errors.last['reason'] as String?;
}

Future<void> _copyText(LineProcess p, String text) =>
    p.request({'cmd': 'setText', 'text': text});

/// Files named [names] with the given sizes under a fresh folder; returns their paths.
Future<List<String>> _makeFiles(
  Rig rig,
  Map<String, int> files, {
  int seed = 5,
}) async {
  final directory = rig.scratch('files');
  final paths = <String>[];
  var s = seed;
  for (final entry in files.entries) {
    paths.add(await writeFile(directory, entry.key, noise(entry.value, s++)));
  }
  return paths;
}

Future<void> _filesBothWays(Rig rig, Map<String, int> files) async {
  for (final toVm in [true, false]) {
    final paths = await _makeFiles(rig, files, seed: toVm ? 11 : 12);
    late Map<dynamic, dynamic> sent;
    await rig.transfer(
      toVm: toVm,
      copy: (source) async {
        await source.request({'cmd': 'setFiles', 'paths': paths});
        sent = await rig.board(toVm);
      },
    );
    final got = await rig.expectArrived(
      toVm: toVm,
      what: 'files ${toVm ? 'PC->VM' : 'VM->PC'}',
      check: (b) =>
          b['kind'] == 'files' &&
          entryKeys(b).join('|') == entryKeys(sent).join('|'),
    );
    expect(entryKeys(got), entryKeys(sent));
  }
}

/// One entry of a virtual-file copy (a .zip folder in Explorer, a mail attachment): [name] as the descriptor carries it, content
/// reached through an IStream or an HGLOBAL, with or without a size in the descriptor.
class _VItem {
  _VItem(
    this.name, {
    this.size = 0,
    this.dir = false,
    this.mode = 'stream',
    this.declare = true,
    this.seed = 1,
  });
  final String name, mode;
  final int size, seed;
  final bool dir, declare;
  String get relative =>
      name.replaceAll('\\', '/').replaceAll(RegExp(r'/+$'), '');
  List<int> get bytes => noise(size, seed);
}

/// Copies [items] as virtual files on one side; returns the mirror tree (the same files on disk) to compare what arrives with.
Future<List<String>> _mirrorOf(Rig rig, List<_VItem> items) async {
  final root = rig.scratch('mirror');
  final tops = <String>{};
  for (final item in items) {
    if (item.dir) {
      await Directory(
        '${root.path}${Platform.pathSeparator}${item.relative.replaceAll('/', Platform.pathSeparator)}',
      ).create(recursive: true);
    } else {
      await writeFile(root, item.relative, item.bytes);
    }
    tops.add(
      '${root.path}${Platform.pathSeparator}${item.relative.split('/').first}',
    );
  }
  return tops.toList();
}

Future<void> _copyVirtual(LineProcess p, List<_VItem> items) => p.request({
  'cmd': 'setVirtual',
  'entries': [
    for (final item in items)
      {
        'name': item.name,
        'dir': item.dir,
        'mode': item.mode,
        'declare': item.declare,
        'data': base64Encode(item.dir ? const <int>[] : item.bytes),
      },
  ],
});

Future<void> _virtualBothWays(
  Rig rig,
  List<_VItem> Function(int seedBase) build,
) async {
  for (final toVm in [true, false]) {
    final items = build(toVm ? 10 : 50);
    final mirror = await _mirrorOf(rig, items);
    final expected = entryKeys(
      await rig.peer(toVm).request({'cmd': 'describePaths', 'paths': mirror})
          as Map,
    );
    await rig.transfer(
      toVm: toVm,
      copy: (source) => _copyVirtual(source, items),
    );
    final got = await rig.expectArrived(
      toVm: toVm,
      what: 'virtual files ${toVm ? 'PC->VM' : 'VM->PC'}',
      check: (b) =>
          b['kind'] == 'files' && entryKeys(b).join('|') == expected.join('|'),
    );
    expect(entryKeys(got), expected);
  }
}

final scenarios = <String, Scenario>{
  'tree both ways followed by images and files keeps clipboard writable':
      (rig) async {
        for (final toVm in [false, true]) {
          final root = rig.scratch('write-sequence');
          final path = await writeFile(
            root,
            'Tree/data.bin',
            noise(800000, toVm ? 32 : 31),
          );
          final top = File(path).parent.path;
          final expected = entryKeys(
            await rig.peer(toVm).request({
              'cmd': 'describePaths',
              'paths': [top],
            }) as Map,
          );
          await rig.transfer(
            toVm: toVm,
            copy: (source) => source.request({
              'cmd': 'setFiles',
              'paths': [top],
            }),
          );
          final received = await rig.expectArrived(
            toVm: toVm,
            what: 'tree before classic writes',
            check: (b) =>
                b['kind'] == 'files' &&
                entryKeys(b).join('|') == expected.join('|'),
          );
          final entries = (received['entries'] as List).cast<Map>();
          expect(entries, isNotEmpty);
        }
        for (final toVm in [false, true]) {
          final png = makePng(130, 100, seed: toVm ? 91 : 92);
          await rig.transfer(
            toVm: toVm,
            copy: (source) => source.request({
              'cmd': 'setImage',
              'png': base64Encode(png),
              'width': 130,
              'height': 100,
            }),
          );
          await rig.expectArrived(
            toVm: toVm,
            what: 'image after tree ${toVm ? 'PC to VM' : 'VM to PC'}',
            check: (b) =>
                b['kind'] == 'image' &&
                b['pngSha'] == sha256.convert(png).toString(),
          );
        }
        await _filesBothWays(rig, {
          'after-tree.bin': 70000,
          'after-tree.txt': 17,
        });
      },
  // ---- Long paths, the scale revision (docs/remote-session-protocol.md, "Copy-source matrix") ------------------------------------------
  // Source folders whose absolute paths are far beyond MAX_PATH are read, sent and received. When the received copy's absolute paths reach
  // MAX_PATH the receiver publishes virtual files served from the local copy instead of CF_HDROP; either way the content is the same.
  'deep source folder (absolute paths beyond MAX_PATH) both ways': (rig) async {
    for (final toVm in [true, false]) {
      final prefix = _deep(rig.scratch('deep'));
      await writeFile(prefix, 'Tree/docs/sub/readme.txt', noise(300, 21));
      await writeFile(prefix, 'Tree/data.bin', noise(70000, 22));
      await Directory(
        '${prefix.path}${Platform.pathSeparator}Tree${Platform.pathSeparator}Empty',
      ).create(recursive: true);
      final top = '${prefix.path}${Platform.pathSeparator}Tree';
      expect(top.length, greaterThan(300));
      late Map<dynamic, dynamic> sent;
      await rig.transfer(
        toVm: toVm,
        copy: (s) async {
          await s.request({
            'cmd': 'setFiles',
            'paths': [top],
          });
          sent = await rig.board(toVm);
        },
      );
      final got = await rig.expectArrived(
        toVm: toVm,
        what: 'deep folder ${toVm ? 'PC->VM' : 'VM->PC'}',
        check: (b) =>
            (b['kind'] == 'files' &&
                entryKeys(b).join('|') == entryKeys(sent).join('|')) ||
            b['kind'] == 'localTree',
      );
      if (got['kind'] == 'localTree') {
        final destination = rig.scratch('pasted');
        await rig.peer(!toVm).request({
          'cmd': 'paste',
          'dest': destination.path,
        }, timeout: const Duration(seconds: 120));
        expect(
          await _digest(rig, !toVm, destination),
          entryKeys(sent).join('|'),
        );
      }
    }
  },
  // A relative path of 190 characters: fine with the scale revision (up to 259), a limit of 160 without it, named by its precise message.
  'folder with a 190-character relative path: pasted, or refused with the path message':
      (rig) async {
        final scale = rig.session.limits.scale;
        for (final toVm in [true, false]) {
          final root = rig.scratch('p190');
          await writeFile(
            root,
            'Tree/${'d' * 90}/${'e' * 90}/f.txt',
            noise(120, 31),
          );
          final top = '${root.path}${Platform.pathSeparator}Tree';
          if (scale) {
            late Map<dynamic, dynamic> sent;
            await rig.transfer(
              toVm: toVm,
              copy: (s) async {
                await s.request({
                  'cmd': 'setFiles',
                  'paths': [top],
                });
                sent = await rig.board(toVm);
              },
            );
            final got = await rig.expectArrived(
              toVm: toVm,
              what: 'a 190-character relative path',
              check: (b) => b['kind'] == 'files' || b['kind'] == 'localTree',
            );
            if (got['kind'] == 'files') {
              expect(entryKeys(got), entryKeys(sent));
            } else {
              final destination = rig.scratch('pasted');
              await rig.peer(!toVm).request({
                'cmd': 'paste',
                'dest': destination.path,
              }, timeout: const Duration(seconds: 120));
              expect(
                await _digest(rig, !toVm, destination),
                entryKeys(sent).join('|'),
              );
            }
            continue;
          }
          // A pair without the revision: 160 characters, the peer that sees the path first says so; the pair stays up.
          rig.session.error =
              null; // a refusal of an earlier round is not this one
          await rig.peer(toVm).request({
            'cmd': 'setFiles',
            'paths': [top],
          });
          await _refused(rig, toVm);
          expect(rig.session.error, 'clipboard_limits_exceeded');
          if (toVm) {
            expect(rig.session.errorDetail, startsWith('path'));
            expect(rig.session.status, contains('경로가 너무 긴 파일이 있어요'));
            expect(rig.session.status, contains('160자까지'));
          } else {
            // The Host reports the general reason to a pair that does not know the precise one.
            expect(_hostReason(rig), 'clipboard_limits_exceeded');
          }
          await rig.peer(toVm).request({'cmd': 'clear'});
        }
      },
  // 260 characters or more cannot be pasted into any folder: refused up front. The PC names the length; the VM's Host sends the precise
  // reason when the pair has the revision, the general one otherwise.
  'a relative path above 259 characters is refused up front with the path message':
      (rig) async {
        final scale = rig.session.limits.scale;
        for (final toVm in [true, false]) {
          final root = rig.scratch('p300');
          await writeFile(
            root,
            'Tree/${'d' * 95}/${'e' * 95}/${'g' * 95}/f.txt',
            noise(50, 41),
          );
          rig.session.error =
              null; // a refusal of an earlier round is not this one
          await rig.peer(toVm).request({
            'cmd': 'setFiles',
            'paths': ['${root.path}${Platform.pathSeparator}Tree'],
          });
          await _refused(rig, toVm);
          expect(rig.session.error, 'clipboard_limits_exceeded');
          if (toVm || scale) {
            expect(
              rig.session.status,
              isNot(contains('파일 하나에')),
              reason: 'the all-limits sentence',
            );
          }
          if (toVm) {
            expect(rig.session.errorDetail, 'path:298');
            expect(rig.session.status, contains('가장 긴 경로 298자'));
            expect(rig.session.status, contains(scale ? '259자까지' : '160자까지'));
          } else {
            expect(
              _hostReason(rig),
              scale ? 'clipboard_path_too_long' : 'clipboard_limits_exceeded',
            );
            // The Host's journal record carries the precise reason code (reasons 1-based: clipboard_path_too_long is 17, limits_exceeded 12).
            await eventually(
              () => rig.host.events.any(
                (e) =>
                    e['event'] == 'diag' &&
                    e['stage'] == 6 &&
                    e['error'] == (scale ? 17 : 12),
              ),
              'the Host record of the refusal',
            );
            if (scale) expect(rig.session.status, contains('경로가 너무 긴 파일이 있어요'));
          }
          expect(
            (await rig.board(!toVm))['kind'],
            'empty',
            reason: 'nothing may arrive',
          );
          await rig.peer(toVm).request({'cmd': 'clear'});
          rig.host.events.removeWhere(
            (e) => e['event'] == 'emit' || e['event'] == 'diag',
          );
        }
      },
  // Sizes: no per-file, total or top-level limit with the revision. The tail of a file above 4 GiB is read at its offset (a sparse file).
  'a file above 4 GiB is offered and its tail is pasted at the offset, both ways':
      (rig) async {
        final scale = rig.session.limits.scale;
        for (final toVm in [true, false]) {
          final path =
              '${rig.scratch('sparse').path}${Platform.pathSeparator}huge.bin';
          const size = 5 * gib + 7;
          await rig.peer(toVm).request({
            'cmd': 'makeSparse',
            'path': path,
            'size': size,
            'tail': 'TAIL123',
          });
          try {
            rig.session.error = null;
            await rig.peer(toVm).request({
              'cmd': 'setFiles',
              'paths': [path],
            });
            if (!scale) {
              await _refused(rig, toVm);
              expect(rig.session.error, 'clipboard_limits_exceeded');
              if (toVm) {
                expect(rig.session.errorDetail, 'file');
                expect(rig.session.status, contains('4 GB'));
              }
              continue;
            }
            if (!rig.sync) {
              if (toVm) {
                await rig.session.sendLocal();
              } else {
                await rig.session.receiveRemote();
              }
            }
            await rig.expectArrived(
              toVm: toVm,
              what: 'offer of a 5 GiB file',
              check: (b) =>
                  b['kind'] == 'offer' && b['files'] == 1 && b['bytes'] == size,
            );
            final tail = await rig.peer(!toVm).request({
              'cmd': 'pasteTail',
              'entry': 0,
              'length': 7,
            }, timeout: const Duration(seconds: 120)) as Map;
            expect(tail['size'], size);
            expect(tail['tail'], 'TAIL123');
          } finally {
            await rig.peer(toVm).request({'cmd': 'clear'});
            try {
              File(path).deleteSync();
            } catch (_) {}
          }
        }
      },
  'more than 128 top-level items are offered and pasted both ways':
      (rig) async {
        final scale = rig.session.limits.scale;
        for (final toVm in [true, false]) {
          final directory = rig.scratch('many');
          final paths = [
            for (var i = 0; i < 300; i++)
              await writeFile(directory, 'item$i.txt', noise(30 + i, 300 + i)),
          ];
          late Map<dynamic, dynamic> sent;
          await rig.peer(toVm).request({'cmd': 'setFiles', 'paths': paths});
          sent = await rig.board(toVm);
          if (!scale) {
            await _refused(rig, toVm);
            expect(rig.session.error, 'clipboard_limits_exceeded');
            if (toVm) {
              expect(rig.session.errorDetail, 'count');
              expect(rig.session.status, contains('128개'));
            }
            await rig.peer(toVm).request({'cmd': 'clear'});
            continue;
          }
          if (!rig.sync) {
            if (toVm) {
              await rig.session.sendLocal();
            } else {
              await rig.session.receiveRemote();
            }
          }
          await rig.expectArrived(
            toVm: toVm,
            what: 'offer of 300 top-level files',
            check: (b) => b['kind'] == 'offer' && b['files'] == 300,
          );
          final destination = rig.scratch('pasted');
          await rig.peer(!toVm).request({
            'cmd': 'paste',
            'dest': destination.path,
          }, timeout: const Duration(seconds: 120));
          expect(
            await _digest(rig, !toVm, destination),
            entryKeys(sent).join('|'),
          );
        }
      },
  // Text: 64 MiB with the revision (1 MiB without); the receiver gets exactly the bytes.
  '64 MiB of text both ways, or the text message without the revision':
      (rig) async {
        final scale = rig.session.limits.scale;
        final length = scale ? 64 * mib : 2 * mib;
        final text = _longText(length);
        final expected = sha256.convert(utf8.encode(text)).toString();
        for (final toVm in [true, false]) {
          await rig.peer(toVm).request({'cmd': 'setText', 'text': text});
          if (!scale) {
            await _refused(rig, toVm);
            expect(rig.session.error, 'clipboard_limits_exceeded');
            if (toVm) {
              expect(rig.session.errorDetail, 'text');
              expect(rig.session.status, contains('1 MB'));
            }
            await rig.peer(toVm).request({'cmd': 'clear'});
            continue;
          }
          if (!rig.sync) {
            if (toVm) {
              await rig.session.sendLocal();
            } else {
              await rig.session.receiveRemote();
            }
          }
          final got = await rig.expectArrived(
            toVm: toVm,
            what: '64 MiB of text',
            check: (b) => b['kind'] == 'text' && b['textSha'] == expected,
            timeout: const Duration(minutes: 3),
          );
          expect(got['textLength'], length);
          await rig.peer(toVm).request({'cmd': 'clear'});
        }
      },
  'text one byte above the limit is refused with the text message':
      (rig) async {
        final scale = rig.session.limits.scale;
        final text = _longText(scale ? 64 * mib + 1 : mib + 1);
        for (final toVm in [true, false]) {
          rig.session.error =
              null; // a refusal of an earlier round is not this one
          await rig.peer(toVm).request({'cmd': 'setText', 'text': text});
          await _refused(rig, toVm);
          expect(rig.session.error, 'clipboard_limits_exceeded');
          if (toVm) {
            expect(rig.session.errorDetail, 'text');
            expect(rig.session.status, contains(scale ? '64 MB' : '1 MB'));
          } else {
            expect(
              _hostReason(rig),
              scale ? 'clipboard_text_too_large' : 'clipboard_limits_exceeded',
            );
            if (scale) expect(rig.session.status, contains('64 MB'));
          }
          await rig.peer(toVm).request({'cmd': 'clear'});
          rig.host.events.removeWhere((e) => e['event'] == 'emit');
        }
      },
  // A newer copy cancels the older transfer that is still crossing (sharing is automatic only in sync mode).
  'a newer copy replaces a big text still crossing, both ways': (rig) async {
    if (!rig.sync) return;
    final length = rig.session.limits.scale ? 64 * mib : 900 * 1024;
    final big = _longText(length);
    for (final toVm in [true, false]) {
      await rig.peer(toVm).request({'cmd': 'setText', 'text': big});
      // (A pair without the revision carries 1 MiB at most: that is over before the poll can see it, and the newer copy simply follows.)
      if (rig.session.limits.scale) {
        await eventually(
          () => rig.session.busy,
          'the big text to start crossing',
          timeout: const Duration(seconds: 20),
        );
        await Future<void>.delayed(const Duration(milliseconds: 150));
      } else {
        await Future<void>.delayed(const Duration(milliseconds: 20));
      }
      final newer = 'the newer copy ${toVm ? 'on the PC' : 'in the VM'}';
      await rig.peer(toVm).request({'cmd': 'setText', 'text': newer});
      await rig.expectArrived(
        toVm: toVm,
        what: 'the newer text',
        check: (b) => b['kind'] == 'text' && b['text'] == newer,
        timeout: const Duration(minutes: 2),
      );
      await eventually(() => !rig.session.busy, 'the session to be idle');
    }
  },
  // Explorer items inside a .zip folder, Outlook / Thunderbird attachments, phone and cloud namespaces: FileGroupDescriptorW +
  // FileContents and no CF_HDROP. They are read into a private folder and sent as files / a folder tree.
  'virtual files (zip contents, attachments) both ways': (rig) =>
      _virtualBothWays(
        rig,
        (s) => [
          _VItem('Docs\\', dir: true),
          _VItem('Docs\\readme.txt', size: 300, seed: s),
          _VItem(
            'Docs/sub/data.bin',
            size: 80000,
            mode: 'global',
            seed: s + 1,
          ), // '/' separators, an implied folder
          _VItem('Empty\\', dir: true),
          _VItem(
            'unknown-size.bin',
            size: 120000,
            declare: false,
            seed: s + 2,
          ), // read to the end of the stream
          _VItem('첨부 파일 (1).pdf', size: 50000, seed: s + 3),
          _VItem('empty.dat'),
        ],
      ),
  'virtual files, a single mail attachment both ways': (rig) =>
      _virtualBothWays(
        rig,
        (s) => [_VItem('Quarterly report.pdf', size: 70000, seed: s)],
      ),
  'virtual files with 300 entries are offered and pasted both ways':
      (rig) async {
        for (final toVm in [true, false]) {
          final items = [
            for (var i = 0; i < 300; i++)
              _VItem('Archive\\f$i.txt', size: 20 + i, seed: 200 + i),
          ];
          final mirror = await _mirrorOf(rig, items);
          final expected = entryKeys(
            await rig.peer(toVm).request({
              'cmd': 'describePaths',
              'paths': mirror,
            }) as Map,
          );
          await rig.transfer(
            toVm: toVm,
            copy: (source) => _copyVirtual(source, items),
          );
          await rig.expectArrived(
            toVm: toVm,
            what: 'offer of the virtual files',
            check: (b) => b['kind'] == 'offer' && b['files'] == 300,
          );
          final destination = rig.scratch('pasted');
          await rig.peer(!toVm).request({
            'cmd': 'paste',
            'dest': destination.path,
          }, timeout: const Duration(seconds: 120));
          expect(await _digest(rig, !toVm, destination), expected.join('|'));
        }
      },
  'virtual files without FileContents are refused, the clipboards stay':
      (rig) async {
        if (rig.sync) return;
        await rig.peer(true).request({
          'cmd': 'setVirtual',
          'entries': [
            {
              'name': 'shell-only.lnk',
              'dir': false,
              'mode': 'none',
              'declare': true,
              'data': '',
            },
          ],
        });
        await expectLater(rig.session.sendLocal(), throwsA(anything));
        expect(rig.session.error, 'unsupported_clipboard_format');
        expect(
          (await rig.board(false))['kind'],
          'empty',
          reason: 'nothing may reach the VM',
        );
      },

  'text both ways': (rig) async {
    for (final toVm in [true, false]) {
      final text = toVm ? '안녕 hello 🙂 PC' : 'from the VM ✓';
      await rig.transfer(toVm: toVm, copy: (s) => _copyText(s, text));
      final got = await rig.expectArrived(
        toVm: toVm,
        what: 'text',
        check: (b) => b['kind'] == 'text' && b['text'] == text,
      );
      expect(got['text'], text);
    }
  },
  'long text (200 KB) both ways': (rig) async {
    for (final toVm in [true, false]) {
      final text = List.generate(
        40000,
        (i) => 'word$i',
      ).join(' ').substring(0, 200000);
      await rig.transfer(toVm: toVm, copy: (s) => _copyText(s, text));
      await rig.expectArrived(
        toVm: toVm,
        what: 'long text',
        check: (b) => b['kind'] == 'text' && b['text'] == text,
      );
    }
  },
  'rich text (HTML) both ways': (rig) async {
    for (final toVm in [true, false]) {
      await rig.transfer(
        toVm: toVm,
        copy: (s) => s.request({
          'cmd': 'setRich',
          'text': 'bold and italic',
          'html': cfHtml('<b>bold</b> and <i>italic</i>'),
        }),
      );
      final got = await rig.expectArrived(
        toVm: toVm,
        what: 'rich text',
        check: (b) => b['kind'] == 'richtext' && b['text'] == 'bold and italic',
      );
      expect((got['html'] as String).contains('<b>bold</b>'), isTrue);
    }
  },
  'image both ways': (rig) async {
    for (final toVm in [true, false]) {
      final png = makePng(240, 160, seed: toVm ? 1 : 2);
      await rig.transfer(
        toVm: toVm,
        copy: (s) => s.request({
          'cmd': 'setImage',
          'png': base64Encode(png),
          'width': 240,
          'height': 160,
        }),
      );
      final got = await rig.expectArrived(
        toVm: toVm,
        what: 'image',
        check: (b) => b['kind'] == 'image',
      );
      expect(got['width'], 240);
      expect(got['height'], 160);
    }
  },
  // Snipping Tool / Win+Shift+S: the bitmap and its auto-saved PNG file are on the clipboard together. The user copied a picture.
  'screenshot (bitmap + its PNG file) arrives as an image both ways':
      (rig) async {
        for (final toVm in [true, false]) {
          final png = makePng(240, 160, seed: toVm ? 5 : 6);
          final file = await writeFile(
            rig.scratch('shot'),
            'Screenshot 2026-10-27 101010.png',
            png,
          );
          await rig.transfer(
            toVm: toVm,
            copy: (s) => s.request({
              'cmd': 'setScreenshot',
              'png': base64Encode(png),
              'width': 240,
              'height': 160,
              'paths': [file],
            }),
          );
          final got = await rig.expectArrived(
            toVm: toVm,
            what: 'screenshot',
            check: (b) => b['kind'] == 'image',
          );
          expect(got['width'], 240);
          expect(got['height'], 160);
        }
      },
  'bitmap next to other files or a folder stays the files': (rig) async {
    for (final toVm in [true, false]) {
      final png = makePng(40, 40, seed: 7);
      final dir = rig.scratch('mix');
      final picture = await writeFile(dir, 'pic.png', png);
      final note = await writeFile(dir, 'note.txt', noise(30, 3));
      await writeFile(dir, 'Folder/inner.txt', noise(20, 4));
      for (final paths in [
        [note],
        [picture, note],
        ['${dir.path}${Platform.pathSeparator}Folder'],
      ]) {
        late Map<dynamic, dynamic> sent;
        await rig.transfer(
          toVm: toVm,
          copy: (s) async {
            await s.request({
              'cmd': 'setScreenshot',
              'png': base64Encode(png),
              'width': 40,
              'height': 40,
              'paths': paths,
            });
            sent = await rig.board(toVm);
          },
        );
        await rig.expectArrived(
          toVm: toVm,
          what: 'files next to a bitmap ($paths)',
          check: (b) =>
              b['kind'] == 'files' &&
              entryKeys(b).join('|') == entryKeys(sent).join('|'),
        );
      }
    }
  },
  'one small file both ways': (rig) => _filesBothWays(rig, {'note.txt': 100}),
  'files 100 KB + 3 B + empty both ways': (rig) =>
      _filesBothWays(rig, {'big.bin': 100000, 'tiny.txt': 3, 'empty.dat': 0}),
  'files above one chunk (46 KB, 70 KB) both ways': (rig) =>
      _filesBothWays(rig, {'a.bin': 46081, 'b.bin': 70000}),
  'two files copied three times in a row': (rig) async {
    final paths = await _makeFiles(rig, {'first.bin': 80000, 'second.txt': 20});
    await rig.peer(true).request({'cmd': 'setFiles', 'paths': paths});
    final sent = await rig.board(true);
    for (var round = 1; round <= 3; round++) {
      if (rig.sync) {
        // The same copy again (a new clipboard sequence each time), a moment apart like repeated Ctrl+C.
        if (round > 1) {
          await rig.peer(true).request({'cmd': 'setFiles', 'paths': paths});
        }
        await Future<void>.delayed(const Duration(milliseconds: 400));
      } else {
        await rig.session.sendLocal();
      }
      await rig.expectArrived(
        toVm: true,
        what: 'files, round $round',
        check: (b) =>
            b['kind'] == 'files' &&
            entryKeys(b).join('|') == entryKeys(sent).join('|'),
      );
      // Different content in between, so the next round is not an echo of an unchanged clipboard.
      if (round < 3) {
        await rig.transfer(
          toVm: true,
          copy: (s) => _copyText(s, 'between $round'),
        );
        await rig.expectArrived(
          toVm: true,
          what: 'text between rounds',
          check: (b) => b['kind'] == 'text' && b['text'] == 'between $round',
        );
        await rig.peer(true).request({'cmd': 'setFiles', 'paths': paths});
      }
    }
  },
  'three different file copies quickly': (rig) async {
    for (var round = 0; round < 3; round++) {
      final paths = await _makeFiles(rig, {
        'r$round-a.bin': 60000 + round,
        'r$round-b.txt': 10 + round,
      }, seed: 30 + round);
      late Map<dynamic, dynamic> sent;
      await rig.transfer(
        toVm: true,
        copy: (s) async {
          await s.request({'cmd': 'setFiles', 'paths': paths});
          sent = await rig.board(true);
        },
      );
      await rig.expectArrived(
        toVm: true,
        what: 'quick files $round',
        check: (b) =>
            b['kind'] == 'files' &&
            entryKeys(b).join('|') == entryKeys(sent).join('|'),
      );
    }
  },
  'folder tree both ways (pushed)': (rig) async {
    for (final toVm in [true, false]) {
      final root = rig.scratch('tree');
      await writeFile(root, 'Docs/a.txt', noise(50, 3));
      await writeFile(root, 'Docs/sub/b.bin', noise(100000, 4));
      await Directory(
        '${root.path}${Platform.pathSeparator}Docs${Platform.pathSeparator}Empty',
      ).create(recursive: true);
      await writeFile(root, 'loose.txt', noise(7, 6));
      late Map<dynamic, dynamic> sent;
      await rig.transfer(
        toVm: toVm,
        copy: (s) async {
          await s.request({
            'cmd': 'setFiles',
            'paths': [
              '${root.path}${Platform.pathSeparator}Docs',
              '${root.path}${Platform.pathSeparator}loose.txt',
            ],
          });
          sent = await rig.board(toVm);
        },
      );
      await rig.expectArrived(
        toVm: toVm,
        what: 'tree',
        check: (b) =>
            b['kind'] == 'files' &&
            entryKeys(b).join('|') == entryKeys(sent).join('|'),
      );
    }
  },
  'large file (20 MiB) offered and pasted both ways': (rig) async {
    for (final toVm in [true, false]) {
      final directory = rig.scratch('large');
      final path = await writeBigFile(
        directory,
        'large.bin',
        20 * 1024 * 1024,
        toVm ? 71 : 72,
      );
      late Map<dynamic, dynamic> sent;
      await rig.transfer(
        toVm: toVm,
        copy: (s) async {
          await s.request({
            'cmd': 'setFiles',
            'paths': [path],
          });
          sent = await rig.board(toVm);
        },
      );
      await rig.expectArrived(
        toVm: toVm,
        what: 'offer of the large file',
        check: (b) => b['kind'] == 'offer' && b['files'] == 1,
      );
      final destination = rig.scratch('pasted');
      final pasted = await rig.peer(!toVm).request({
        'cmd': 'paste',
        'dest': destination.path,
      }, timeout: const Duration(seconds: 120));
      expect((pasted! as Map)['bytes'], 20 * 1024 * 1024);
      expect(await _digest(rig, !toVm, destination), entryKeys(sent).join('|'));
    }
  },
  'tree above 256 entries offered and pasted both ways': (rig) async {
    for (final toVm in [true, false]) {
      final root = rig.scratch('bigtree');
      for (var i = 0; i < 300; i++) {
        await writeFile(root, 'Many/f$i.txt', noise(20 + i, 100 + i));
      }
      await writeFile(root, 'Many/deep/inner.bin', noise(300000, 9));
      late Map<dynamic, dynamic> sent;
      await rig.transfer(
        toVm: toVm,
        copy: (s) async {
          await s.request({
            'cmd': 'setFiles',
            'paths': ['${root.path}${Platform.pathSeparator}Many'],
          });
          sent = await rig.board(toVm);
        },
      );
      await rig.expectArrived(
        toVm: toVm,
        what: 'offer of the tree',
        check: (b) => b['kind'] == 'offer' && b['files'] == 301,
      );
      final destination = rig.scratch('pasted');
      await rig.peer(!toVm).request({
        'cmd': 'paste',
        'dest': destination.path,
      }, timeout: const Duration(seconds: 120));
      expect(await _digest(rig, !toVm, destination), entryKeys(sent).join('|'));
    }
  },
  'unusual file names both ways': (rig) => _filesBothWays(rig, {
    '한글 파일 (1).txt': 40000,
    'a b.tar.gz': 50000,
    'aux-notes.txt': 10,
    'CONSOLE.log': 11,
    'résumé.docx': 47000,
    'ß.txt': 12,
    '.gitignore': 13,
    'name.with.many.dots.v1.2.3.bin': 60000,
  }),
  'received content is not echoed back': (rig) async {
    // A copy that arrives from the other side must not bounce (auto sharing watches both clipboards).
    final paths = await _makeFiles(rig, {'once.bin': 60000, 'twice.txt': 5});
    late Map<dynamic, dynamic> sent;
    await rig.transfer(
      toVm: false,
      copy: (s) async {
        await s.request({'cmd': 'setFiles', 'paths': paths});
        sent = await rig.board(false);
      },
    );
    await rig.expectArrived(
      toVm: false,
      what: 'files',
      check: (b) =>
          b['kind'] == 'files' &&
          entryKeys(b).join('|') == entryKeys(sent).join('|'),
    );
    final hostSequence = (await rig.board(false))['sequence'];
    final pcSequence = (await rig.board(true))['sequence'];
    await Future<void>.delayed(const Duration(seconds: 2));
    expect(
      (await rig.board(false))['sequence'],
      hostSequence,
      reason: 'the VM clipboard was rewritten (echo)',
    );
    expect(
      (await rig.board(true))['sequence'],
      pcSequence,
      reason: 'the PC clipboard was rewritten again (echo)',
    );
    expect(rig.session.busy, isFalse);
    // The same for text.
    await rig.transfer(toVm: true, copy: (s) => _copyText(s, 'ping'));
    await rig.expectArrived(
      toVm: true,
      what: 'text',
      check: (b) => b['kind'] == 'text' && b['text'] == 'ping',
    );
    final after = (await rig.board(true))['sequence'];
    await Future<void>.delayed(const Duration(seconds: 1));
    expect((await rig.board(true))['sequence'], after);
  },
  'Host reports every transfer (hostAction 253)': (rig) async {
    bool diag(int stage, int format) => rig.host.events.any(
      (e) =>
          e['event'] == 'diag' && e['stage'] == stage && e['format'] == format,
    );
    final paths = await _makeFiles(rig, {'a.bin': 70000, 'b.txt': 9});
    await rig.transfer(
      toVm: true,
      copy: (s) => s.request({'cmd': 'setFiles', 'paths': paths}),
    );
    await eventually(
      () => diag(1, 2) && diag(3, 2),
      'the receive begin and end of files',
    );
    await rig.transfer(
      toVm: false,
      copy: (s) => s.request({'cmd': 'setFiles', 'paths': paths}),
    );
    await eventually(
      () => diag(2, 2) && diag(4, 2),
      'the send begin and end of files',
    );
    // No content, names or paths in what the Host reports.
    for (final e in rig.host.events.where((e) => e['event'] == 'diag')) {
      expect(
        e.keys.toSet().difference({
          'event',
          'stage',
          'ok',
          'error',
          'cause',
          'format',
          'items',
          'kib',
        }),
        isEmpty,
      );
    }
    // The PC cancels a transfer under way: the Host says who ended it.
    final big = await writeBigFile(
      rig.scratch('cancel'),
      'cancel.bin',
      12 * 1024 * 1024,
      91,
    );
    await rig.peer(true).request({
      'cmd': 'setFiles',
      'paths': [big],
    });
    if (rig.sync) {
      await eventually(() => rig.session.busy, 'the automatic send to start');
    } else {
      unawaited(rig.session.sendLocal().then((_) {}, onError: (_) {}));
      await eventually(() => rig.session.busy, 'the send to start');
    }
    await Future<void>.delayed(const Duration(milliseconds: 200));
    await rig.session.cancelTransfer();
    await eventually(
      () => diag(8, 2) || diag(7, 2),
      'the Host to report that the peer ended the transfer',
      timeout: const Duration(seconds: 20),
    );
  },
  // The normal workflow: Ctrl+C in the VM, then straight to the PC to paste. Leaving the remote window must not lose the copy; a copy
  // made after the 2 s grace is not pulled (focus gate). Sharing only exists in sync mode.
  'copy in the VM, leave the window at once, paste on the PC': (rig) async {
    if (!rig.sync) return;
    final paths = await _makeFiles(rig, {
      'leave.bin': 60000,
      'leave.txt': 7,
    }, seed: 33);
    late Map<dynamic, dynamic> sent;
    await rig.peer(false).request({'cmd': 'setFiles', 'paths': paths});
    sent = await rig.board(false);
    await rig.session.setSharingActive(false);
    final got = await rig.expectArrived(
      toVm: false,
      what: 'files copied just before leaving',
      check: (b) =>
          b['kind'] == 'files' &&
          entryKeys(b).join('|') == entryKeys(sent).join('|'),
    );
    expect(entryKeys(got), entryKeys(sent));
    // Past the grace nothing more is pulled, however it is announced.
    await Future<void>.delayed(const Duration(milliseconds: 2600));
    final pc = (await rig.board(true))['sequence'];
    await rig.peer(false).request({
      'cmd': 'setText',
      'text': 'after the grace',
    });
    await Future<void>.delayed(const Duration(seconds: 2));
    expect(
      (await rig.board(true))['sequence'],
      pc,
      reason: 'a VM copy after the grace reached the PC',
    );
    // Coming back resumes sharing.
    await rig.session.setSharingActive(true);
    await Future<void>.delayed(
      const Duration(milliseconds: 1000),
    ); // the Host's watch is re-enabled
    await rig.peer(false).request({'cmd': 'setText', 'text': 'back again'});
    await rig.expectArrived(
      toVm: false,
      what: 'text after returning',
      check: (b) => b['kind'] == 'text' && b['text'] == 'back again',
    );
  },
  'alternating formats back and forth': (rig) async {
    for (var round = 0; round < 2; round++) {
      await rig.transfer(toVm: true, copy: (s) => _copyText(s, 'round $round'));
      await rig.expectArrived(
        toVm: true,
        what: 'text',
        check: (b) => b['kind'] == 'text' && b['text'] == 'round $round',
      );
      final paths = await _makeFiles(rig, {
        'alt$round.bin': 55000,
      }, seed: 80 + round);
      late Map<dynamic, dynamic> sent;
      await rig.transfer(
        toVm: true,
        copy: (s) async {
          await s.request({'cmd': 'setFiles', 'paths': paths});
          sent = await rig.board(true);
        },
      );
      await rig.expectArrived(
        toVm: true,
        what: 'files',
        check: (b) =>
            b['kind'] == 'files' &&
            entryKeys(b).join('|') == entryKeys(sent).join('|'),
      );
      await rig.transfer(toVm: false, copy: (s) => _copyText(s, 'back $round'));
      await rig.expectArrived(
        toVm: false,
        what: 'text back',
        check: (b) => b['kind'] == 'text' && b['text'] == 'back $round',
      );
    }
  },
};

/// A copy in the VM that is still arriving claims the PC clipboard: a paste made meanwhile finds nothing instead of the older content,
/// and the arriving files replace the claim. A copy made on the PC meanwhile is never overwritten.
Future<void> _claimScenario(Rig rig) async {
  await rig.peer(true).request({'cmd': 'setText', 'text': 'older PC content'});
  final big = await writeBigFile(
    rig.scratch('claim'),
    'claim.bin',
    12 * 1024 * 1024,
    61,
  );
  await rig.peer(false).request({
    'cmd': 'setFiles',
    'paths': [big],
  });
  final sent = await rig.board(false);
  if (!rig.sync) {
    unawaited(rig.session.receiveRemote().then((_) {}, onError: (_) {}));
  }
  // Every look at the PC clipboard while the copy arrives: the older text never comes back once it is gone.
  final kinds = <String>[];
  await eventually(
    () async {
      final b = await rig.board(true);
      kinds.add(b['kind'] as String);
      return b['kind'] == 'files' &&
          entryKeys(b).join('|') == entryKeys(sent).join('|');
    },
    'the files to arrive',
    timeout: const Duration(seconds: 60),
  );
  final firstGone = kinds.indexWhere((k) => k != 'text');
  expect(firstGone, isNot(-1));
  expect(
    kinds.sublist(firstGone).contains('text'),
    isFalse,
    reason: 'the older content was pasteable again: $kinds',
  );
  expect(
    kinds.contains('empty'),
    isTrue,
    reason: 'the clipboard was never claimed while the copy arrived: $kinds',
  );
}

/// The pasted directory in the form a board's entries have: the receiving helper describes it without touching its clipboard.
Future<String> _digest(Rig rig, bool pastedOnPc, Directory destination) async {
  final tops = destination.listSync().map((e) => e.path).toList();
  final described = await rig.peer(pastedOnPc).request({
    'cmd': 'describePaths',
    'paths': tops,
  }) as Map;
  return entryKeys(described).join('|');
}

int _chunks(Rig rig) => rig.host.events
    .where(
      (e) =>
          (e['event'] == 'emit' || e['event'] == 'recv') &&
          e['type'] == 'clipboard.chunk',
    )
    .length;

/// Messages that carry a folder list (an offer's entry pages, a deferred list's counts) as the Host sent or received them.
int _listMessages(Rig rig) => rig.host.events
    .where(
      (e) =>
          (e['event'] == 'emit' || e['event'] == 'recv') &&
          (e['type'] == 'clipboard.entries' || e['type'] == 'clipboard.manifest.begin'),
    )
    .length;

/// A folder of [count] small files below [name], in folders of a hundred.
Future<List<String>> _bigFolder(Rig rig, String name, int count) async {
  final root = rig.scratch('folder-$name');
  for (var i = 0; i < count; ++i) {
    await writeFile(root, '$name/d${i ~/ 100}/f$i.bin', noise(1 + i % 61, i));
  }
  return ['${root.path}${Platform.pathSeparator}$name'];
}

/// 200,001 empty files in folders of a thousand (`Huge/<n>`). Made once (creating them takes minutes where an antivirus watches them)
/// and kept next to the run folders.
Future<String> _hugeFixture(Rig rig) async {
  final sep = Platform.pathSeparator;
  final kept = Directory('${Directory(_env['E2E_WORK']!).parent.path}${sep}fixture-200001');
  final huge = '${kept.path}${sep}Huge';
  if (!File('${kept.path}${sep}complete').existsSync()) {
    if (kept.existsSync()) kept.deleteSync(recursive: true);
    kept.createSync(recursive: true);
    await rig.client.request(
      {'cmd': 'makeEmptyFiles', 'root': huge, 'total': 200001, 'perFolder': 1000},
      timeout: const Duration(minutes: 20),
    );
    File('${kept.path}${sep}complete').writeAsStringSync('200001');
  }
  return huge;
}

/// List requests the Host sent or received (each starts a walk on the offering side).
int _listRequests(Rig rig) => rig.host.events
    .where((e) => (e['event'] == 'emit' || e['event'] == 'recv') && e['type'] == 'clipboard.manifest')
    .length;

String _hostTrace(Rig rig) {
  final host = rig.host.events
      .map((e) => e['event'] == 'emit' || e['event'] == 'recv'
          ? '${e['event']}:${e['type']}:${e['requestId']}${e['reason'] != null && e['reason'] != '' ? '(${e['reason']})' : ''}${e['type'] == 'clipboard.watch' ? '(enabled=${e['enabled']})' : ''}'
          : '${e['event']}${e['what'] != null ? '(${e['what']})' : ''}${e['stage'] != null ? '/stage=${e['stage']}/error=${e['error']}/cause=${e['cause']}' : ''}')
      .where((line) => !line.contains('clipboard.chunk') && !line.contains('clipboard.entries'))
      .join(' ');
  return 'session error: ${rig.session.error}, relay closures: ${rig.relay.closures}, host: $host';
}

Future<void> _onPasteOffer(Rig rig, bool toVm, List<String> paths) async {
  final previous = (await rig.board(!toVm))['sequence'];
  await rig.peer(toVm).request({'cmd': 'setFiles', 'paths': paths});
  await eventually(() async {
    final board = await rig.board(!toVm);
    return board['kind'] == 'offer' &&
        board['offerFormat'] == 'tree' &&
        board['sequence'] != previous;
  }, 'a new on-paste file announcement');
}

final onPasteScenarios = <String, ({Scenario run, bool dropWaiting, int delayMs})>{
  'small files are silent until a paste, CF_HDROP and virtual streams reuse one copy':
      (
        dropWaiting: false,
        delayMs: 0,
        run: (rig) async {
          for (final toVm in [true, false]) {
            final paths = await _makeFiles(rig, {
              'small.bin': 150123,
              'empty.txt': 0,
            });
            final expected = entryKeys(
              await rig.peer(toVm).request({
                'cmd': 'describePaths',
                'paths': paths,
              }) as Map,
            );
            final before = _chunks(rig);
            await _onPasteOffer(rig, toVm, paths);
            await Future<void>.delayed(const Duration(milliseconds: 900));
            expect(
              _chunks(rig),
              before,
              reason: 'copying must never transfer file bytes',
            );
            expect(rig.session.userWaiting, isFalse);
            expect(rig.session.transferring, isFalse);
            final formats =
                await rig.peer(!toVm).request({'cmd': 'formats'}) as Map;
            expect(formats['formats'], contains('CF_HDROP'));
            expect(
              formats['explorerProgress'],
              isFalse,
              reason: 'the Multitap card owns progress',
            );
            final rendered =
                await rig.peer(!toVm).request({'cmd': 'renderHdrop'}) as Map;
            expect(entryKeys(rendered['board'] as Map), expected);
            final afterHdrop = _chunks(rig);
            expect(afterHdrop, greaterThan(before));
            final destination = rig.scratch('second-consumer');
            await rig.peer(!toVm).request({
              'cmd': 'paste',
              'dest': destination.path,
            });
            expect(await _digest(rig, !toVm, destination), expected.join('|'));
            expect(
              _chunks(rig),
              afterHdrop,
              reason: 'a consumer reading virtual streams must reuse the staged CF_HDROP copy',
            );
          }
        },
      ),
  'preparePaste waits for the announcement, never for file bytes': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      final paths = await _makeFiles(rig, {'first.bin': 70000});
      final before = _chunks(rig);
      await rig.client.request({'cmd': 'setFiles', 'paths': paths});
      expect(
        await rig.session.preparePaste().timeout(const Duration(seconds: 10)),
        isTrue,
      );
      expect((await rig.board(false))['kind'], 'offer');
      expect(_chunks(rig), before);
      final destination = rig.scratch('immediate-paste');
      await rig.host.request({'cmd': 'paste', 'dest': destination.path});
      expect(
        await File('${destination.path}${Platform.pathSeparator}first.bin')
            .readAsBytes(),
        await File(paths.first).readAsBytes(),
      );
    },
  ),
  'more than 256 entries remains virtual-only and pastes both ways': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      final root = rig.scratch('virtual-only');
      for (var i = 0; i < 257; ++i) {
        await writeFile(root, 'Many/f$i.txt', noise(i % 37, i));
      }
      final paths = ['${root.path}${Platform.pathSeparator}Many'];
      final expected = entryKeys(
        await rig.client.request({'cmd': 'describePaths', 'paths': paths})
            as Map,
      );
      for (final toVm in [true, false]) {
        final before = _chunks(rig);
        await _onPasteOffer(rig, toVm, paths);
        final formats =
            await rig.peer(!toVm).request({'cmd': 'formats'}) as Map;
        expect(formats['formats'], isNot(contains('CF_HDROP')));
        expect(formats['explorerProgress'], isFalse);
        expect(_chunks(rig), before);
        final destination = rig.scratch('virtual-paste');
        await rig.peer(!toVm).request({
          'cmd': 'paste',
          'dest': destination.path,
        });
        expect(await _digest(rig, !toVm, destination), expected.join('|'));
      }
    },
  ),
  'deferred: a large folder crosses without its list until a paste, both ways': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      final paths = await _bigFolder(rig, 'Big', 3000);
      final expected = entryKeys(
        await rig.client.request({'cmd': 'describePaths', 'paths': paths})
            as Map,
      );
      for (final toVm in [true, false]) {
        final lists = _listMessages(rig), chunks = _chunks(rig);
        await _onPasteOffer(rig, toVm, paths);
        await Future<void>.delayed(const Duration(milliseconds: 600));
        expect(
          (await rig.board(!toVm))['files'],
          0,
          reason: 'a deferred offer has no list until something pastes it',
        );
        expect(_listMessages(rig), lists, reason: 'copying must not walk or send the list');
        expect(_chunks(rig), chunks);
        expect(rig.session.userWaiting, isFalse);
        final formats = await rig.peer(!toVm).request({'cmd': 'formats'}) as Map;
        expect(formats['formats'], isNot(contains('CF_HDROP')));
        final destination = rig.scratch('deferred-paste');
        await rig.peer(!toVm).request({'cmd': 'paste', 'dest': destination.path});
        expect(await _digest(rig, !toVm, destination), expected.join('|'));
        expect(_listMessages(rig), greaterThan(lists));
        // A second paste of the same copy reuses the list (no new walk).
        final again = _listMessages(rig);
        final second = rig.scratch('deferred-again');
        await rig.peer(!toVm).request({'cmd': 'paste', 'dest': second.path});
        expect(await _digest(rig, !toVm, second), expected.join('|'));
        expect(_listMessages(rig), again);
      }
    },
  ),
  'a renumbered clipboard (no new copy) still pastes, small and deferred, both ways': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      // The 2026-11-13 field failure: the source clipboard's number moved without a new copy between the copy and the paste.
      final small = await _makeFiles(rig, {'small.bin': 90000});
      final large = await _bigFolder(rig, 'Renumbered', 400);
      for (final paths in [small, large]) {
        final expected = entryKeys(
          await rig.client.request({'cmd': 'describePaths', 'paths': paths})
              as Map,
        );
        for (final toVm in [true, false]) {
          await _onPasteOffer(rig, toVm, paths);
          await rig.peer(toVm).request({'cmd': 'renumber'});
          await rig.peer(toVm).request({'cmd': 'renumber'});
          final destination = rig.scratch('renumbered-paste');
          await rig.peer(!toVm).request({'cmd': 'paste', 'dest': destination.path});
          expect(await _digest(rig, !toVm, destination), expected.join('|'));
        }
      }
    },
  ),
  'deferred: a newer copy before the paste is what pastes, both ways': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      final first = await _bigFolder(rig, 'First', 300);
      final second = await _bigFolder(rig, 'Second', 350);
      final expected = entryKeys(
        await rig.client.request({'cmd': 'describePaths', 'paths': second})
            as Map,
      );
      for (final toVm in [true, false]) {
        await _onPasteOffer(rig, toVm, first);
        await _onPasteOffer(rig, toVm, second);
        final destination = rig.scratch('newer-paste');
        await rig.peer(!toVm).request({'cmd': 'paste', 'dest': destination.path});
        expect(await _digest(rig, !toVm, destination), expected.join('|'));
      }
    },
  ),
  'deferred: more than 200,000 entries is refused when pasted, with the entry limit': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      // 200,001 empty files: the copy is announced at once; the paste's walk meets the entry limit and says so.
      // Made once (creating 200,001 files takes minutes where an antivirus watches them) and kept next to the run folders.
      final paths = [await _hugeFixture(rig)];
      for (final toVm in [true, false]) {
        final lists = _listMessages(rig);
        try {
          await _onPasteOffer(rig, toVm, paths);
        } catch (error) {
          throw StateError('$error; host events: ${rig.host.events.map((e) => '${e['event']}:${e['type'] ?? ''}:${e['reason'] ?? ''}:${e['stage'] ?? ''}').join(' ')}; session error: ${rig.session.error}');
        }
        expect(_listMessages(rig), lists, reason: 'the copy itself must not walk 200,001 entries');
        final destination = rig.scratch('too-many-paste');
        await expectLater(
          rig.peer(!toVm).request({'cmd': 'paste', 'dest': destination.path}),
          throwsA(anything),
        );
        if (!toVm) {
          await eventually(
            () => rig.session.error == 'clipboard_limits_exceeded' && rig.session.errorDetail == 'entries',
            'the entry limit named on this PC',
          );
        } else {
          await eventually(
            () => _hostReason(rig) == 'clipboard_entry_count_limit' || rig.session.errorDetail == 'entries',
            'the entry limit named for the VM paste',
          );
        }
      }
    },
  ),
  // A paste under way keeps its copy from the moment it asks for the list (OfferRegistry::listing): a copy made on the offering side
  // while the list is walked does not throw the walk away (until 1.1.6 the walked copy was dropped and the paste failed as expired).
  'deferred: a copy made while the paste walks a large folder does not cut it off, both ways': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      final huge = await _hugeFixture(rig);
      final folders = (Directory(huge).listSync().whereType<Directory>().map((d) => d.path).toList()..sort()).take(60).toList();
      final files = folders.fold<int>(0, (sum, folder) => sum + Directory(folder).listSync(recursive: true).whereType<File>().length);
      for (final toVm in [true, false]) {
        await _onPasteOffer(rig, toVm, folders);
        final asked = _listRequests(rig);
        final destination = rig.scratch('walk-newer');
        final paste = rig
            .peer(!toVm)
            .request({'cmd': 'paste', 'dest': destination.path}, timeout: const Duration(minutes: 15))
            .then<Object?>((value) => value, onError: (Object error) => error);
        await eventually(() => _listRequests(rig) > asked, 'the paste to ask for the list');
        final newer = await _makeFiles(rig, {'newer-during-walk.bin': 777});
        await rig.peer(toVm).request({'cmd': 'setFiles', 'paths': newer});
        final older = await paste;
        expect(older, isA<Map<dynamic, dynamic>>(),
            reason: 'the paste ${toVm ? 'PC to VM' : 'VM to PC'} goes on after a copy during its walk: $older; host: ${_hostTrace(rig)}');
        final pasted = destination.listSync(recursive: true).whereType<File>().length;
        expect(pasted, files, reason: 'every file of the walked copy');
        // The next paste gets the newer copy.
        await eventually(() async => (await rig.board(!toVm))['kind'] == 'offer', 'the newer copy announced');
        final next = rig.scratch('walk-newer-next');
        await rig.peer(!toVm).request({'cmd': 'paste', 'dest': next.path});
        expect(await File('${next.path}${Platform.pathSeparator}newer-during-walk.bin').readAsBytes(), await File(newer.single).readAsBytes());
        expect(rig.session.ready, isTrue);
      }
    },
  ),
  // The user cancels a paste while its list is walked and pastes the same copy again at once: the older walk ends (the PC's walk
  // holds its message lane; its cancel is taken as it arrives), and the newer request is answered, on the same channel.
  'deferred: a paste cancelled during the walk, then the same copy pasted again, both ways': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      final huge = await _hugeFixture(rig);
      final folders = (Directory(huge).listSync().whereType<Directory>().map((d) => d.path).toList()..sort()).skip(60).take(60).toList();
      // (One folder of the fixture holds the 200,001st file alone: count what was copied.)
      final files = folders.fold<int>(0, (sum, folder) => sum + Directory(folder).listSync(recursive: true).whereType<File>().length);
      for (final toVm in [true, false]) {
        await _onPasteOffer(rig, toVm, folders);
        final asked = _listRequests(rig);
        final first = rig.scratch('walk-cancel');
        final paste = rig
            .peer(!toVm)
            .request({'cmd': 'paste', 'dest': first.path}, timeout: const Duration(minutes: 5))
            .then<Object?>((value) => value, onError: (Object error) => error);
        await eventually(() => _listRequests(rig) > asked, 'the paste to ask for the list');
        await Future<void>.delayed(const Duration(milliseconds: 300));
        await rig.session.cancelTransfer();
        expect(await paste, anyOf(isA<Exception>(), isA<Error>()), reason: 'the cancelled paste fails');
        expect(rig.session.ready, isTrue, reason: 'the channel stays after a cancel: ${_hostTrace(rig)}');
        final again = rig.scratch('walk-cancel-again');
        final result = await rig
            .peer(!toVm)
            .request({'cmd': 'paste', 'dest': again.path}, timeout: const Duration(minutes: 15))
            .then<Object?>((value) => value, onError: (Object error) => error);
        expect(result, isA<Map<dynamic, dynamic>>(), reason: 'the same copy pastes again: $result; host: ${_hostTrace(rig)}');
        expect(again.listSync(recursive: true).whereType<File>().length, files);
        expect(rig.session.ready, isTrue);
      }
    },
  ),
  // A copy made on the side that pastes, while the paste moves bytes, is that side's next copy: the paste under way goes on (until
  // 1.1.6 the Host ended a file or list it was serving when the PC's next copy arrived).
  'a copy on the pasting side does not cut off a paste under way, both ways': (
    dropWaiting: false,
    delayMs: 20,
    run: (rig) async {
      for (final toVm in [true, false]) {
        final paths = await _makeFiles(rig, {'under-way.bin': 5 * mib});
        await _onPasteOffer(rig, toVm, paths);
        final destination = rig.scratch('pasting-side-copy');
        final chunks = _chunks(rig);
        final paste = rig
            .peer(!toVm)
            .request({'cmd': 'paste', 'dest': destination.path})
            .then<Object?>((value) => value, onError: (Object error) => error);
        await eventually(() => rig.session.transferring && _chunks(rig) > chunks + 4, 'the paste to move bytes');
        final text = 'copied on the ${toVm ? 'VM' : 'PC'} while pasting';
        await rig.peer(!toVm).request({'cmd': 'setText', 'text': text});
        final result = await paste;
        expect(result, isA<Map<dynamic, dynamic>>(),
            reason: 'the paste ${toVm ? 'PC to VM' : 'VM to PC'} goes on after a copy on its own side: $result; host: ${_hostTrace(rig)}');
        expect(await File('${destination.path}${Platform.pathSeparator}under-way.bin').readAsBytes(), await File(paths.single).readAsBytes());
        await rig.expectArrived(toVm: !toVm, what: 'the copy made while pasting', check: (b) => b['kind'] == 'text' && b['text'] == text);
        expect(rig.session.ready, isTrue);
      }
    },
  ),
  'large text and rich text render on paste in either direction': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      final text = _longText(2 * mib + 17);
      final digest = sha256.convert(utf8.encode(text)).toString();
      for (final rich in [false, true]) {
        for (final toVm in [true, false]) {
          final before = _chunks(rig);
          final html = cfHtml('<b>$text</b>');
          await rig.peer(toVm).request({
            'cmd': rich ? 'setRich' : 'setText',
            'text': text,
            if (rich) 'html': html,
          });
          await rig.expectArrived(
            toVm: toVm,
            what: 'delayed content offer',
            check: (b) =>
                b['kind'] == 'offer' &&
                b['offerFormat'] == (rich ? 'richtext' : 'text'),
          );
          expect(_chunks(rig), before);
          final result =
              await rig.peer(!toVm).request({'cmd': 'renderText'}) as Map;
          expect(result['textLength'], text.length);
          expect(result['textSha'], digest);
          if (rich) {
            final rendered =
                await rig.peer(!toVm).request({'cmd': 'renderHtml'}) as Map;
            expect(rendered['html'], contains('<b>$text</b>'));
          }
          expect(rig.session.ready, isTrue);
        }
      }
    },
  ),
  'small text and images sync silently, a large image waits for paste': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      final small = makePng(60, 40, seed: 17);
      final large = makePng(1500, 1500, seed: 18);
      expect(large.length, greaterThan(8 * mib));
      for (final toVm in [true, false]) {
        await rig.peer(toVm).request({
          'cmd': 'setText',
          'text': 'small ${toVm ? 'PC' : 'VM'} text',
        });
        await rig.expectArrived(
          toVm: toVm,
          what: 'small text sync',
          check: (b) =>
              b['kind'] == 'text' &&
              b['text'] == 'small ${toVm ? 'PC' : 'VM'} text',
        );
        await eventually(
          () => !rig.session.userWaiting,
          'the preceding paste waiting episode to end',
        );
        expect(rig.session.userWaiting, isFalse);
        await rig.peer(toVm).request({
          'cmd': 'setImage',
          'png': base64Encode(small),
          'width': 60,
          'height': 40,
        });
        await rig.expectArrived(
          toVm: toVm,
          what: 'small image sync',
          check: (b) =>
              b['kind'] == 'image' &&
              b['pngSha'] == sha256.convert(small).toString(),
        );
        expect(rig.session.userWaiting, isFalse);
        final before = _chunks(rig);
        await rig.peer(toVm).request({
          'cmd': 'setImage',
          'png': base64Encode(large),
          'width': 1500,
          'height': 1500,
        });
        await rig.expectArrived(
          toVm: toVm,
          what: 'large image offer',
          check: (b) => b['kind'] == 'offer' && b['offerFormat'] == 'image',
        );
        expect(_chunks(rig), before);
        final rendered =
            await rig.peer(!toVm).request({'cmd': 'renderImage'}) as Map;
        expect(rendered['pngSha'], sha256.convert(large).toString());
        expect(rendered['width'], 1500);
        expect(rendered['height'], 1500);
      }
    },
  ),
  'Host without a waiting event stays silent while serving a VM paste': (
    dropWaiting: true,
    delayMs: 15,
    run: (rig) async {
      final paths = await _makeFiles(rig, {'slow.bin': 3 * mib});
      await _onPasteOffer(rig, true, paths);
      final destination = rig.scratch('no-waiting');
      final paste = rig.host.request({
        'cmd': 'paste',
        'dest': destination.path,
      });
      await eventually(() => rig.session.transferring, 'the VM to pull');
      await Future<void>.delayed(const Duration(milliseconds: 800));
      expect(
        rig.session.userWaiting,
        isFalse,
        reason: 'serving by itself does not prove a waiting paste',
      );
      await paste;
      expect(
        await File('${destination.path}${Platform.pathSeparator}slow.bin')
            .readAsBytes(),
        await File(paths.first).readAsBytes(),
      );
    },
  ),
  'cancel a slow paste, then paste a newer copy without closing the channel': (
    dropWaiting: false,
    delayMs: 20,
    run: (rig) async {
      for (final toVm in [true, false]) {
        final paths = await _makeFiles(rig, {'cancel.bin': 5 * mib});
        await _onPasteOffer(rig, toVm, paths);
        final destination = rig.scratch('cancel-paste');
        final paste = rig
            .peer(!toVm)
            .request({'cmd': 'paste', 'dest': destination.path})
            .then<Object?>((value) => value, onError: (Object error) => error);
        await eventually(
          () => rig.session.transferring && rig.session.userWaiting,
          'a paste to wait',
        );
        await rig.session.cancelTransfer();
        expect(await paste, anyOf(isA<Exception>(), isA<Error>()));
        expect(rig.session.ready, isTrue);
        final next = await _makeFiles(rig, {'next.bin': 123});
        await _onPasteOffer(rig, toVm, next);
        final target = rig.scratch('after-cancel');
        await rig.peer(!toVm).request({'cmd': 'paste', 'dest': target.path});
        expect(
          await File('${target.path}${Platform.pathSeparator}next.bin')
              .readAsBytes(),
          await File(next.first).readAsBytes(),
        );
      }
    },
  ),
  'a changed source fails the paste and the pair stays available': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      for (final toVm in [true, false]) {
        final paths = await _makeFiles(rig, {'changed.bin': 12345});
        await _onPasteOffer(rig, toVm, paths);
        await File(paths.single).writeAsBytes([1, 2, 3]);
        final target = rig.scratch('changed');
        await expectLater(
          rig.peer(!toVm).request({'cmd': 'paste', 'dest': target.path}),
          throwsA(anything),
        );
        await eventually(
          () => rig.session.error == 'clipboard_selected_file_changed',
          'the precise source-change error',
        );
        expect(rig.session.status, contains('원본 파일'));
        expect(rig.session.ready, isTrue);
      }
    },
  ),
  'a source folder can be renamed before and during paste, and the pasted folder can be renamed': (
    dropWaiting: false,
    delayMs: 20,
    run: (rig) async {
      for (final toVm in [true, false]) {
        final root = rig.scratch('rename');
        final bytes = noise(3 * mib, toVm ? 31 : 32);
        await writeFile(root, 'Original/data.bin', bytes);
        var folder = Directory('${root.path}${Platform.pathSeparator}Original');
        await _onPasteOffer(rig, toVm, [folder.path]);
        folder = await folder.rename(
          '${root.path}${Platform.pathSeparator}Renamed',
        );
        final first = rig.scratch('renamed-before-paste');
        await rig.peer(!toVm).request({'cmd': 'paste', 'dest': first.path});
        expect(
          await File(
            '${first.path}${Platform.pathSeparator}Original${Platform.pathSeparator}data.bin',
          ).readAsBytes(),
          bytes,
        );
        await _onPasteOffer(rig, toVm, [folder.path]);
        final second = rig.scratch('renamed-during-paste');
        final paste = rig.peer(!toVm).request({
          'cmd': 'paste',
          'dest': second.path,
        });
        await eventually(
          () => rig.session.transferring,
          'the file stream to start',
        );
        var renamed = false;
        await eventually(
          () async {
            if (renamed) return true;
            try {
              folder = await folder.rename(
                '${root.path}${Platform.pathSeparator}Moved',
              );
              renamed = true;
            } on FileSystemException {
              return false;
            }
            return true;
          },
          'source directory rename between range reads',
          timeout: const Duration(seconds: 5),
        );
        await paste;
        final received = await Directory(
          '${second.path}${Platform.pathSeparator}Renamed',
        ).rename('${second.path}${Platform.pathSeparator}Received-renamed');
        expect(
          await File('${received.path}${Platform.pathSeparator}data.bin')
              .readAsBytes(),
          bytes,
        );
        expect(rig.session.ready, isTrue);
      }
    },
  ),
  'a source deleted before paste reports source changed without closing the pair':
      (
        dropWaiting: false,
        delayMs: 0,
        run: (rig) async {
          for (final toVm in [true, false]) {
            final paths = await _makeFiles(rig, {'deleted.bin': 12345});
            await _onPasteOffer(rig, toVm, paths);
            await File(paths.single).delete();
            final target = rig.scratch('deleted-source');
            await expectLater(
              rig.peer(!toVm).request({'cmd': 'paste', 'dest': target.path}),
              throwsA(anything),
            );
            await eventually(
              () => rig.session.error == 'clipboard_selected_file_changed',
              'the deleted-source reason',
            );
            expect(rig.session.ready, isTrue);
          }
        },
      ),
  // A paste under way keeps its copy, as RDP locks a paste's clipboard data (OfferRegistry::pasteHold): a newer copy does not cut it
  // off, and the next paste gets the newer files. (Until 2026-11-13 this scenario expected the older paste to fail, the opposite of
  // that rule; it failed on the code before the deferred manifest too.)
  'a newer copy does not cut a slow paste off, and only the newer files are served next': (
    dropWaiting: false,
    delayMs: 20,
    run: (rig) async {
      for (final toVm in [true, false]) {
        final paths = await _makeFiles(rig, {'older.bin': 5 * mib});
        await _onPasteOffer(rig, toVm, paths);
        final first = rig.scratch('superseded');
        final chunks = _chunks(rig);
        final paste = rig
            .peer(!toVm)
            .request({'cmd': 'paste', 'dest': first.path})
            .then<Object?>((value) => value, onError: (Object error) => error);
        // Under way means bytes are moving: a copy made before the first file was even admitted is a new copy for that paste.
        await eventually(
          () => rig.session.transferring && _chunks(rig) > chunks + 4,
          'the older paste to move bytes',
        );
        final newer = await _makeFiles(rig, {'newer.bin': 23456});
        await _onPasteOffer(rig, toVm, newer);
        final older = await paste;
        expect(
          older,
          isA<Map<dynamic, dynamic>>(),
          reason:
              'the older paste ${toVm ? 'PC to VM' : 'VM to PC'} goes on after a newer copy: $older; host events: '
              '${rig.host.events.where((e) => e['event'] == 'emit' || e['event'] == 'recv').map((e) => '${e['event']}:${e['type']}:${e['requestId']}${e['reason'] != null && e['reason'] != '' ? '(${e['reason']})' : ''}').join(' ')}',
        );
        expect(
          await File('${first.path}${Platform.pathSeparator}older.bin').readAsBytes(),
          await File(paths.single).readAsBytes(),
        );
        final next = rig.scratch('newer');
        await rig.peer(!toVm).request({'cmd': 'paste', 'dest': next.path});
        expect(
          await File('${next.path}${Platform.pathSeparator}newer.bin')
              .readAsBytes(),
          await File(newer.single).readAsBytes(),
        );
        expect(
          await File('${next.path}${Platform.pathSeparator}older.bin').exists(),
          isFalse,
        );
        expect(rig.session.ready, isTrue);
      }
    },
  ),
  'withdraw clears only the still-current peer offer': (
    dropWaiting: false,
    delayMs: 0,
    run: (rig) async {
      for (final toVm in [true, false]) {
        final paths = await _makeFiles(rig, {'withdraw.bin': 12345});
        await _onPasteOffer(rig, toVm, paths);
        await rig.peer(toVm).request({'cmd': 'clear'});
        await eventually(
          () async => (await rig.board(!toVm))['kind'] == 'empty',
          'withdrawn offer to leave the clipboard',
        );
        await _onPasteOffer(rig, toVm, paths);
        await rig.peer(!toVm).request({
          'cmd': 'setText',
          'text': 'new local copy',
        });
        await rig.peer(toVm).request({'cmd': 'clear'});
        await Future<void>.delayed(const Duration(milliseconds: 800));
        expect((await rig.board(!toVm))['text'], 'new local copy');
        expect(rig.session.ready, isTrue);
      }
    },
  ),
};

/// Policy combinations: what is refused is refused with the fixed reason, and what is allowed still flows.
final policyScenarios = <String, ({ClipboardPolicy policy, Scenario run})>{
  'files denied: screenshot and files are refused, text flows': (
    policy: const ClipboardPolicy(textToRemote: true, textToLocal: true),
    run: (rig) async {
      final png = makePng(60, 40, seed: 8);
      final file = await writeFile(rig.scratch('deny'), 'shot.png', png);
      await rig.peer(true).request({
        'cmd': 'setScreenshot',
        'png': base64Encode(png),
        'width': 60,
        'height': 40,
        'paths': [file],
      });
      final vm = (await rig.board(false))['sequence'];
      if (!rig.sync) {
        await expectLater(rig.session.sendLocal(), throwsA(anything));
        expect(rig.session.error, 'clipboard_policy_denied');
      }
      await rig.peer(false).request({
        'cmd': 'setScreenshot',
        'png': base64Encode(png),
        'width': 60,
        'height': 40,
        'paths': [],
      });
      if (!rig.sync) {
        await expectLater(rig.session.receiveRemote(), throwsA(anything));
        expect(rig.session.error, 'clipboard_policy_denied');
      }
      await Future<void>.delayed(const Duration(milliseconds: 800));
      expect(
        (await rig.board(false))['kind'],
        'image',
        reason: 'the VM clipboard must keep its own content',
      );
      expect(
        (await rig.board(true))['kind'],
        'files',
        reason: 'the PC clipboard must keep its own content',
      );
      expect(vm, isNotNull);
      await rig.transfer(
        toVm: true,
        copy: (s) => _copyText(s, 'text still flows'),
      );
      await rig.expectArrived(
        toVm: true,
        what: 'text',
        check: (b) => b['kind'] == 'text' && b['text'] == 'text still flows',
      );
    },
  ),
  'text denied: a picture flows, text is refused': (
    policy: const ClipboardPolicy(filesToRemote: true, filesToLocal: true),
    run: (rig) async {
      final png = makePng(60, 40, seed: 9);
      await rig.transfer(
        toVm: true,
        copy: (s) => s.request({
          'cmd': 'setImage',
          'png': base64Encode(png),
          'width': 60,
          'height': 40,
        }),
      );
      await rig.expectArrived(
        toVm: true,
        what: 'image',
        check: (b) => b['kind'] == 'image',
      );
      await rig.peer(false).request({'cmd': 'setText', 'text': 'refused'});
      if (!rig.sync) {
        await expectLater(rig.session.receiveRemote(), throwsA(anything));
        expect(rig.session.error, 'clipboard_policy_denied');
      }
      await Future<void>.delayed(const Duration(milliseconds: 800));
      expect(
        (await rig.board(true))['kind'],
        'image',
        reason: 'refused text must not reach the PC',
      );
    },
  ),
};
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  if (!_enabled) {
    test(
      'clipboard end-to-end harness (set MULTITAP_CLIPBOARD_E2E=1)',
      () {},
      skip: 'run tools/clipboard-e2e-qa/Run-ClipboardE2E.ps1',
    );
    return;
  }
  setUpAll(() => HttpOverrides.global = null);
  final only = _env['E2E_ONLY'] ?? '';
  final servers = <String, String>{
    for (final item
        in (_env['E2E_SERVERS'] ?? '').split(';').where((s) => s.isNotEmpty))
      item.split('=').first: item.substring(item.indexOf('=') + 1),
  };
  for (final server in servers.entries) {
    for (final scenario in onPasteScenarios.entries) {
      final name = '${server.key}/onpaste/${scenario.key}';
      if (only.isNotEmpty && !name.contains(only)) continue;
      test(name, () async {
        final rig = Rig(
          server.key,
          server.value,
          true,
          onPaste: true,
          dropWaiting: scenario.value.dropWaiting,
          delayMs: scenario.value.delayMs,
        );
        try {
          await rig.up();
          if (!rig.session.limits.onPaste) {
            markTestSkipped(
              'This relay predates the on-paste revision; the compatibility scenario exercises its fallback.',
            );
            return;
          }
          if (scenario.key.startsWith('deferred:') && !rig.session.deferredManifest) {
            markTestSkipped(
              'This relay predates the deferred manifest; "without the deferred manifest" covers the walked offer it keeps.',
            );
            return;
          }
          await scenario.value.run(rig);
          expect(rig.relay.closures, isEmpty);
        } finally {
          await rig.down();
        }
      // The first run of the 200,001-entry scenario makes its fixture (minutes).
      }, timeout: Timeout(Duration(minutes: scenario.key.contains('200,000') ? 25 : 4)));
    }
    // A Host (or relay) without the deferred manifest: a large copy keeps its list from copy time and pastes as before.
    final withoutDeferred =
        '${server.key}/onpaste/without the deferred manifest a large folder is walked at copy time and pastes both ways';
    if (only.isEmpty || withoutDeferred.contains(only)) {
      test(withoutDeferred, () async {
        final rig = Rig(server.key, server.value, true, onPaste: true, deferred: false);
        try {
          await rig.up();
          if (!rig.session.limits.onPaste) {
            markTestSkipped('This relay predates the on-paste revision.');
            return;
          }
          expect(rig.session.deferredManifest, isFalse);
          final paths = await _bigFolder(rig, 'Walked', 600);
          final expected = entryKeys(
            await rig.client.request({'cmd': 'describePaths', 'paths': paths}) as Map,
          );
          for (final toVm in [true, false]) {
            final lists = _listMessages(rig);
            await _onPasteOffer(rig, toVm, paths);
            expect(_listMessages(rig), greaterThan(lists), reason: 'without the revision the list crosses at copy time');
            final destination = rig.scratch('walked-paste');
            await rig.peer(!toVm).request({'cmd': 'paste', 'dest': destination.path});
            expect(await _digest(rig, !toVm, destination), expected.join('|'));
          }
          expect(rig.relay.closures, isEmpty);
        } finally {
          await rig.down();
        }
      }, timeout: const Timeout(Duration(minutes: 4)));
    }
    final compatibilityName =
        '${server.key}/onpaste/negotiation falls back on a relay without the revision';
    if (only.isEmpty || compatibilityName.contains(only)) {
      test(compatibilityName, () async {
        final rig = Rig(server.key, server.value, true, onPaste: true);
        try {
          await rig.up();
          if (rig.session.limits.onPaste) {
            expect(
              rig.session.dialHeaders,
              contains('X-Multitap-Clipboard-OnPaste'),
            );
          } else {
            await _filesBothWays(rig, {'fallback.bin': 70000});
          }
          expect(rig.session.ready, isTrue);
          expect(rig.relay.closures, isEmpty);
        } finally {
          await rig.down();
        }
      }, timeout: const Timeout(Duration(minutes: 2)));
    }
    for (final toVm in [true, false]) {
      final name =
          '${server.key}/onpaste/text permission revoked during ${toVm ? 'PC to VM' : 'VM to PC'} render';
      if (only.isNotEmpty && !name.contains(only)) continue;
      test(name, () async {
        final rig = Rig(
          server.key,
          server.value,
          true,
          onPaste: true,
          delayMs: 20,
          policy: const ClipboardPolicy(textToRemote: true, textToLocal: true),
        );
        try {
          await rig.up();
          if (!rig.session.limits.onPaste) {
            markTestSkipped('On-paste revision required.');
            return;
          }
          final text = _longText(3 * mib);
          await rig.peer(toVm).request({'cmd': 'setText', 'text': text});
          await rig.expectArrived(
            toVm: toVm,
            what: 'revocable text offer',
            check: (b) => b['kind'] == 'offer' && b['offerFormat'] == 'text',
          );
          final before = _chunks(rig);
          final render = rig
              .peer(!toVm)
              .request({'cmd': 'renderText'})
              .then<Object?>(
                (value) => value,
                onError: (Object error) => error,
              );
          await eventually(
            () => _chunks(rig) > before,
            'the slow text render to transfer bytes',
          );
          await rig.relay.setPolicy({
            'textToRemote': false,
            'textToLocal': false,
            'filesToRemote': false,
            'filesToLocal': false,
          });
          expect(await render, anyOf(isA<Exception>(), isA<Error>()));
          await eventually(
            () => !rig.session.ready,
            'the relay to revoke the clipboard pair',
          );
          expect(rig.relay.closures, isNotEmpty);
        } finally {
          await rig.down();
        }
      }, timeout: const Timeout(Duration(minutes: 2)));
    }
    for (final textAllowed in [true, false]) {
      final name =
          '${server.key}/onpaste/text permission without file permission: ${textAllowed ? 'allowed' : 'denied'}';
      if (only.isNotEmpty && !name.contains(only)) continue;
      test(name, () async {
        final rig = Rig(
          server.key,
          server.value,
          true,
          onPaste: true,
          policy: ClipboardPolicy(
            textToRemote: textAllowed,
            textToLocal: textAllowed,
            filesToRemote: !textAllowed,
            filesToLocal: !textAllowed,
          ),
        );
        try {
          await rig.up();
          if (!rig.session.limits.onPaste) {
            markTestSkipped('On-paste revision required.');
            return;
          }
          final text = _longText(2 * mib + 19);
          for (final rich in [false, true]) {
            for (final toVm in [true, false]) {
              final before = _chunks(rig);
              final sequence = (await rig.board(!toVm))['sequence'];
              await rig.peer(toVm).request({
                'cmd': rich ? 'setRich' : 'setText',
                'text': text,
                if (rich) 'html': cfHtml('<b>$text</b>'),
              });
              if (textAllowed) {
                await rig.expectArrived(
                  toVm: toVm,
                  what: 'text-only offer',
                  check: (b) =>
                      b['kind'] == 'offer' &&
                      b['offerFormat'] == (rich ? 'richtext' : 'text'),
                );
                expect(_chunks(rig), before);
                final rendered =
                    await rig.peer(!toVm).request({'cmd': 'renderText'}) as Map;
                expect(
                  rendered['textSha'],
                  sha256.convert(utf8.encode(text)).toString(),
                );
                if (rich) {
                  final html = await rig.peer(!toVm).request({
                    'cmd': 'renderHtml',
                  }) as Map;
                  expect(html['html'], contains('<b>$text</b>'));
                }
              } else {
                await Future<void>.delayed(const Duration(milliseconds: 700));
                if (toVm) {
                  await expectLater(rig.session.sendLocal(), throwsA(anything));
                } else {
                  await expectLater(
                    rig.session.receiveRemote(),
                    throwsA(anything),
                  );
                }
                expect((await rig.board(!toVm))['sequence'], sequence);
                expect(
                  _chunks(rig),
                  before,
                  reason: 'file permission must not grant text access',
                );
              }
              expect(rig.session.ready, isTrue);
            }
          }
          expect(rig.relay.closures, isEmpty);
        } finally {
          await rig.down();
        }
      }, timeout: const Timeout(Duration(minutes: 3)));
    }
    // File permission and a large copy (announced without its list on a deferred pair): without the permission nothing is announced
    // and text still flows; the permission revoked while the list is made ends that paste (the relay checks every list request and
    // every message of one against the live policy, as for files being pulled).
    for (final toVm in [true, false]) {
      final name = '${server.key}/onpaste/file permission off: a large copy is not announced, ${toVm ? 'PC to VM' : 'VM to PC'}';
      if (only.isNotEmpty && !name.contains(only)) continue;
      test(name, () async {
        final rig = Rig(server.key, server.value, true, onPaste: true,
            policy: const ClipboardPolicy(textToRemote: true, textToLocal: true));
        try {
          await rig.up();
          if (!rig.session.limits.onPaste) {
            markTestSkipped('On-paste revision required.');
            return;
          }
          final paths = await _bigFolder(rig, 'Denied', 400);
          final sequence = (await rig.board(!toVm))['sequence'];
          final lists = _listMessages(rig), asked = _listRequests(rig);
          await rig.peer(toVm).request({'cmd': 'setFiles', 'paths': paths});
          await Future<void>.delayed(const Duration(milliseconds: 1500));
          expect((await rig.board(!toVm))['sequence'], sequence, reason: 'a copy of files must not be announced without the permission');
          expect(_listMessages(rig), lists);
          expect(_listRequests(rig), asked);
          final text = 'text still flows ${toVm ? 'to the VM' : 'to the PC'}';
          await rig.peer(toVm).request({'cmd': 'setText', 'text': text});
          await rig.expectArrived(toVm: toVm, what: 'text', check: (b) => b['kind'] == 'text' && b['text'] == text);
          expect(rig.session.ready, isTrue);
          expect(rig.relay.closures, isEmpty);
        } finally {
          await rig.down();
        }
      }, timeout: const Timeout(Duration(minutes: 3)));
    }
    for (final toVm in [true, false]) {
      final name = '${server.key}/onpaste/file permission revoked while a large list is made, ${toVm ? 'PC to VM' : 'VM to PC'}';
      if (only.isNotEmpty && !name.contains(only)) continue;
      test(name, () async {
        final rig = Rig(server.key, server.value, true, onPaste: true);
        try {
          await rig.up();
          if (!rig.session.deferredManifest) {
            markTestSkipped('Deferred manifest (manifest-on-paste-v1) required.');
            return;
          }
          final huge = await _hugeFixture(rig);
          final folders = (Directory(huge).listSync().whereType<Directory>().map((d) => d.path).toList()..sort()).skip(120).take(60).toList();
          // The copying side announces copies once it watches its clipboard (the Host about 100 ms after the PC's clipboard.watch on);
          // a copy made before is the clipboard the watch starts from and is not announced. A text that arrives shows the watch is on
          // (a readiness probe of this rig, not a retry of what the scenario checks): a new text every 2 s, ten at most.
          var watching = false;
          for (var attempt = 1; attempt <= 10 && !watching; ++attempt) {
            final ready = 'watching ${toVm ? 'on the PC' : 'in the VM'} $attempt';
            await rig.peer(toVm).request({'cmd': 'setText', 'text': ready});
            try {
              await eventually(() async {
                final board = await rig.board(!toVm);
                return board['kind'] == 'text' && board['text'] == ready;
              }, 'a first text', timeout: const Duration(seconds: 2));
              watching = true;
            } catch (_) {}
          }
          expect(watching, isTrue, reason: 'the copying side never watched its clipboard: ${_hostTrace(rig)}');
          try {
            await _onPasteOffer(rig, toVm, folders);
          } catch (error) {
            throw StateError('$error; ${_hostTrace(rig)}');
          }
          final asked = _listRequests(rig);
          final destination = rig.scratch('revoked-walk');
          final paste = rig
              .peer(!toVm)
              .request({'cmd': 'paste', 'dest': destination.path}, timeout: const Duration(minutes: 5))
              .then<Object?>((value) => value, onError: (Object error) => error);
          await eventually(() => _listRequests(rig) > asked, 'the paste to ask for the list');
          await rig.relay.setPolicy({'textToRemote': true, 'textToLocal': true, 'filesToRemote': false, 'filesToLocal': false});
          expect(await paste, anyOf(isA<Exception>(), isA<Error>()), reason: 'the paste ends once files are no longer allowed: ${_hostTrace(rig)}');
          try {
            // (A policy change ends the pair at once as clipboard_policy_changed; a message checked against it first, as denied.)
            await eventually(
              () => rig.relay.closures.any((reason) => reason == 'clipboard_policy_changed' || reason == 'clipboard_policy_denied'),
              'the relay to end the pair for the policy',
            );
          } catch (error) {
            throw StateError('$error; ${_hostTrace(rig)}');
          }
          expect(destination.listSync(recursive: true).whereType<File>(), isEmpty, reason: 'no file of the revoked copy');
        } finally {
          await rig.down();
        }
      }, timeout: const Timeout(Duration(minutes: 5)));
    }
    for (final sync in [false, true]) {
      for (final scenario in scenarios.entries) {
        final name =
            '${server.key}/${sync ? 'sync' : 'manual'}/${scenario.key}';
        if (only.isNotEmpty && !name.contains(only)) continue;
        test(name, () async {
          final rig = Rig(server.key, server.value, sync);
          try {
            await rig.up();
            await scenario.value(rig);
            expect(
              rig.relay.closures,
              isEmpty,
              reason: 'the relay ended the clipboard pair',
            );
          } finally {
            await rig.down();
          }
        }, timeout: const Timeout(Duration(minutes: 4)));
      }

      if (only.isEmpty ||
          '${server.key}/${sync ? 'sync' : 'manual'}/claim'.contains(only)) {
        test(
          '${server.key}/${sync ? 'sync' : 'manual'}/claim while a copy arrives',
          () async {
            final rig = Rig(
              server.key,
              server.value,
              sync,
              claimAfter: const Duration(milliseconds: 40),
            );
            try {
              await rig.up();
              await _claimScenario(rig);
            } finally {
              await rig.down();
            }
          },
          timeout: const Timeout(Duration(minutes: 4)),
        );
      }

      for (final scenario in policyScenarios.entries) {
        final name =
            '${server.key}/${sync ? 'sync' : 'manual'}/policy: ${scenario.key}';
        if (only.isNotEmpty && !name.contains(only)) continue;
        test(name, () async {
          final rig = Rig(
            server.key,
            server.value,
            sync,
            policy: scenario.value.policy,
          );
          try {
            await rig.up();
            await scenario.value.run(rig);
            expect(
              rig.relay.closures,
              isEmpty,
              reason: 'the relay ended the clipboard pair',
            );
          } finally {
            await rig.down();
          }
        }, timeout: const Timeout(Duration(minutes: 4)));
      }
    }
  }
}
