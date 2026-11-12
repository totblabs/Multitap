import 'broker_transport.dart';
import 'broker_probe.dart';

import 'dart:io';

// A broker session URL is validated before this call. Refuse redirects too:
// an HTTP redirect must never move its Authorization header to another target.
class _NoRedirectClient implements HttpClient {
  final _client = HttpClient()..connectionTimeout = const Duration(seconds: 12);
  @override
  Future<HttpClientRequest> openUrl(String method, Uri url) async {
    final request = await _client.openUrl(method, url);
    request.followRedirects = false;
    return request;
  }

  @override
  void close({bool force = false}) => _client.close(force: force);
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

Future<WebSocket> connectRelaySocket(
  Uri uri,
  String token, {
  int maxPayloadLength = 4 * 1024 * 1024 + 12,
  Uri? origin,
  Map<String, String> headers = const {},
}) async {
  final configured = origin ?? brokerOriginForRelay(uri);
  if (!brokerRelayMatches(uri, configured)) {
    throw ArgumentError(
      'A relay origin must match the configured HTTPS broker.',
    );
  }
  final admission = BrokerTransport.token(configured);
  if (configured.scheme == 'http') {
    await ensureBrokerTransport(configured, force: true);
  }
  BrokerTransport.requireCurrent(configured, admission);
  final client = _NoRedirectClient();
  var connected = false;
  try {
    final socket = await WebSocket.connect(
      uri.toString(),
      customClient: client,
      headers: {
        for (final entry in headers.entries)
          if (entry.key.toLowerCase() != brokerWireHeader.toLowerCase())
            entry.key: entry.value,
        brokerWireHeader: brokerWireVersion,
        HttpHeaders.authorizationHeader: 'Bearer $token',
        if (origin != null) 'Origin': origin.origin,
      },
      compression: CompressionOptions.compressionOff,
      maxPayloadLength: maxPayloadLength,
    ).timeout(const Duration(seconds: 12));
    connected = true;
    return socket;
  } finally {
    client.close(force: !connected);
  }
}
