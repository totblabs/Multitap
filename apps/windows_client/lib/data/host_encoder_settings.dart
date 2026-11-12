import 'dart:math';

import 'host_settings_api.dart';
import 'encoder_telemetry.dart';
import '../l10n/l10n.dart';

final _encoderLuid = RegExp(r'^[a-f0-9]{8}:[a-f0-9]{8}$');
bool _luid(Object? value) =>
    value is String &&
    _encoderLuid.hasMatch(value) &&
    value != '00000000:00000000';
String newEncoderSettingsOperationId() {
  final random = Random.secure();
  final bytes = List<int>.generate(16, (_) => random.nextInt(256));
  bytes[6] = (bytes[6] & 15) | 64;
  bytes[8] = (bytes[8] & 63) | 128;
  final h = bytes.map((v) => v.toRadixString(16).padLeft(2, '0')).join();
  return '${h.substring(0, 8)}-${h.substring(8, 12)}-${h.substring(12, 16)}-${h.substring(16, 20)}-${h.substring(20)}';
}

class HostEncoderAdapter {
  const HostEncoderAdapter(
    this.id,
    this.vendorId,
    this.deviceId,
    this.name,
    this.candidate,
  );
  final String id;
  final int vendorId, deviceId;
  final String? name;
  final bool candidate;
  String get label =>
      name ??
      'GPU · ${switch (vendorId) {
        0x10de => 'NVIDIA',
        0x1002 => 'AMD',
        0x8086 => 'Intel',
        _ => l10n.unknownVendor,
      }}';
}

class HostEncoderSettings {
  const HostEncoderSettings({
    this.supported = false,
    this.administrator = false,
    this.revision,
    this.backend = 'x264',
    this.adapterId,
    this.operationId,
    this.inventoryKnown = false,
    this.adapters = const [],
    this.recentUsage,
    this.autoSupported = false,
  });
  final bool supported, administrator, inventoryKnown, autoSupported;
  final String? revision, adapterId, operationId;
  final String backend;
  final List<HostEncoderAdapter> adapters;

  /// Historical validated-output record; never the current live encoder.
  final HostEncoderUsage? recentUsage;
  bool get canSave => supported && administrator && revision != null;
  static HostEncoderSettings parse(Object? value) {
    if (value is! Map ||
        value['schema'] != 1 ||
        value['supported'] != true ||
        value['administrator'] is! bool ||
        value['inventoryKnown'] is! bool ||
        value['inventoryIsEncoderSupport'] != false ||
        value['revision'] is! String ||
        !RegExp(r'^[a-f0-9]{64}$').hasMatch(value['revision']) ||
        !const {'x264', 'mft', 'gpu', 'auto'}.contains(value['backend']) ||
        (value['backend'] == 'auto' && value['autoSupported'] != true) ||
        (value['adapterId'] != null && !_luid(value['adapterId'])) ||
        (value['operationId'] != null &&
            (value['operationId'] is! String ||
                !WindowsAutologonStatus.uuidPattern.hasMatch(
                  value['operationId'],
                ))) ||
        value['adapters'] is! List ||
        (value['adapters'] as List).length > 32) {
      return const HostEncoderSettings();
    }
    final rows = <HostEncoderAdapter>[], ids = <String>{};
    for (final row in value['adapters'] as List) {
      if (row is! Map ||
          !_luid(row['id']) ||
          !ids.add(row['id']) ||
          row['vendorId'] is! int ||
          row['vendorId'] < 0 ||
          row['vendorId'] > 0xffffffff ||
          row['deviceId'] is! int ||
          row['deviceId'] < 0 ||
          row['deviceId'] > 0xffffffff ||
          row['candidate'] is! bool ||
          (row['name'] != null && !validEncoderAdapterName(row['name']))) {
        return const HostEncoderSettings();
      }
      if (row['candidate'] !=
          const {0x10de, 0x1002, 0x8086}.contains(row['vendorId'])) {
        return const HostEncoderSettings();
      }
      rows.add(
        HostEncoderAdapter(
          row['id'],
          row['vendorId'],
          row['deviceId'],
          row['name'] as String?,
          row['candidate'] as bool,
        ),
      );
    }
    if (value['backend'] != 'gpu' && value['adapterId'] != null) {
      return const HostEncoderSettings();
    }
    return HostEncoderSettings(
      supported: true,
      administrator: value['administrator'],
      revision: value['revision'],
      backend: value['backend'],
      adapterId: value['adapterId'],
      operationId: value['operationId'],
      inventoryKnown: value['inventoryKnown'],
      adapters: List.unmodifiable(rows),
      recentUsage: HostEncoderUsage.parse(value['recentUsage']),
      autoSupported: value['autoSupported'] == true,
    );
  }
}

