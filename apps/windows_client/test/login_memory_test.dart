import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/login_memory.dart';
import 'package:multitap_client/data/broker_transport.dart';

class TestVault implements CredentialVault {
  final entries = <String, RememberedLogin>{};
  bool failWrites = false;
  final deleted = <String>[];

  @override
  Future<RememberedLogin?> load(String origin) async => entries[origin];

  @override
  Future<void> save(String origin, String username, String password) async {
    if (failWrites) throw StateError('Test vault unavailable');
    entries[origin] = RememberedLogin(username: username, password: password);
  }

  @override
  Future<void> delete(String origin) async {
    deleted.add(origin);
    entries.remove(origin);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final origin = Uri.parse('https://vdi.example.test');
  final another = Uri.parse('https://other.example.test:8443');
  late Directory directory;
  late File metadata;
  late TestVault vault;
  late WindowsLoginMemory memory;

  setUp(() async {
    BrokerTransport.clear();
    directory = await Directory.systemTemp.createTemp('multitap-login-test-');
    metadata = File('${directory.path}/login.json');
    vault = TestVault();
    memory = WindowsLoginMemory(metadata, vault: vault);
  });

  tearDown(() async {
    BrokerTransport.clear();
    await directory.delete(recursive: true);
  });

  test('the removed logout option\'s leftover file is deleted, quietly', () async {
    await memory.saveUsername(origin, '테스트아이디');
    final leftover = File('${directory.path}/login-privacy.json');
    await memory.deleteRetiredLogoutOption();
    expect(await leftover.exists(), isFalse, reason: 'missing is not an error');
    await leftover.writeAsString('{"version":1,"retypeAfterLogout":true}');
    await memory.deleteRetiredLogoutOption();
    expect(await leftover.exists(), isFalse);
    expect(await metadata.exists(), isTrue);
    expect((await memory.load(origin)).username, '테스트아이디');
  });

  test('Korean login IDs reload per server without a password file', () async {
    await memory.saveUsername(origin, '테스트아이디');
    await memory.saveUsername(another, '다른아이디');
    final reopened = WindowsLoginMemory(metadata, vault: vault);
    expect((await reopened.load(origin)).username, '테스트아이디');
    expect((await reopened.load(another)).username, '다른아이디');
    expect((await reopened.load(origin)).password, isNull);
    final data = jsonDecode(await metadata.readAsString()) as Map;
    expect(data.keys, unorderedEquals(['version', 'usernames']));
    expect(data['usernames'], {
      origin.origin: '테스트아이디',
      another.origin: '다른아이디',
    });
  });

  test(
    'Remembered passwords stay in vault and cannot cross servers or IDs',
    () async {
      const password = 'A-test-only-secret!';
      await memory.savePassword(origin, '원래아이디', password);
      await memory.saveUsername(another, '다른아이디');
      final reopened = WindowsLoginMemory(metadata, vault: vault);
      expect((await reopened.load(origin)).password, password);
      expect((await reopened.load(another)).password, isNull);
      expect(await metadata.readAsString(), isNot(contains(password)));

      // Changing the ID removes the old account's credential from this origin.
      await reopened.saveUsername(origin, '새아이디');
      expect(vault.entries.containsKey(origin.origin), isFalse);
      expect((await reopened.load(origin)).username, '새아이디');
      expect((await reopened.load(origin)).password, isNull);
      // A mismatched native entry must never fill the current ID's password.
      vault.entries[origin.origin] = const RememberedLogin(
        username: '원래아이디',
        password: password,
      );
      expect((await reopened.load(origin)).password, isNull);
    },
  );

  test(
    'Opt out removes only this server password and retains automatic ID',
    () async {
      await memory.savePassword(origin, '아이디', 'Test-only-password!');
      await memory.savePassword(another, '별도아이디', 'Another-test-password!');
      await memory.forgetPassword(origin);
      final current = await memory.load(origin);
      expect(current.username, '아이디');
      expect(current.password, isNull);
      expect((await memory.load(another)).password, 'Another-test-password!');
    },
  );

  test('Vault write failure never falls back to plaintext settings', () async {
    vault.failWrites = true;
    const password = 'Do-not-write-this-to-disk!';
    await expectLater(
      memory.savePassword(origin, '아이디', password),
      throwsStateError,
    );
    expect(await metadata.readAsString(), isNot(contains(password)));
    expect((await memory.load(origin)).password, isNull);
    // The queue recovers so ID memory can still change after the failure.
    await memory.saveUsername(origin, '바뀐아이디');
    expect((await memory.load(origin)).username, '바뀐아이디');
  });

  test(
    'ID edits and opt-out remain ordered when called without awaiting',
    () async {
      final save = memory.savePassword(origin, '아이디', 'Test-only-password!');
      final change = memory.saveUsername(origin, '새아이디');
      final optOut = memory.forgetPassword(origin);
      await Future.wait([save, change, optOut]);
      final current = await memory.load(origin);
      expect(current.username, '새아이디');
      expect(current.password, isNull);
    },
  );

  test(
    'Unapproved HTTP and secret-bearing origins cannot reach the vault',
    () async {
      for (final address in [
        'http://vdi.example.test',
        'https://user:secret@vdi.example.test',
        'https://vdi.example.test/path',
      ]) {
        await expectLater(
          memory.savePassword(Uri.parse(address), '아이디', 'Test-password!'),
          throwsFormatException,
        );
      }
      expect(vault.entries, isEmpty);
      expect(await metadata.exists(), isFalse);
    },
  );

  test('HTTP credential access uses exact consent while erasure survives withdrawal', () async {
    final http = Uri.parse('http://vdi.example.test:18480');
    final approval = BrokerTransport.approve(http, http.origin);
    // A password is never remembered for an HTTP server, even an approved
    // one: one stored by an older version is never read back, and saving
    // keeps the ID and deletes it.
    await memory.saveUsername(http, 'fixture-user');
    vault.entries[http.origin] = const RememberedLogin(
      username: 'fixture-user',
      password: 'Older-password!',
    );
    final before = await memory.load(http);
    expect(before.username, 'fixture-user');
    expect(before.password, isNull);
    await memory.savePassword(http, 'fixture-user', 'Fixture-password!');
    expect(vault.entries, isEmpty);
    expect(vault.deleted.last, http.origin);
    final after = await memory.load(http);
    expect(after.username, 'fixture-user');
    expect(after.password, isNull);
    await expectLater(
      memory.savePassword(
        Uri.parse('http://vdi.example.test:18481'),
        'other',
        'Fixture-password!',
      ),
      throwsFormatException,
    );
    BrokerTransport.clear();
    approval.release();
    await expectLater(memory.load(http), throwsFormatException);
    await expectLater(
      memory.saveUsername(http, 'changed'),
      throwsFormatException,
    );
    await expectLater(
      memory.savePassword(http, 'changed', 'Other-password!'),
      throwsFormatException,
    );
    await memory.forgetPassword(http);
    expect(vault.entries, isEmpty);
    expect(vault.deleted.last, http.origin);
  });

  test(
    'Method channel sends only scoped credentials and handles absent entry',
    () async {
      const channel = MethodChannel('multitap/credentials');
      final calls = <MethodCall>[];
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            calls.add(call);
            if (call.method == 'load') return null;
            return null;
          });
      addTearDown(() {
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(channel, null);
      });
      const native = WindowsCredentialVault();
      expect(await native.load(origin.origin), isNull);
      await native.save(origin.origin, '아이디', 'Test-only-password!');
      await native.delete(origin.origin);
      expect(calls.map((call) => call.method), ['load', 'save', 'delete']);
      expect(calls[0].arguments, {'origin': origin.origin});
      expect(calls[1].arguments, {
        'origin': origin.origin,
        'username': '아이디',
        'password': 'Test-only-password!',
      });
    },
  );
}
