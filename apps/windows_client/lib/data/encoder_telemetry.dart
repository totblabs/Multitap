import 'dart:convert';
import '../l10n/l10n.dart';

/// Host encoder observations only. Never this client's decoder or permission.
class HostEncoderInfo {
  const HostEncoderInfo({
    required this.backend,
    required this.initialized,
    required this.hardwareVerified,
    required this.gpuFallbackCode,
    required this.softwareFallbackCode,
    this.adapterLuid,
    this.vendorId,
    this.deviceId,
    this.adapterName,
  });
  final String backend;
  final bool initialized, hardwareVerified;
  final int gpuFallbackCode, softwareFallbackCode;
  final String? adapterLuid, adapterName;
  final int? vendorId, deviceId;
  static const backends = {'x264', 'mft', 'nvenc', 'amf', 'qsv-mft'};
  static const keys = {
    'protocol',
    'backend',
    'initialized',
    'hardwareVerified',
    'adapterLuid',
    'vendorId',
    'deviceId',
    'adapterName',
    'gpuFallbackCode',
    'softwareFallbackCode',
  };
  static bool gpuCode(Object? code) =>
      code is int && (code == 0 || (code >= 32 && code <= 44));
  static HostEncoderInfo? parse(Object? value) {
    if (value is! Map ||
        value.length != keys.length ||
        !keys.containsAll(value.keys) ||
        value['protocol'] != 'h264-encoder-v1' ||
        !backends.contains(value['backend']) ||
        value['initialized'] is! bool ||
        value['hardwareVerified'] is! bool ||
        !gpuCode(value['gpuFallbackCode']) ||
        value['softwareFallbackCode'] is! int ||
        value['softwareFallbackCode'] < 0 ||
        value['softwareFallbackCode'] > 7) {
      return null;
    }
    final backend = value['backend'] as String;
    final initialized = value['initialized'] as bool,
        hardware = value['hardwareVerified'] as bool;
    final luid = value['adapterLuid'],
        vendor = value['vendorId'],
        device = value['deviceId'],
        name = value['adapterName'];
    bool id(Object? n) => n == null || (n is int && n > 0 && n <= 0xffffffff);
    if (!id(vendor) ||
        !id(device) ||
        (name != null && !validEncoderAdapterName(name))) {
      return null;
    }
    if (luid != null &&
        (luid is! String ||
            !RegExp(r'^[0-9a-f]{8}:[0-9a-f]{8}$').hasMatch(luid) ||
            luid == '00000000:00000000')) {
      return null;
    }
    final software = backend == 'x264' || backend == 'mft';
    if (software &&
        (hardware ||
            luid != null ||
            vendor != null ||
            device != null ||
            name != null)) {
      return null;
    }
    if (!initialized &&
        (hardware ||
            luid != null ||
            vendor != null ||
            device != null ||
            name != null)) {
      return null;
    }
    final expectedVendor = switch (backend) {
      'nvenc' => 0x10de,
      'amf' => 0x1002,
      'qsv-mft' => 0x8086,
      _ => null,
    };
    if (expectedVendor != null && vendor != null && vendor != expectedVendor) {
      return null;
    }
    if (hardware &&
        (!initialized || luid == null || vendor != expectedVendor)) {
      return null;
    }
    return HostEncoderInfo(
      backend: backend,
      initialized: initialized,
      hardwareVerified: hardware,
      gpuFallbackCode: value['gpuFallbackCode'] as int,
      softwareFallbackCode: value['softwareFallbackCode'] as int,
      adapterLuid: luid as String?,
      vendorId: vendor as int?,
      deviceId: device as int?,
      adapterName: name as String?,
    );
  }
}

bool validEncoderAdapterName(Object value) {
  if (value is! String ||
      value.isEmpty ||
      value.length > 128 ||
      utf8.encode(value).length > 512 ||
      value.runes.any(
        (n) => n < 32 || (n >= 0x7f && n <= 0x9f) || n == 0x2028 || n == 0x2029,
      )) {
    return false;
  }
  final units = value.codeUnits;
  for (var i = 0; i < units.length; i++) {
    if (units[i] >= 0xd800 && units[i] <= 0xdbff) {
      if (++i >= units.length || units[i] < 0xdc00 || units[i] > 0xdfff) {
        return false;
      }
    } else if (units[i] >= 0xdc00 && units[i] <= 0xdfff) {
      return false;
    }
  }
  return true;
}

String hostEncoderLabel(HostEncoderInfo? value, [String? legacy]) {
  if (value == null) {
    return switch (legacy) {
      'x264' => 'CPU · x264',
      'mft' => l10n.mediaFoundationDeviceNotVerified,
      _ => '—',
    };
  }
  if (!value.initialized) return l10n.checkingInitialization;
  final label = hostEncoderBackendLabel(value.backend);
  if (value.backend == 'x264' || value.backend == 'mft') return label;
  return value.hardwareVerified ? 'GPU · $label' : l10n.outputNotVerified(label);
}

String hostEncoderBackendLabel(String backend) => switch (backend) {
  'x264' => 'CPU · x264',
  'mft' => 'CPU · Media Foundation',
  'nvenc' => 'NVIDIA NVENC',
  'amf' => 'AMD AMF',
  'qsv-mft' => 'Intel · Media Foundation',
  _ => '—',
};

String? hostEncoderFallback(HostEncoderInfo? value) {
  if (value == null) return null;
  return hostEncoderFallbackCodes(
    value.gpuFallbackCode,
    value.softwareFallbackCode,
  );
}

String? hostEncoderFallbackCodes(
  int gpuFallbackCode,
  int softwareFallbackCode,
) {
  return switch (gpuFallbackCode) {
    32 => l10n.buildWithoutGpuEncoder,
    33 => l10n.selectedGpuNotFound,
    34 => l10n.vendorNotSupportedCurrentGpu,
    35 => l10n.gpuDriverEncoderUnavailable,
    36 => l10n.gpuApiVersionNotSupported,
    37 => l10n.requestedH264FormatNot,
    38 => l10n.gpuInitializationFailed,
    39 => l10n.gpuMemoryAllocationFailed,
    40 => l10n.gpuFrameSubmissionFailed,
    41 => l10n.gpuOutputTimedOut,
    42 => l10n.gpuOutputFormatCheckFailed,
    43 => l10n.gpuDeviceDisconnected,
    44 => l10n.gpuSettingsChangeFailed,
    _ => switch (softwareFallbackCode) {
      1 => l10n.x264SettingsRejected,
      2 => l10n.x264InitializationFailed,
      3 => l10n.x264EncodingFailed,
      4 => l10n.x264OutputFormatCheckFailed,
      5 => l10n.x264OutputTooLarge,
      6 => l10n.x264MemoryAllocationFailed,
      7 => l10n.x264ProcessingFailed,
      _ => null,
    },
  };
}
