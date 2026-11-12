import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/login_memory.dart';

import 'login_memory_test.dart' show TestVault;

class HeldVault extends TestVault {
  Completer<void>? loadGate, deleteGate;
  final loadStarted = Completer<void>(), deleteStarted = Completer<void>();
  int loads = 0;
  @override
  Future<RememberedLogin?> load(String origin) async {
    loads++;
    final gate = loadGate;
    if (gate != null) {
      loadGate = null;
      if (!loadStarted.isCompleted) loadStarted.complete();
      await gate.future;
    }
    return super.load(origin);
  }

  @override
  Future<void> delete(String origin) async {
    final gate = deleteGate;
    if (gate != null) {
      deleteGate = null;
      if (!deleteStarted.isCompleted) deleteStarted.complete();
      await gate.future;
    }
    await super.delete(origin);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final origin = Uri.parse('https://st15-memory.fixture.invalid');
  const original = RememberedLogin(username: 'alice', password: 'fixture-old');
  late Directory directory;
  late HeldVault vault;
  late WindowsLoginMemory memory;
  setUp(() async {
    directory = await Directory.systemTemp.createTemp('multitap-st15-memory-');
    vault = HeldVault();
    memory = WindowsLoginMemory(
      File('${directory.path}/login.json'),
      vault: vault,
    );
    await memory.savePassword(origin, original.username, original.password!);
    vault.deleted.clear();
  });
  tearDown(() async {
    final full = directory.absolute.path.replaceAll('\\', '/'),
        temp = Directory.systemTemp.absolute.path.replaceAll('\\', '/');
    if (!full.startsWith('$temp/multitap-st15-memory-')) {
      throw StateError('owned temporary fixture cleanup mismatch');
    }
    await directory.delete(recursive: true);
  });
  test(
    'Current exact expected credential is erased and remembered ID stays',
    () async {
      expect(
        await memory.forgetPasswordIfMatches(
          origin,
          original,
          current: () => true,
        ),
        isTrue,
      );
      final now = await memory.load(origin);
      expect(now.username, 'alice');
      expect(now.password, isNull);
      expect(vault.deleted, [origin.origin]);
    },
  );
  for (final expected in [
    const RememberedLogin(username: 'ALICE', password: 'fixture-old'),
    const RememberedLogin(username: 'alice', password: 'changed'),
    const RememberedLogin(username: 'alice'),
  ]) {
    test(
      'Different exact credential ${expected.username}/${expected.password == null ? 'absent' : 'present'} never erases',
      () async {
        expect(
          await memory.forgetPasswordIfMatches(
            origin,
            expected,
            current: () => true,
          ),
          isFalse,
        );
        expect((await memory.load(origin)).password, 'fixture-old');
        expect(vault.deleted, isEmpty);
      },
    );
  }
  test(
    'Different origin cannot erase the matching username/password elsewhere',
    () async {
      expect(
        await memory.forgetPasswordIfMatches(
          Uri.parse('https://other.fixture.invalid'),
          original,
          current: () => true,
        ),
        isFalse,
      );
      expect((await memory.load(origin)).password, 'fixture-old');
      expect(vault.deleted, isEmpty);
    },
  );
  test('Already retired callback refuses before vault read', () async {
    final loads = vault.loads;
    expect(
      await memory.forgetPasswordIfMatches(
        origin,
        original,
        current: () => false,
      ),
      isFalse,
    );
    expect(vault.loads, loads);
    expect(vault.deleted, isEmpty);
  });
  test('Owner retired during actual vault await prevents deletion', () async {
    var current = true;
    final gate = Completer<void>();
    vault.loadGate = gate;
    final erase = memory.forgetPasswordIfMatches(
      origin,
      original,
      current: () => current,
    );
    await vault.loadStarted.future.timeout(const Duration(seconds: 3));
    current = false;
    gate.complete();
    expect(await erase, isFalse);
    expect(vault.deleted, isEmpty);
    expect((await memory.load(origin)).password, 'fixture-old');
  });
  test('Real ordered read then newer queued save then conditional erase preserves new password', () async {
    final read = memory.load(origin);
    final newer = memory.savePassword(origin, 'alice', 'fixture-new');
    final cleanup = read.then(
      (snapshot) =>
          memory.forgetPasswordIfMatches(origin, snapshot, current: () => true),
    );
    expect((await read).password, 'fixture-old');
    await newer;
    expect(await cleanup, isFalse);
    expect((await memory.load(origin)).password, 'fixture-new');
    expect(vault.deleted, isEmpty);
  });
  test(
    'Save queued after compare/erase operation completes after old erasure',
    () async {
      final gate = Completer<void>();
      vault.loadGate = gate;
      final erase = memory.forgetPasswordIfMatches(
        origin,
        original,
        current: () => true,
      );
      await vault.loadStarted.future.timeout(const Duration(seconds: 3));
      final newer = memory.savePassword(origin, 'alice', 'fixture-new');
      gate.complete();
      expect(await erase, isTrue);
      await newer;
      expect((await memory.load(origin)).password, 'fixture-new');
    },
  );
  test('Already issued erasure cannot overtake successor queued save after owner changes', () async {
    var current = true;
    final gate = Completer<void>();
    vault.deleteGate = gate;
    final erase = memory.forgetPasswordIfMatches(
      origin,
      original,
      current: () => current,
    );
    await vault.deleteStarted.future.timeout(const Duration(seconds: 3));
    current = false;
    final newer = memory.savePassword(origin, 'bobby', 'fixture-new');
    gate.complete();
    expect(await erase, isTrue);
    await newer;
    final now = await memory.load(origin);
    expect(now.username, 'bobby');
    expect(now.password, 'fixture-new');
  });
  test(
    'Callback failure does not erase and queue still accepts a later save',
    () async {
      await expectLater(
        memory.forgetPasswordIfMatches(
          origin,
          original,
          current: () => throw StateError('fixture retired'),
        ),
        throwsStateError,
      );
      expect(vault.deleted, isEmpty);
      await memory.savePassword(origin, 'alice', 'fixture-new');
      expect((await memory.load(origin)).password, 'fixture-new');
    },
  );
}
