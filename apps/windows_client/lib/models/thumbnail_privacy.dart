import 'package:flutter/foundation.dart';

const thumbnailPrivacyProtocol = 'thumbnail-privacy-v1';
final _thumbnailEpoch = RegExp(r'^[0-9a-f]{32}$');

bool isThumbnailPrivacyEpoch(Object? value) =>
    value is String &&
    _thumbnailEpoch.hasMatch(value) &&
    value.substring(16) != '0000000000000000';

/// Authenticated Host observation only; never stream/input readiness.
@immutable
class ThumbnailPrivacy {
  const ThumbnailPrivacy({required this.epoch, required this.allowed});
  final String epoch;
  final bool allowed;

  static ThumbnailPrivacy? parse(Object? value) {
    if (value is! Map ||
        value.length != 3 ||
        value['protocol'] != thumbnailPrivacyProtocol ||
        !isThumbnailPrivacyEpoch(value['epoch']) ||
        value['allowed'] is! bool) {
      return null;
    }
    return ThumbnailPrivacy(
      epoch: value['epoch'] as String,
      allowed: value['allowed'] as bool,
    );
  }

  /// Only the broker's fresh authenticated projection is authority. An older
  /// broker may blindly forward raw Host capabilities, so never consume those.
  static ThumbnailPrivacy? fromHost(Object? value) {
    if (value is! Map ||
        value['connected'] != true ||
        value['capabilitiesPending'] == true) {
      return null;
    }
    return parse(value['thumbnailPrivacy']);
  }

  @override
  bool operator ==(Object other) =>
      other is ThumbnailPrivacy &&
      other.epoch == epoch &&
      other.allowed == allowed;
  @override
  int get hashCode => Object.hash(epoch, allowed);
}
