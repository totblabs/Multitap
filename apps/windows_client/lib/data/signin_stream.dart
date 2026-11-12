/// Optional, single-output Windows sign-in stream contract. It grants no OS
/// authority and must never enable a launcher entry or Host capability itself.
const signinStreamProtocol = 'signin-stream-v1';

/// A short-lived offer from the authenticated broker's assigned inventory.
/// This only selects the explicit sign-in protocol; Host/native authority is
/// checked again at reservation, actual dispatch, capture and input.
class SigninAvailability {
  const SigninAvailability({
    required this.id,
    required this.expiresAt,
    required this.windowsState,
  });
  final String id, windowsState;
  final DateTime expiresAt;
  bool fresh(DateTime now) =>
      expiresAt.isAfter(now) &&
      expiresAt.difference(now) <= const Duration(seconds: 10);
  static SigninAvailability? tryParse(Object? value) {
    if (value is! Map ||
        value.length != 4 ||
        !const {
          'protocol',
          'availabilityId',
          'expiresAtMs',
          'windowsState',
        }.every(value.containsKey) ||
        value['protocol'] != signinStreamProtocol ||
        value['availabilityId'] is! String ||
        !RegExp(r'^[a-f0-9]{32}$')
            .hasMatch(value['availabilityId'] as String) ||
        value['expiresAtMs'] is! int ||
        (value['expiresAtMs'] as int) <= 0 ||
        (value['expiresAtMs'] as int) > 8640000000000000 ||
        !const {'locked', 'no-user'}.contains(value['windowsState'])) {
      return null;
    }
    return SigninAvailability(
      id: value['availabilityId'] as String,
      expiresAt: DateTime.fromMillisecondsSinceEpoch(
        value['expiresAtMs'] as int,
        isUtc: true,
      ),
      windowsState: value['windowsState'] as String,
    );
  }
}

const _u32 = 0xffffffff;
final _opaque = RegExp(r'^[a-f0-9]{32}$');
bool _int(Object? value, int max) => value is int && value > 0 && value <= max;
bool _shape(Map value, Set<String> keys) =>
    value.length == keys.length && keys.every(value.containsKey);

enum SigninPhase { signin, desktop }

/// Composite epoch is uint16. TVT1 topology/per-track uint32 IDs are a different
/// contract and are deliberately unsupported in this first implementation.
class SigninMediaScope {
  const SigninMediaScope(this.epoch);
  final int epoch;
  factory SigninMediaScope.parse(Object? raw) {
    if (raw is! Map ||
        !_shape(raw, {'kind', 'epoch'}) ||
        raw['kind'] != 'composite' ||
        !_int(raw['epoch'], 65535)) {
      throw const FormatException('Unsupported sign-in media scope.');
    }
    return SigninMediaScope(raw['epoch'] as int);
  }
  Map<String, Object> toJson() => {'kind': 'composite', 'epoch': epoch};
  @override
  bool operator ==(Object other) =>
      other is SigninMediaScope && other.epoch == epoch;
  @override
  int get hashCode => epoch.hashCode;
}

class SigninBinding {
  const SigninBinding({
    required this.phase,
    required this.workerGeneration,
    required this.transitionId,
    required this.mediaScope,
  });
  final SigninPhase phase;
  final int workerGeneration;
  final String transitionId;
  final SigninMediaScope mediaScope;
  factory SigninBinding.parse(Object? raw) {
    if (raw is! Map ||
        !_shape(raw, {
          'protocol',
          'phase',
          'workerGeneration',
          'transitionId',
          'mediaScope',
        }) ||
        raw['protocol'] != signinStreamProtocol ||
        !['signin', 'desktop'].contains(raw['phase']) ||
        !_int(raw['workerGeneration'], _u32) ||
        raw['transitionId'] is! String ||
        !_opaque.hasMatch(raw['transitionId'] as String)) {
      throw const FormatException('Invalid sign-in binding.');
    }
    return SigninBinding(
      phase: raw['phase'] == 'signin'
          ? SigninPhase.signin
          : SigninPhase.desktop,
      workerGeneration: raw['workerGeneration'] as int,
      transitionId: raw['transitionId'] as String,
      mediaScope: SigninMediaScope.parse(raw['mediaScope']),
    );
  }
  Map<String, Object> toJson() => {
    'protocol': signinStreamProtocol,
    'phase': phase.name,
    'workerGeneration': workerGeneration,
    'transitionId': transitionId,
    'mediaScope': mediaScope.toJson(),
  };
  SigninBinding withEpoch(int epoch) => SigninBinding(
    phase: phase,
    workerGeneration: workerGeneration,
    transitionId: transitionId,
    mediaScope: SigninMediaScope(epoch),
  );
  @override
  bool operator ==(Object other) =>
      other is SigninBinding &&
      other.phase == phase &&
      other.workerGeneration == workerGeneration &&
      other.transitionId == transitionId &&
      other.mediaScope == mediaScope;
  @override
  int get hashCode =>
      Object.hash(phase, workerGeneration, transitionId, mediaScope);
}

