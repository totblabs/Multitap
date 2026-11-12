import '../l10n/l10n.dart';

/// Explicit dialect marker for the Multitap wire protocol during a coordinated
/// rollout. This carries no authentication or authorization.
const brokerWireHeader = 'X-Multitap-Wire';
const brokerWireVersion = '1';

/// One broker origin and an explicit local HTTP decision. No network or storage
/// side effects occur while parsing addresses or health JSON.
Uri normalizeBrokerOrigin(String input, {String defaultScheme = 'https'}) {
  if (!const {'http', 'https'}.contains(defaultScheme) ||
      input.isEmpty ||
      input.length > 512 ||
      RegExp(r'[\s\x00-\x20\x7f\\@?#%]').hasMatch(input)) {
    throw FormatException(l10n.checkYouEnteredServerAddress);
  }
  final text = input.contains('://') ? input : '$defaultScheme://$input';
  final separator = text.indexOf('://');
  final scheme = text.substring(0, separator).toLowerCase();
  if (scheme != 'http' && scheme != 'https') {
    throw FormatException(l10n.enterHttpHttpsServerAddress);
  }
  var authority = text.substring(separator + 3);
  final slash = authority.indexOf('/');
  if (slash >= 0) {
    if (authority.substring(slash) != '/') {
      throw FormatException(l10n.dontIncludePathServerAddress);
    }
    authority = authority.substring(0, slash);
  }
  var portText = '';
  String host;
  if (authority.startsWith('[')) {
    final end = authority.indexOf(']');
    if (end < 0) throw FormatException(l10n.checkIpv6Address);
    final address = authority.substring(1, end);
    if (address.contains('.')) {
      final tail = address.substring(address.lastIndexOf(':') + 1).split('.');
      if (tail.length != 4 ||
          tail.any(
            (v) =>
                !RegExp(r'^(0|[1-9]\d{0,2})$').hasMatch(v) ||
                int.parse(v) > 255,
          )) {
        throw FormatException(l10n.checkIpv6Address);
      }
    }
    final bytes = Uri.parseIPv6Address(address);
    final words = [
      for (var i = 0; i < 16; i += 2) (bytes[i] << 8) | bytes[i + 1],
    ];
    var best = -1, longest = 0;
    for (var i = 0; i < 8;) {
      if (words[i] != 0) {
        i++;
        continue;
      }
      final start = i;
      while (i < 8 && words[i] == 0) {
        i++;
      }
      if (i - start > longest && i - start >= 2) {
        best = start;
        longest = i - start;
      }
    }
    final out = StringBuffer();
    for (var i = 0; i < 8;) {
      if (i == best) {
        out.write('::');
        i += longest;
        continue;
      }
      final current = out.toString();
      if (current.isNotEmpty && !current.endsWith(':')) out.write(':');
      out.write(words[i++].toRadixString(16));
    }
    host = out.toString();
    final suffix = authority.substring(end + 1);
    if (suffix.isNotEmpty) {
      if (!suffix.startsWith(':')) throw FormatException(l10n.checkServerPort);
      portText = suffix.substring(1);
      if (portText.isEmpty) throw FormatException(l10n.enterServerPort);
    }
  } else {
    final colon = authority.indexOf(':');
    var raw = colon < 0 ? authority : authority.substring(0, colon);
    if (colon >= 0) {
      if (authority.indexOf(':', colon + 1) >= 0) {
        throw FormatException(l10n.ipv6AddressesNeedSquareBrackets);
      }
      portText = authority.substring(colon + 1);
      if (portText.isEmpty) throw FormatException(l10n.enterServerPort);
    }
    raw = raw.replaceAll(RegExp('[\u3002\uff0e\uff61]'), '.').toLowerCase();
    final rootDot = raw.endsWith('.');
    final labels = (rootDot ? raw.substring(0, raw.length - 1) : raw).split(
      '.',
    );
    final numeric =
        labels.isNotEmpty &&
        (RegExp(r'^\d+$').hasMatch(labels.last) ||
            labels.last.startsWith('0x'));
    if (numeric) {
      if (labels.length != 4 ||
          labels.any(
            (v) =>
                !RegExp(r'^(0|[1-9]\d{0,2})$').hasMatch(v) ||
                int.parse(v) > 255,
          )) {
        throw FormatException(l10n.enterIpAddressAsFour);
      }
      host = labels.join('.');
    } else {
      final ascii = labels.map(_asciiBrokerLabel).toList();
      host = '${ascii.join('.')}${rootDot ? '.' : ''}';
    }
    if (host.isEmpty || host.length > 253) {
      throw FormatException(l10n.checkServerAddress);
    }
  }
  final port = portText.isEmpty
      ? (scheme == 'https' ? 443 : 80)
      : int.tryParse(portText);
  if ((portText.isNotEmpty && !RegExp(r'^\d+$').hasMatch(portText)) ||
      port == null ||
      port < 1 ||
      port > 65535) {
    throw FormatException(l10n.portNumberFrom165535);
  }
  final wrapped = host.contains(':') ? '[$host]' : host;
  return Uri.parse(
    '$scheme://$wrapped${port == (scheme == 'https' ? 443 : 80) ? '' : ':$port'}',
  );
}