class HostEncoderUsage {
  const HostEncoderUsage(
    this.time,
    this.backend,
    this.trackId,
    this.hardwareVerified,
    this.gpuFallbackCode,
    this.softwareFallbackCode,
  );
  final DateTime time;
  final String backend, trackId;
  final bool hardwareVerified;
  final int gpuFallbackCode, softwareFallbackCode;
  String get label =>
      '${hardwareVerified ? 'GPU · ' : ''}${hostEncoderBackendLabel(backend)}';
  String? get fallback =>
      hostEncoderFallbackCodes(gpuFallbackCode, softwareFallbackCode);
  static HostEncoderUsage? parse(Object? value) {
    if (value is! Map ||
        value['protocol'] != 'host-encoder-usage-v1' ||
        !const {'0', '1'}.contains(value['trackId']) ||
        !HostEncoderInfo.backends.contains(value['backend']) ||
        value['hardwareVerified'] is! bool ||
        !HostEncoderInfo.gpuCode(value['gpuFallbackCode']) ||
        value['softwareFallbackCode'] is! int ||
        value['softwareFallbackCode'] < 0 ||
        value['softwareFallbackCode'] > 7 ||
        value['time'] is! String ||
        !RegExp(r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$')
            .hasMatch(value['time'])) {
      return null;
    }
    final time = DateTime.tryParse(value['time'] as String);
    final gpu = !const {'x264', 'mft'}.contains(value['backend']);
    if (time == null ||
        time.toIso8601String() != value['time'] ||
        value['hardwareVerified'] != gpu) {
      return null;
    }
    // Historical evidence is deliberately a separate DTO; it cannot create a
    // live h264-encoder-v1 snapshot with a fabricated/missing adapter identity.
    return HostEncoderUsage(
      time,
      value['backend'] as String,
      value['trackId'] as String,
      gpu,
      value['gpuFallbackCode'] as int,
      value['softwareFallbackCode'] as int,
    );
  }
}

class HostEncoderSettingsApi {
  const HostEncoderSettingsApi(this.api);
  final HostSettingsApi api;
  Future<HostEncoderSettings> read() async => HostEncoderSettings.parse(
    (await api.request('encoderSettings'))['encoderSettings'],
  );
  Future<HostEncoderSettings> apply({
    required String operationId,
    required String expectedRevision,
    required String backend,
    String? adapterId,
  }) async => HostEncoderSettings.parse(
    (await api.request('applyEncoderSettings', {
      'operationId': operationId,
      'expectedRevision': expectedRevision,
      'backend': backend,
      'adapterId': adapterId,
      'confirmed': true,
    }))['encoderSettings'],
  );
}

String encoderSettingsFailure(Object error) {
  final text = error.toString();
  for (final entry in {
    'encoder_settings_changed': l10n.anotherSettingsChangeWasMade,
    'encoder_adapter_changed': l10n.selectedGpuChangedCheckList,
    'encoder_settings_busy': l10n.anotherEncoderSettingsTaskProgress,
    'encoder_settings_untrusted':
        l10n.couldntVerifyEncoderSettingsProtection,
    'encoder_settings_invalid': l10n.couldntReadEncoderSettingsFile,
    'encoder_settings_save_failed': l10n.couldntConfirmSaveResultCheck,
    'encoder_settings_result_unknown': l10n.saveResponseDoesntMatchCurrent,
    'encoder_inventory_unavailable': l10n.couldntReadGpuListCpu,
  }.entries) {
    if (text.contains(entry.key)) return entry.value;
  }
  return l10n.couldntCheckEncoderSettingsUpdate;
}