class SigninFrameInfo {
  SigninFrameInfo(
    this.binding,
    this.firstFrameId,
    this.info, {
    this.resize = false,
  });
  final SigninBinding binding;
  final String firstFrameId;
  final Map<String, dynamic> info;
  final bool resize;
}

/// A local owner captured before input can wait in an async paste/shortcut.
/// It is never serialized; only its already-observed binding may reach wire.
class SigninInputOwner {
  const SigninInputOwner._(this._state, this.revision, this.binding);
  final Object _state;
  final int revision;
  final SigninBinding binding;
}

class SigninRedirectOwner {
  const SigninRedirectOwner._(this._state, this.binding);
  final Object _state;
  final SigninBinding binding;
}

class SigninStreamState {
  SigninStreamState({required this.sessionId, required SigninBinding initial})
    : binding = SigninBinding.parse(initial.toJson());
  final String sessionId;
  final Object _identity = Object();
  SigninBinding binding;
  int revision = 0;
  bool active = false,
      closed = false,
      infoAccepted = false,
      keyAccepted = false;
  String? firstFrameId;
  int? preparationDeadlineUs;
  bool _resizing = false;
  bool get redirectsAllowed =>
      active && !closed && binding.phase == SigninPhase.desktop;

  /// Only owners already admitted in this exact desktop worker may survive
  /// its video resize. This is not permission to grant/reconnect a channel.
  SigninRedirectOwner? get redirectOwner =>
      redirectsAllowed ? SigninRedirectOwner._(_identity, binding) : null;
  bool retainsRedirects(SigninRedirectOwner? owner) =>
      !closed &&
      (active || _resizing) &&
      owner != null &&
      identical(owner._state, _identity) &&
      owner.binding.phase == SigninPhase.desktop &&
      binding.phase == SigninPhase.desktop &&
      owner.binding.workerGeneration == binding.workerGeneration &&
      owner.binding.transitionId == binding.transitionId;
  SigninInputOwner? get inputOwner => active && !closed
      ? SigninInputOwner._(_identity, revision, binding)
      : null;
  bool owns(SigninInputOwner? owner) =>
      owner != null &&
      active &&
      !closed &&
      identical(owner._state, _identity) &&
      owner.revision == revision &&
      owner.binding == binding;

  SigninBinding _controlBinding(
    Map<String, dynamic> event,
    String type,
    Set<String> extra,
  ) {
    if (!_shape(event, {
          'type',
          'protocol',
          'sessionId',
          'transitionId',
          'workerGeneration',
          'phase',
          'mediaScope',
          ...extra,
        }) ||
        event['type'] != type ||
        event['sessionId'] != sessionId) {
      throw const FormatException('Invalid sign-in control identity.');
    }
    return SigninBinding.parse({
      for (final key in [
        'protocol',
        'phase',
        'workerGeneration',
        'transitionId',
        'mediaScope',
      ])
        key: event[key],
    });
  }

  /// Exact duplicate controls do not restart cleanup or extend the deadline.
  bool prepare(Map<String, dynamic> event, int nowUs) {
    final next = _controlBinding(event, 'stream.phase.prepare', {'timeoutMs'});
    final timeout = event['timeoutMs'];
    if (!_int(timeout, 90000) || closed) throw const FormatException();
    if (next == binding && (infoAccepted || preparationDeadlineUs != null)) {
      return false;
    }
    if (!active ||
        next.phase == binding.phase ||
        binding.workerGeneration == _u32 ||
        binding.mediaScope.epoch == 65535 ||
        next.workerGeneration != binding.workerGeneration + 1 ||
        next.mediaScope.epoch != binding.mediaScope.epoch + 1 ||
        next.transitionId == binding.transitionId) {
      throw const FormatException('Unannounced sign-in generation.');
    }
    binding = next;
    _resizing = false;
    revision++;
    active = infoAccepted = keyAccepted = false;
    firstFrameId = null;
    preparationDeadlineUs = nowUs + (timeout as int) * 1000;
    return true;
  }