// RFC 3492 label encoding. ASCII syntax is still independently bounded below;
// no DNS lookup, native trust change or Unicode/path percent decoding occurs.
String _asciiBrokerLabel(String label) {
  if (label.isEmpty ||
      label.runes.any(
        (v) =>
            v <= 32 ||
            v == 0x7f ||
            (v >= 0xd800 && v <= 0xdfff) ||
            (v >= 0x2000 && v <= 0x200f) ||
            v == 0x2028 ||
            v == 0x2029,
      )) {
    throw FormatException(l10n.checkServerAddress);
  }
  var result = label;
  if (label.runes.any((v) => v >= 128)) {
    final points = label.runes.toList(), output = <int>[];
    for (final point in points) {
      if (point < 128) output.add(point);
    }
    final basic = output.length;
    if (basic > 0) output.add(45);
    var n = 128, delta = 0, bias = 72, handled = basic;
    int digit(int value) => value < 26 ? 97 + value : 22 + value;
    int adapt(int value, int count, bool first) {
      value = first ? value ~/ 700 : value ~/ 2;
      value += value ~/ count;
      var k = 0;
      while (value > 455) {
        value ~/= 35;
        k += 36;
      }
      return k + (36 * value) ~/ (value + 38);
    }

    while (handled < points.length) {
      final next = points.where((v) => v >= n).reduce((a, b) => a < b ? a : b);
      delta += (next - n) * (handled + 1);
      n = next;
      for (final point in points) {
        if (point < n) delta++;
        if (point != n) continue;
        var q = delta;
        for (var k = 36; ; k += 36) {
          final t = k <= bias
              ? 1
              : k >= bias + 26
              ? 26
              : k - bias;
          if (q < t) break;
          output.add(digit(t + (q - t) % (36 - t)));
          q = (q - t) ~/ (36 - t);
        }
        output.add(digit(q));
        bias = adapt(delta, handled + 1, handled == basic);
        delta = 0;
        handled++;
      }
      delta++;
      n++;
    }
    result = 'xn--${String.fromCharCodes(output)}';
  }
  if (result.length > 63 ||
      !RegExp(r'^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$').hasMatch(result)) {
    throw FormatException(l10n.checkServerAddress);
  }
  return result;
}

/// A refused HTTP address with its fixed reason. The message is what the
/// person reads; [code] is what a log may keep.
class BrokerTransportRefusal extends FormatException {
  BrokerTransportRefusal(this.code, String message) : super(message);

  /// 'http_approval_missing': the address has no HTTP approval in this run.
  /// 'http_unverified': its approval has no passed connection test.
  final String code;
}

class HttpTransportApproval {
  HttpTransportApproval._(this.origin, this._token, this._lease);
  final Uri origin;
  final Object _token, _lease;
  bool get current =>
      BrokerTransport.current(origin, _token) &&
      (BrokerTransport._leases[origin.origin]?.contains(_lease) ?? false);
  void release() => BrokerTransport._release(origin, _token, _lease);
}

