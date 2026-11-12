import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/models/stream_options.dart';

const signinSessionId = 'a015f587-2af5-4e8b-a5c9-90fd48b4bf52';
Map<String, dynamic> signinConnectionJson() => {
  'sessionId': signinSessionId,
  'webSocketUrl': 'wss://cloud.example.com/api/v1/sessions/$signinSessionId',
  'mode': 'stream',
  'transport': 'media-epoch-v1',
  'resizeProtocol': 'epoch-v1',
};
Map<String, dynamic> signinBindingJson() => {
  'protocol': 'signin-stream-v1',
  'phase': 'signin',
  'workerGeneration': 1,
  'transitionId': '1' * 32,
  'mediaScope': {'kind': 'composite', 'epoch': 1},
};
Map<String, dynamic> preparePhase({
  int generation = 2,
  int epoch = 2,
  String phase = 'desktop',
  String? transition,
}) => {
  'type': 'stream.phase.prepare',
  ...signinBindingJson(),
  'sessionId': signinSessionId,
  'workerGeneration': generation,
  'phase': phase,
  'transitionId': transition ?? '2' * 32,
  'mediaScope': {'kind': 'composite', 'epoch': epoch},
  'timeoutMs': 15000,
};
Map<String, dynamic> stateInfo(SigninBinding binding, {String? frameId}) => {
  'type': 'stream.phase.info',
  ...binding.toJson(),
  'sessionId': signinSessionId,
  'firstFrameId': frameId ?? 'a' * 32,
  'info': {
    'type': 'stream.info',
    'sessionId': signinSessionId,
    'epoch': binding.mediaScope.epoch,
    'codec': 'h264',
    'width': 1280,
    'height': 720,
  },
};
SigninStreamState activeState() {
  final state = SigninStreamState(
    sessionId: signinSessionId,
    initial: SigninBinding.parse(signinBindingJson()),
  );
  state.acceptInfo(stateInfo(state.binding), 0);
  state.acceptedIdr(1);
  expect(state.decoded(), isNotNull);
  return state;
}

