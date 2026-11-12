import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'client_settings.dart';
import 'broker_transport.dart';
import '../l10n/l10n.dart';

class BrokerProbeResult {
  const BrokerProbeResult({
    required this.version,
    required this.elapsedMs,
    this.httpApproval,
  });
  final String version;
  final int elapsedMs;
  final HttpTransportApproval? httpApproval;
}

class BrokerProbeException implements Exception {
  const BrokerProbeException(this.code, {this.status});
  final String code;
  final int? status;
  String get message => switch (code) {
    'dns' => l10n.couldntFindServerAddressCheck2,
    'tls' => l10n.couldntConnectSecurelyOverHttps,
    'timeout' => l10n.serverDidntRespondTimeCheck,
    'http' =>
      l10n.serverCouldntProcessRequestTry('$status'),
    'wrong_server' => l10n.addressIsntMultitapServerCheck,
    'http_consent' => l10n.youNeedAgreeUnencryptedHttp2,
    'http_unsupported' =>
      l10n.serverDoesntAllowHttpConnections2,
    'http_disabled' => l10n.httpConnectionsTurnedOffServer2,
    'origin_not_allowed' =>
      l10n.addressIsntRegisteredServerCheck2,
    'setup_only' =>
      l10n.httpAddressOnlyServersFirst,
    'http_profile_mismatch' =>
      l10n.addressDoesntMatchServersSettings2,
    'connection_refused' =>
      l10n.cantReachServerAddressPort2,
    _ => l10n.couldntConnectServerCheckAddress,
  };
}

// Read-only check of the entered origin. No account, password or session is sent.
Future<BrokerProbeResult> probeBroker(
  Uri origin, {
  Duration timeout = const Duration(seconds: 8),
  HttpClient Function()? clientFactory,
  String? httpConsentOrigin,
  bool closeClient = true,
  bool rememberConsent = true,
}) async {
  final validOrigin = parseServerOrigin(origin.toString());
  String? explicit = httpConsentOrigin;
  try {
    explicit ??= validOrigin.scheme == 'http'
        ? BrokerTransport.consentOrigin(validOrigin)
        : null;
  } on FormatException {
    throw const BrokerProbeException('http_consent');
  }
  if (validOrigin.scheme == 'http' &&
      (explicit == null || normalizeBrokerOrigin(explicit) != validOrigin)) {
    throw const BrokerProbeException('http_consent');
  }
  final priorToken = validOrigin.scheme == 'http' && httpConsentOrigin == null
      ? BrokerTransport.token(validOrigin)
      : null;
  final client = clientFactory?.call() ?? HttpClient();
  client.connectionTimeout = timeout;
  final clock = Stopwatch()..start();
  var observing = true;
  HttpClientRequest? healthRequest;
  var complete = false;
  try {
    return await (() async {
      final request = await client.getUrl(validOrigin.resolve('/healthz'));
      healthRequest = request;
      if (!observing) {
        try {
          request.abort();
        } catch (_) {}
        throw const BrokerProbeException('timeout');
      }
      request.followRedirects = false;
      request.headers.set(HttpHeaders.acceptHeader, 'application/json');
      request.headers.set(brokerWireHeader, brokerWireVersion);
      final response = await request.close();
      final bytes = <int>[];
      await for (final chunk in response) {
        if (bytes.length + chunk.length > 65536) {
          throw const BrokerProbeException('wrong_server');
        }
        bytes.addAll(chunk);
      }
      if (response.statusCode != HttpStatus.ok) {
        Object? denied;
        try {
          denied = jsonDecode(utf8.decode(bytes));
        } catch (_) {
          /* fixed status below */
        }
        throw BrokerProbeException(
          brokerAccessFailureCode(denied) ?? 'http',
          status: response.statusCode,
        );
      }
      final value = jsonDecode(utf8.decode(bytes));
      if (value is! Map<String, dynamic> ||
          value['service'] != 'multitap-broker' ||
          value['status'] != 'ok' ||
          value['authentication'] != 'id-password' ||
          value['version'] is! String ||
          !RegExp(r'^\d+\.\d+\.\d+(?:[-+][A-Za-z0-9.-]+)?$')
              .hasMatch(value['version'] as String) ||
          (value['version'] as String).length > 40) {
        throw const BrokerProbeException('wrong_server');
      }
      if (validOrigin.scheme == 'http' && value['media'] != 'relay-v1') {
        final modern =
            (value['transport'] is Map &&
                value['transport']['protocol'] == 'broker-transport-v1') ||
            (value['access'] is Map &&
                value['access']['protocol'] == 'server-access-v1');
        throw BrokerProbeException(
          modern ? 'http_profile_mismatch' : 'http_unsupported',
        );
      }
      try {
        validateHttpTransportHealth(
          validOrigin,
          value['transport'],
          access: value['access'],
        );
      } on BrokerTransportHealthException catch (failure) {
        throw BrokerProbeException(failure.code);
      }
      if (!observing) throw const BrokerProbeException('timeout');
      complete = true;
      HttpTransportApproval? approval;
      if (validOrigin.scheme == 'http') {
        if (rememberConsent) {
          approval = BrokerTransport.approve(validOrigin, explicit!);
        } else {
          BrokerTransport.verifiedCurrent(
            validOrigin,
            priorToken ?? BrokerTransport.token(validOrigin),
          );
        }
      }
      return BrokerProbeResult(
        version: value['version'] as String,
        elapsedMs: clock.elapsedMilliseconds,
        httpApproval: approval,
      );
    })().timeout(timeout);
  } on BrokerProbeException {
    rethrow;
  } on TimeoutException {
    throw const BrokerProbeException('timeout');
  } on HandshakeException {
    throw const BrokerProbeException('tls');
  } on SocketException catch (error) {
    throw BrokerProbeException(brokerSocketFailureCode(error));
  } on FormatException {
    throw const BrokerProbeException('wrong_server');
  } on IOException {
    throw const BrokerProbeException('network');
  } finally {
    observing = false;
    if (!complete) {
      try {
        healthRequest?.abort();
      } catch (_) {
        /* Cleanup cannot mask the fixed failure. */
      }
    }
    clock.stop();
    if (closeClient) client.close(force: true);
  }
}

Future<void> ensureBrokerTransport(
  Uri origin, {
  HttpClient? client,
  bool force = false,
}) async {
  final token = BrokerTransport.token(origin);
  if (force || !BrokerTransport.verified(origin)) {
    await probeBroker(
      origin,
      clientFactory: client == null ? null : () => client,
      closeClient: client == null,
      rememberConsent: false,
    );
  }
  BrokerTransport.requireCurrent(origin, token);
}

// Platform-native numeric refusal/timeout values only; no arbitrary text is shown.
String brokerSocketFailureCode(SocketException error) {
  final code = error.osError?.errorCode;
  final message = error.message.toLowerCase();
  if (const [-2, 7, 8, 11001, 11002, 11003, 11004].contains(code) ||
      message.contains('host lookup') ||
      message.contains('getaddrinfo')) {
    return 'dns';
  }
  if (const [61, 111, 10061].contains(code)) return 'connection_refused';
  if (const [54, 104, 10054].contains(code)) return 'connection_reset';
  if (const [60, 110, 10060].contains(code)) return 'timeout';
  return 'network';
}