class BrokerTransport {
  static final _tokens = <String, Object>{};
  static final _verified = <String, Object>{};
  static final _leases = <String, Set<Object>>{};
  static String? _stored;
  static String key(Uri origin) =>
      normalizeBrokerOrigin(origin.toString()).origin;
  static Object? token(Uri origin) {
    final normalized = normalizeBrokerOrigin(origin.toString());
    if (normalized.scheme == 'https') return null;
    final value = _tokens[normalized.origin];
    if (value == null) {
      throw BrokerTransportRefusal(
        'http_approval_missing',
        l10n.agreeHttpServerAddressFirst,
      );
    }
    return value;
  }

  static bool current(Uri origin, Object? value) =>
      origin.scheme == 'https' ||
      (value != null && identical(_tokens[key(origin)], value));
  static void requireCurrent(Uri origin, Object? value) {
    if (!current(origin, value)) {
      throw FormatException(l10n.serverSettingsChangedConnectAgain);
    }
  }

  static bool verified(Uri origin) =>
      origin.scheme == 'https' ||
      identical(_verified[key(origin)], token(origin));
  static String? consentOrigin(Uri origin) {
    token(origin);
    return origin.scheme == 'http' ? key(origin) : null;
  }

  static HttpTransportApproval approve(Uri origin, String consent) {
    final normalized = normalizeBrokerOrigin(origin.toString());
    if (normalized.scheme != 'http' ||
        normalizeBrokerOrigin(consent) != normalized) {
      throw FormatException(l10n.httpConsentAddressDifferent);
    }
    final value = _tokens.putIfAbsent(normalized.origin, Object.new);
    final lease = Object();
    _leases.putIfAbsent(normalized.origin, () => <Object>{}).add(lease);
    _verified[normalized.origin] = value;
    return HttpTransportApproval._(normalized, value, lease);
  }

  static void verifiedCurrent(Uri origin, Object? value) {
    requireCurrent(origin, value);
    if (origin.scheme == 'http') _verified[key(origin)] = value!;
  }

  static void _release(Uri origin, Object value, Object lease) {
    final name = key(origin);
    if (!identical(_tokens[name], value)) return;
    _leases[name]?.remove(lease);
    if (_leases[name]?.isEmpty == true) _leases.remove(name);
    if (_stored != name && !_leases.containsKey(name)) {
      _tokens.remove(name);
      _verified.remove(name);
    }
  }

  static void stored(Uri origin) {
    final value = token(origin), name = key(origin);
    _tokens.removeWhere((key, _) => key != name);
    _verified.removeWhere((key, _) => key != name);
    _leases.removeWhere((key, _) => key != name);
    _stored = origin.scheme == 'http' ? name : null;
    if (value == null) {
      _tokens.clear();
      _verified.clear();
      _leases.clear();
    }
  }

  static void restore(Uri origin, Object? consent) {
    final normalized = normalizeBrokerOrigin(origin.toString());
    if (normalized.scheme == 'http' &&
        (consent is! String || consent != normalized.origin)) {
      clear();
      throw FormatException(l10n.checkHttpConsentAgain);
    }
    if (normalized.scheme == 'http' &&
        _stored == normalized.origin &&
        _tokens.containsKey(normalized.origin)) {
      return;
    }
    _tokens.clear();
    _verified.clear();
    _leases.clear();
    _stored = null;
    if (normalized.scheme == 'http') {
      _stored = normalized.origin;
      _tokens[normalized.origin] = Object();
    }
  }

  static void clear() {
    _tokens.clear();
    _verified.clear();
    _leases.clear();
    _stored = null;
  }
}

Uri decodeBrokerTransportSettings(Object? value) {
  if (value is! Map || value['serverOrigin'] is! String) {
    throw FormatException(l10n.checkServerSettings);
  }
  final origin = normalizeBrokerOrigin(value['serverOrigin'] as String);
  if (value.length == 1 && origin.scheme == 'https') return origin;
  final expected = origin.scheme == 'http'
      ? {'version', 'serverOrigin', 'httpConsentOrigin'}
      : {'version', 'serverOrigin'};
  if (value['version'] is! int ||
      value['version'] != 2 ||
      value['serverOrigin'] != origin.origin ||
      value.length != expected.length ||
      !expected.containsAll(value.keys) ||
      (origin.scheme == 'http' &&
          value['httpConsentOrigin'] != origin.origin)) {
    throw FormatException(l10n.checkHttpConsentAgain);
  }
  return origin;
}