void main() {
  test(
    'Unrequested sign-in metadata never silently grants a privileged phase',
    () {
      expect(
        () => BrokerConnection.fromJson({
          ...signinConnectionJson(),
          'signin': signinBindingJson(),
        }, Uri.parse('https://cloud.example.com')),
        throwsA(isA<BrokerException>()),
      );
    },
  );
  test('Ordinary epoch stream remains a valid old-peer connection', () {
    final value = BrokerConnection.fromJson(
      signinConnectionJson(),
      Uri.parse('https://cloud.example.com'),
    );
    expect(value.sessionId, signinSessionId);
    expect(value.epochMedia, isTrue);
  });
  test('Only explicitly requested single composite grants are admitted', () {
    final value = BrokerConnection.fromJson(
      {...signinConnectionJson(), 'signin': signinBindingJson()},
      Uri.parse('https://cloud.example.com'),
      requestedSignin: true,
    );
    expect(value.signin!.mediaScope.epoch, 1);
    for (final patch in [
      {'signin': null},
      {'transport': 'media-v1'},
      {
        'signin': {...signinBindingJson(), 'protocol': 'future'},
      },
      {
        'signin': {
          ...signinBindingJson(),
          'mediaScope': {'kind': 'tracks', 'topologyEpoch': 1, 'tracks': []},
        },
      },
      {'udpDirectProtocol': 'udp-direct-v1'},
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          {...signinConnectionJson(), 'signin': signinBindingJson(), ...patch},
          Uri.parse('https://cloud.example.com'),
          requestedSignin: true,
        ),
        throwsA(isA<BrokerException>()),
      );
    }
  });
  test(
    'Binding rejects unknown fields, invalid identities and integer truncation',
    () {
      for (final patch in [
        {'workerGeneration': 0},
        {'workerGeneration': 4294967296},
        {'workerGeneration': 1.5},
        {'phase': 'system'},
        {'transitionId': 'A' * 32},
        {'transitionId': '1' * 31},
        {'extra': true},
        {
          'mediaScope': {'kind': 'composite', 'epoch': 0},
        },
        {
          'mediaScope': {'kind': 'composite', 'epoch': 65536},
        },
        {
          'mediaScope': {'kind': 'composite', 'epoch': 1.5},
        },
      ]) {
        expect(
          () => SigninBinding.parse({...signinBindingJson(), ...patch}),
          throwsFormatException,
        );
      }
    },
  );
  test('Metadata, wrong epoch and echoed frame ID do not establish decoder readiness', () {
    final state = SigninStreamState(
      sessionId: signinSessionId,
      initial: SigninBinding.parse(signinBindingJson()),
    );
    expect(state.inputOwner, isNull);
    expect(state.redirectsAllowed, isFalse);
    state.acceptInfo(stateInfo(state.binding), 0);
    expect(state.decoded(), isNull);
    state.acceptedIdr(2);
    expect(state.decoded(), isNull);
    state.acceptedIdr(1);
    final ready = state.decoded()!;
    expect(ready['firstFrameId'], 'a' * 32);
    expect(state.decoded(), isNull);
    expect(state.inputOwner, isNotNull);
    expect(state.redirectsAllowed, isFalse);
  });
  test('A handoff retires the observed input owner before any new info', () {
    final state = activeState(), owner = stateSafeOwner(activeState());
    expect(state.wrapInput({'type': 'input.reset'}, owner), isNull);
    final old = state.inputOwner!;
    expect(state.wrapInput({'type': 'input.reset'}, old), isNotNull);
    expect(state.prepare(preparePhase(), 100), isTrue);
    expect(state.wrapInput({'type': 'input.reset'}, old), isNull);
    expect(state.inputOwner, isNull);
    expect(state.redirectsAllowed, isFalse);
    state.acceptInfo(stateInfo(state.binding), 101);
    state.acceptedIdr(2);
    expect(state.decoded(), isNotNull);
    expect(state.redirectsAllowed, isTrue);
    expect(state.wrapInput({'type': 'input.reset'}, old), isNull);
  });
  test(
    'Duplicates do not renew deadlines, restart phases or issue two ACKs',
    () {
      final state = activeState();
      state.prepare(preparePhase(), 100);
      final deadline = state.preparationDeadlineUs;
      expect(state.prepare(preparePhase(), 500), isFalse);
      expect(state.preparationDeadlineUs, deadline);
      final message = stateInfo(state.binding);
      expect(state.acceptInfo(message, 501), isNotNull);
      expect(state.acceptInfo(message, 502), isNull);
      state.acceptedIdr(2);
      expect(state.decoded(), isNotNull);
      expect(state.prepare(preparePhase(), 503), isFalse);
      expect(state.decoded(), isNull);
    },
  );
  test('Phase correlation, generation, scope and deadline remain exact', () {
    for (final patch in [
      {'sessionId': 'foreign'},
      {'workerGeneration': 3},
      {
        'mediaScope': {'kind': 'composite', 'epoch': 3},
      },
      {'phase': 'signin'},
      {'transitionId': '1' * 32},
      {'timeoutMs': 90001},
    ]) {
      expect(
        () => activeState().prepare({...preparePhase(), ...patch}, 0),
        throwsFormatException,
      );
    }
    final late = activeState();
    late.prepare(preparePhase(), 0);
    expect(
      () => late.acceptInfo(stateInfo(late.binding), 15000001),
      throwsFormatException,
    );
  });
  test(
    'Desktop resize requires a fresh decoded ACK without a worker handoff',
    () {
      final state = activeState();
      state.prepare(preparePhase(), 0);
      state.acceptInfo(stateInfo(state.binding), 1);
      state.acceptedIdr(2);
      state.decoded();
      final old = state.inputOwner!, redirect = state.redirectOwner;
      final resized = stateInfo(state.binding.withEpoch(3), frameId: 'b' * 32);
      final frame = state.acceptInfo(resized, 10)!;
      expect(frame.resize, isTrue);
      expect(state.binding.workerGeneration, 2);
      expect(state.active, isFalse);
      expect(state.redirectsAllowed, isFalse);
      expect(state.redirectOwner, isNull);
      expect(state.retainsRedirects(redirect), isTrue);
      expect(state.retainsRedirects(activeState().redirectOwner), isFalse);
      expect(state.wrapInput({'type': 'input.reset'}, old), isNull);
      final deadline = state.preparationDeadlineUs;
      expect(state.acceptInfo(resized, 100), isNull);
      expect(state.preparationDeadlineUs, deadline);
      expect(state.decoded(), isNull);
      state.acceptedIdr(2);
      expect(state.decoded(), isNull);
      state.acceptedIdr(3);
      expect(state.decoded()!['mediaScope'], {'kind': 'composite', 'epoch': 3});
      expect(state.retainsRedirects(redirect), isTrue);
      expect(state.acceptInfo(resized, 200), isNull);
      expect(
        state.prepare(
          preparePhase(
            generation: 3,
            epoch: 4,
            phase: 'signin',
            transition: '3' * 32,
          ),
          201,
        ),
        isTrue,
      );
      expect(state.retainsRedirects(redirect), isFalse);
    },
  );
  test(
    'Implicit resize rejects sign-in, scope jumps and foreign desktop owners',
    () {
      final signin = activeState();
      expect(
        () => signin.acceptInfo(stateInfo(signin.binding.withEpoch(2)), 1),
        throwsFormatException,
      );
      for (final patch in [
        {
          'mediaScope': {'kind': 'composite', 'epoch': 4},
        },
        {'workerGeneration': 3},
        {'transitionId': '3' * 32},
        {'phase': 'signin'},
        {'sessionId': 'b015f587-2af5-4e8b-a5c9-90fd48b4bf52'},
      ]) {
        final state = activeState();
        state.prepare(preparePhase(), 0);
        state.acceptInfo(stateInfo(state.binding), 1);
        state.acceptedIdr(2);
        state.decoded();
        expect(
          () => state.acceptInfo({
            ...stateInfo(state.binding.withEpoch(3)),
            ...patch,
          }, 10),
          throwsFormatException,
        );
        expect(state.active, isTrue);
        expect(state.binding.mediaScope.epoch, 2);
      }
    },
  );
  test(
    'Epoch and worker generation exhaustion never wraps or grants input',
    () {
      for (final binding in [
        {...signinBindingJson(), 'workerGeneration': 4294967295},
        {
          ...signinBindingJson(),
          'mediaScope': {'kind': 'composite', 'epoch': 65535},
        },
      ]) {
        final state = SigninStreamState(
          sessionId: signinSessionId,
          initial: SigninBinding.parse(binding),
        );
        state.acceptInfo(stateInfo(state.binding), 0);
        state.acceptedIdr(state.binding.mediaScope.epoch);
        state.decoded();
        expect(() => state.prepare(preparePhase(), 0), throwsFormatException);
      }
    },
  );
  test('Closed ownership cannot wrap input or revive a decoded phase', () {
    final state = activeState();
    final own = state.inputOwner;
    state.close();
    expect(state.wrapInput({'type': 'input.reset'}, own), isNull);
    expect(state.decoded(), isNull);
    expect(state.inputOwner, isNull);
  });
  for (final result in ['signin', 'refused', 'missing', 'ordinary']) {
    test(
      'Production HTTP optional sign-in path: $result is one-shot and explicit',
      () async {
        BrokerTransport.clear();
        final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
        final origin = Uri.parse('http://127.0.0.1:${server.port}');
        final approval = BrokerTransport.approve(origin, origin.origin);
        final bodies = <Map<String, dynamic>>[];
        server.listen((request) async {
          request.response.headers.contentType = ContentType.json;
          if (request.uri.path == '/healthz') {
            request.response.write(
              jsonEncode({
                'service': 'multitap-broker',
                'status': 'ok',
                'authentication': 'id-password',
                'media': 'relay-v1',
                'version': '9.9.9',
                'transport': {
                  'protocol': 'broker-transport-v1',
                  'publicOrigin': origin.origin,
                  'scheme': 'http',
                  'allowInsecureHttp': true,
                  'directTls': false,
                },
              }),
            );
          } else {
            final body = jsonDecode(
              await utf8.decoder.bind(request).join(),
            ) as Map<String, dynamic>;
            bodies.add(body);
            if (result == 'refused') {
              request.response.statusCode = 400;
              request.response.write(
                jsonEncode({
                  'error': {'code': 'unexpected_field'},
                }),
              );
            } else {
              request.response.write(
                jsonEncode({
                  ...signinConnectionJson(),
                  'webSocketUrl':
                      'ws://127.0.0.1:${server.port}/api/v1/sessions/$signinSessionId',
                  'options': body['options'],
                  if (result == 'signin') 'signin': signinBindingJson(),
                }),
              );
            }
          }
          await request.response.close();
        });
        final api = HttpBrokerApi(origin: origin);
        try {
          if (result == 'ordinary') {
            final grant = await api.connect(
              'synthetic-token',
              'assigned-fixture',
            );
            expect(grant.signin, isNull);
          } else if (result == 'signin') {
            final grant = await (api as SigninStreamBrokerApi).connectSignin(
              'synthetic-token',
              'assigned-fixture',
              availabilityId: 'a' * 32,
            );
            expect(grant.signin!.phase, SigninPhase.signin);
          } else {
            await expectLater(
              api.connectSignin(
                'synthetic-token',
                'assigned-fixture',
                availabilityId: 'a' * 32,
              ),
              throwsA(isA<BrokerException>()),
            );
          }
          expect(bodies, hasLength(1));
          expect(
            bodies.single.containsKey('signinProtocol'),
            result != 'ordinary',
          );
          if (result != 'ordinary') {
            expect(bodies.single['signinProtocol'], signinStreamProtocol);
            expect(bodies.single['signinAvailabilityId'], 'a' * 32);
            expect(bodies.single.containsKey('udpDirectProtocol'), isFalse);
            expect(bodies.single.containsKey('sessionResumeProtocol'), isFalse);
            expect(bodies.single.containsKey('relativeMouseProtocol'), isFalse);
          }
        } finally {
          api.close();
          approval.release();
          await server.close(force: true);
          BrokerTransport.clear();
        }
      },
    );
  }
  test('Unsupported sign-in output and bad availability fail before an HTTP request', () async {
    final api = HttpBrokerApi(
      origin: Uri.parse('https://unused.fixture.invalid'),
    );
    try {
      await expectLater(
        api.connectSignin('synthetic', 'assigned', availabilityId: 'bad'),
        throwsArgumentError,
      );
      await expectLater(
        api.connectSignin(
          'synthetic',
          'assigned',
          availabilityId: 'a' * 32,
          options: const StreamOptions(monitor: 'all'),
        ),
        throwsArgumentError,
      );
    } finally {
      api.close();
    }
  });
}

SigninInputOwner? stateSafeOwner(SigninStreamState state) => state.inputOwner;