  SigninFrameInfo? acceptInfo(Map<String, dynamic> event, int nowUs) {
    final value = _controlBinding(event, 'stream.phase.info', {
      'firstFrameId',
      'info',
    });
    final resize =
        active &&
        binding.phase == SigninPhase.desktop &&
        value.phase == binding.phase &&
        value.workerGeneration == binding.workerGeneration &&
        value.transitionId == binding.transitionId &&
        binding.mediaScope.epoch < 65535 &&
        value.mediaScope.epoch == binding.mediaScope.epoch + 1;
    if (closed ||
        (value != binding && !resize) ||
        (preparationDeadlineUs != null && nowUs > preparationDeadlineUs!) ||
        event['firstFrameId'] is! String ||
        !_opaque.hasMatch(event['firstFrameId'] as String) ||
        event['info'] is! Map) {
      throw const FormatException('Invalid sign-in frame correlation.');
    }
    if (infoAccepted && !resize) {
      if (event['firstFrameId'] != firstFrameId) throw const FormatException();
      return null;
    }
    if (active && !resize) throw const FormatException();
    final info = Map<String, dynamic>.from(event['info'] as Map);
    if (info['type'] != 'stream.info' ||
        info['sessionId'] != sessionId ||
        info['epoch'] != value.mediaScope.epoch) {
      throw const FormatException();
    }
    if (resize) {
      binding = value;
      revision++;
      active = keyAccepted = false;
      _resizing = true;
      // Metadata duplicates cannot renew this bounded fresh-decoder wait.
      preparationDeadlineUs = nowUs + 15000000;
    }
    infoAccepted = true;
    firstFrameId = event['firstFrameId'] as String;
    return SigninFrameInfo(binding, firstFrameId!, info, resize: resize);
  }

  void acceptedIdr(int epoch) {
    if (!closed && infoAccepted && epoch == binding.mediaScope.epoch) {
      keyAccepted = true;
    }
  }

  /// Flow correlation is not a decoded frame sequence. Only a caller holding
  /// the fresh media lease/revision with actual native output may invoke this.
  Map<String, dynamic>? decoded() {
    if (closed ||
        active ||
        !infoAccepted ||
        !keyAccepted ||
        firstFrameId == null) {
      return null;
    }
    active = true;
    _resizing = false;
    preparationDeadlineUs = null;
    return {
      'type': 'stream.phase.decoded',
      'protocol': signinStreamProtocol,
      'sessionId': sessionId,
      'transitionId': binding.transitionId,
      'workerGeneration': binding.workerGeneration,
      'mediaScope': binding.mediaScope.toJson(),
      'firstFrameId': firstFrameId,
    };
  }

  Map<String, dynamic>? wrapInput(
    Map<String, dynamic> input,
    SigninInputOwner? owner,
  ) {
    if (!owns(owner) ||
        !['input', 'input.reset', 'input.fence'].contains(input['type'])) {
      return null;
    }
    final stamp = owner!.binding;
    return {
      'type': 'input.phase',
      'protocol': signinStreamProtocol,
      'sessionId': sessionId,
      'transitionId': stamp.transitionId,
      'workerGeneration': stamp.workerGeneration,
      'mediaScope': stamp.mediaScope.toJson(),
      'input': Map<String, dynamic>.from(input),
    };
  }

  void close() {
    closed = true;
    active = false;
    revision++;
  }
}

/// Same Annex-B first-frame requirement as independent tracks: SPS, PPS and
/// IDR, not merely a peer-controlled keyframe flag. Native decode remains
/// independently required before readiness.
bool signinIdrPacket(List<int> bytes, int epoch) {
  if (bytes.length < 17 ||
      bytes[0] != 1 ||
      bytes[1] != 1 ||
      (bytes[2] | bytes[3] << 8) != epoch) {
    return false;
  }
  var mask = 0;
  for (var n = 12; n + 3 < bytes.length; n++) {
    var prefix = 0;
    if (bytes[n] == 0 && bytes[n + 1] == 0 && bytes[n + 2] == 1) {
      prefix = 3;
    } else if (n + 4 < bytes.length &&
        bytes[n] == 0 &&
        bytes[n + 1] == 0 &&
        bytes[n + 2] == 0 &&
        bytes[n + 3] == 1) {
      prefix = 4;
    }
    if (prefix == 0) continue;
    final header = bytes[n + prefix];
    if (header & 128 != 0) return false;
    switch (header & 31) {
      case 5:
        mask |= 1;
      case 7:
        mask |= 2;
      case 8:
        mask |= 4;
    }
    n += prefix;
  }
  return mask == 7;
}
