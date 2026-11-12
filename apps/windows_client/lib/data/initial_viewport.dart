import 'stream_size.dart';

const initialViewportProtocol = 'epoch-v1';
const viewportEncoderLimitsProtocol = 'separate-v1';

bool viewportEncoderLimitsAdvertised(Object? value) =>
    value is Map &&
    initialViewportAdvertised(value) &&
    value['viewportEncoderLimitsProtocol'] == viewportEncoderLimitsProtocol &&
    value['resizeProtocol'] == 'epoch-v1' &&
    value['dynamicResolution'] == true &&
    value['viewportProvisioning'] == true &&
    value['monitors'] is List &&
    (value['monitors'] as List).length == 1 &&
    (value['monitors'] as List).single is Map &&
    ((value['monitors'] as List).single as Map)['id'] == '0';

/// A profile may share initial/ordinary viewport changes only through its
/// separate negotiated contract. Generic viewport support never implies it.
bool initialViewportAdvertised(Object? value) {
  if (value is! Map ||
      value['initialViewportProtocol'] != initialViewportProtocol) {
    return false;
  }
  if (value['streamProfileProtocol'] == null) {
    return value['streamProfileViewportProtocol'] == null;
  }
  return value['streamProfileProtocol'] == 'console-profile-v1' &&
      value['streamProfileViewportProtocol'] == 'epoch-v1';
}

/// Actual owned Windows output pixels, independent from encoder limits. A
/// portrait size taller than 2160 rows needs stream-size-v1 (the connect
/// request refuses it otherwise).
class InitialViewport {
  InitialViewport({
    required this.width,
    required this.height,
    required this.fps,
    this.preserveEncoderLimits = false,
  }) {
    if (!ownedOutputSize(width, height, portrait: true) ||
        (fps != 30 && fps != 60)) {
      throw const FormatException('Invalid initial owned viewport.');
    }
  }

  /// Within the released 3840x2160 rule (no stream-size-v1 needed).
  bool get released => legacyOwnedOutputSize(width, height);
  final int width, height, fps;
  final bool preserveEncoderLimits;
  Map<String, Object> toJson() => {
    'monitor': '0',
    'width': width,
    'height': height,
    'fps': fps,
  };
}

class InitialViewportPreparation {
  InitialViewportPreparation._(
    this.sessionId,
    this.requestId,
    this.resizeId,
    this.viewport,
  );
  factory InitialViewportPreparation.fromJson(
    Object? value, {
    required String sessionId,
    required InitialViewport requested,
  }) {
    const fields = {
      'protocol',
      'sessionId',
      'requestId',
      'resizeId',
      'monitor',
      'width',
      'height',
      'fps',
      'epoch',
    };
    final uuid = RegExp(
      r'^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$',
    );
    if (value is! Map ||
        value.length != fields.length ||
        !value.keys.every(fields.contains) ||
        value['protocol'] != initialViewportProtocol ||
        value['sessionId'] != sessionId ||
        !uuid.hasMatch(sessionId) ||
        value['requestId'] is! String ||
        !uuid.hasMatch(value['requestId'] as String) ||
        value['resizeId'] is! String ||
        !uuid.hasMatch(value['resizeId'] as String) ||
        value['requestId'] == value['resizeId'] ||
        value['epoch'] != 1 ||
        value['epoch'] is! int ||
        requested.toJson().entries.any((e) => value[e.key] != e.value) ||
        value['width'] is! int ||
        value['height'] is! int ||
        value['fps'] is! int) {
      throw const FormatException('Uncorrelated initial viewport.');
    }
    return InitialViewportPreparation._(
      sessionId,
      value['requestId'] as String,
      value['resizeId'] as String,
      requested,
    );
  }
  final String sessionId, requestId, resizeId;
  final InitialViewport viewport;
  bool matchesInfo(Map<String, dynamic> value, int maxWidth, int maxHeight) =>
      value['sessionId'] == sessionId &&
      value['requestId'] == requestId &&
      value['resizeId'] == resizeId &&
      value['epoch'] == 1 &&
      value['sourceWidth'] == viewport.width &&
      value['sourceHeight'] == viewport.height &&
      value['fpsTarget'] == viewport.fps &&
      value['width'] is int &&
      value['height'] is int &&
      (value['width'] as int) <= maxWidth &&
      (value['height'] as int) <= maxHeight &&
      (value['width'] as int) <= viewport.width &&
      (value['height'] as int) <= viewport.height;
}