Map<String, Object> encodeBrokerTransportSettings(Uri origin) {
  final valid = normalizeBrokerOrigin(origin.toString());
  BrokerTransport.token(valid);
  if (!BrokerTransport.verified(valid)) {
    throw BrokerTransportRefusal(
      'http_unverified',
      l10n.testConnectionCurrentHttpAddress,
    );
  }
  return {
    'version': 2,
    'serverOrigin': valid.origin,
    if (valid.scheme == 'http') 'httpConsentOrigin': valid.origin,
  };
}

String brokerRelayScheme(Uri origin) => origin.scheme == 'https' ? 'wss' : 'ws';
int brokerRelayPort(Uri relay) => relay.hasPort
    ? relay.port
    : relay.scheme == 'wss'
    ? 443
    : 80;
bool brokerOriginAllowed(Uri origin) {
  try {
    BrokerTransport.token(origin);
    return true;
  } on FormatException {
    return false;
  }
}

bool brokerRelayAllowed(Uri relay) {
  try {
    return brokerRelayMatches(relay, brokerOriginForRelay(relay));
  } on FormatException {
    return false;
  }
}

Uri brokerOriginForRelay(Uri relay) {
  if (!const {'ws', 'wss'}.contains(relay.scheme) ||
      relay.userInfo.isNotEmpty ||
      relay.hasQuery ||
      relay.hasFragment) {
    throw FormatException(l10n.invalidRelayAddress);
  }
  return normalizeBrokerOrigin(
    relay
        .replace(scheme: relay.scheme == 'wss' ? 'https' : 'http', path: '')
        .toString(),
  );
}

bool brokerRelayMatches(Uri relay, Uri origin, {String? path}) {
  try {
    BrokerTransport.token(origin);
    return relay.scheme == brokerRelayScheme(origin) &&
        brokerOriginForRelay(relay) ==
            normalizeBrokerOrigin(origin.toString()) &&
        (path == null || relay.path == path);
  } on FormatException {
    return false;
  }
}

/// Fixed reason only. Access aliases are informational: they never authorize
/// another origin, copy credentials, change scheme or replace local consent.
class BrokerTransportHealthException extends FormatException {
  BrokerTransportHealthException(this.code) : super(l10n.checkServersAccessSettings);
  final String code;
}

String? brokerAccessFailureCode(Object? body) {
  if (body is! Map) return null;
  final raw = body['error'];
  final code = raw is String
      ? raw
      : raw is Map
      ? raw['code']
      : null;
  return switch (code) {
    'http_disabled' || 'http_not_allowed' => 'http_disabled',
    'origin_not_allowed' || 'unregistered_origin' => 'origin_not_allowed',
    _ => null,
  };
}

void validateHttpTransportHealth(Uri origin, Object? value, {Object? access}) {
  if (origin.scheme == 'https') {
    return; // existing HTTPS compatibility unchanged
  }
  final modern = access is Map && access['protocol'] == 'server-access-v1';
  if (access is Map && modern && access['bootstrapOnly'] == true) {
    throw BrokerTransportHealthException('setup_only');
  }
  if (value is! Map || value['protocol'] != 'broker-transport-v1') {
    throw BrokerTransportHealthException(
      modern ? 'http_profile_mismatch' : 'http_unsupported',
    );
  }
  if (value.length != 5 ||
      value['publicOrigin'] is! String ||
      value['scheme'] is! String ||
      value['allowInsecureHttp'] is! bool ||
      value['directTls'] is! bool) {
    throw BrokerTransportHealthException('http_profile_mismatch');
  }
  if (value['publicOrigin'] != BrokerTransport.key(origin) ||
      value['scheme'] != 'http') {
    throw BrokerTransportHealthException('origin_not_allowed');
  }
  if (value['allowInsecureHttp'] == false) {
    throw BrokerTransportHealthException('http_disabled');
  }
  if (value['directTls'] != false) {
    throw BrokerTransportHealthException('http_profile_mismatch');
  }
}
