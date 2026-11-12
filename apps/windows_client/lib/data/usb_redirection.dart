import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

import 'channel_redial.dart';
import 'client_diagnostic_codes.dart';
import 'product_log.dart';
import 'relay_socket.dart';
import 'usb_memory.dart';
import 'usb_picker.dart';
import '../l10n/l10n.dart';

/// usb-v1 (docs/usb-redirection.md): a USB device of this PC is made available
/// inside the VM. The Host's driver in the VM asks for URBs (control, bulk,
/// interrupt) over this session's own relayed socket; this client runs each one
/// on the captured local device and returns the result. The broker grants the
/// channel only with the per-assignment `usbDevices` permission; the broker
/// and the Host check every device again, so the filtering here is
/// convenience (and a guard against offering what is never allowed), not the
/// enforcement.
const usbProtocol = 'usb-v1';

/// The broker's word that it passes usb-v1 on to the Host of an
/// independent-outputs ("모든 모니터") session as well.
const usbTracksProtocol = 'usb-tracks-v1';
const usbMaxUrbBytes = 262144;
const usbHeaderBytes = 24;
const usbMaxDevices = 4;
const usbMaxAttaches = 64;

/// Linux errno values (negated) the wire allows as a request's status.
const usbStatusOk = 0,
    usbStatusNoEntry = -2,
    usbStatusIo = -5,
    usbStatusNoDevice = -19,
    usbStatusStall = -32,
    usbStatusTimeout = -62,
    usbStatusProtocol = -71,
    usbStatusOverflow = -75,
    usbStatusReset = -104,
    usbStatusShutdown = -108;
const usbWireStatuses = <int>{
  0,
  -2,
  -5,
  -19,
  -32,
  -62,
  -71,
  -75,
  -104,
  -108,
  -110,
};

// --- descriptors and the device policy -------------------------------------

class UsbEndpoint {
  const UsbEndpoint(this.address, this.type, this.maxPacket);
  final int address, type, maxPacket;
}

class UsbInterface {
  const UsbInterface({
    required this.number,
    required this.alternate,
    required this.cls,
    required this.subclass,
    required this.protocol,
    required this.endpoints,
  });
  final int number, alternate, cls, subclass, protocol;
  final List<UsbEndpoint> endpoints;
}

class UsbConfiguration {
  const UsbConfiguration(this.value, this.interfaces);
  final int value;
  final List<UsbInterface> interfaces;
}

/// A device descriptor followed by every configuration descriptor in full,
/// parsed strictly (null for anything malformed). The same rules as the
/// broker (server/src/usb.mjs) and the Host (usb_policy.hpp).
class UsbDescriptors {
  UsbDescriptors._(this.bytes, this.deviceClass, this.configurations);
  final Uint8List bytes;
  final int deviceClass;
  final List<UsbConfiguration> configurations;

  static const maxBytes = 4096;

  /// A device descriptor [parse] refuses only for its SuperSpeed control
  /// endpoint (bMaxPacketSize0 = 9: a USB 3 device in a USB 3 port). The
  /// same device in a USB 2.0 port reports 64 and can be offered.
  static bool superSpeed(Uint8List bytes) =>
      bytes.length >= 18 && bytes[0] == 18 && bytes[1] == 1 && bytes[7] == 9;

  static UsbDescriptors? parse(Uint8List bytes) {
    if (bytes.length < 18 + 9 ||
        bytes.length > maxBytes ||
        bytes[0] != 18 ||
        bytes[1] != 1) {
      return null;
    }
    final count = bytes[17];
    final maxPacket0 = bytes[7];
    if (count < 1 || count > 4 || ![8, 16, 32, 64].contains(maxPacket0)) {
      return null;
    }
    var at = 18;
    final configurations = <UsbConfiguration>[];
    for (var index = 0; index < count; index++) {
      if (at + 9 > bytes.length || bytes[at] != 9 || bytes[at + 1] != 2) {
        return null;
      }
      final total = bytes[at + 2] | bytes[at + 3] << 8;
      if (total < 9 || at + total > bytes.length) return null;
      final value = bytes[at + 5];
      if (bytes[at + 4] < 1 || value < 1) return null;
      final interfaces = <_InterfaceBuilder>[];
      for (var cursor = at + 9; cursor < at + total;) {
        final length = bytes[cursor];
        final type = bytes[cursor + 1];
        if (length < 2 || cursor + length > at + total) return null;
        if (type == 4) {
          if (length != 9) return null;
          interfaces.add(
            _InterfaceBuilder(
              bytes[cursor + 2],
              bytes[cursor + 3],
              bytes[cursor + 4],
              bytes[cursor + 5],
              bytes[cursor + 6],
              bytes[cursor + 7],
            ),
          );
        } else if (type == 5) {
          if (interfaces.isEmpty || (length != 7 && length != 9)) return null;
          interfaces.last.endpoints.add(
            UsbEndpoint(
              bytes[cursor + 2],
              bytes[cursor + 3] & 3,
              bytes[cursor + 4] | bytes[cursor + 5] << 8,
            ),
          );
        }
        cursor += length;
      }
      if (interfaces.isEmpty ||
          interfaces.any((item) => item.endpoints.length != item.declared)) {
        return null;
      }
      configurations.add(
        UsbConfiguration(value, [for (final item in interfaces) item.build()]),
      );
      at += total;
    }
    if (at != bytes.length) return null;
    return UsbDescriptors._(
      Uint8List.fromList(bytes),
      bytes[4],
      configurations,
    );
  }

  /// Is [address] (bit 7 = IN) a bulk or interrupt endpoint of this device,
  /// or endpoint 0? Nothing else is ever used.
  /// A hub (device class 9 or a hub interface): this PC's other USB devices
  /// hang off it, so it is never offered.
  bool get hasHub =>
      deviceClass == 9 ||
      configurations.any((c) => c.interfaces.any((i) => i.cls == 9));

  bool hasEndpoint(int address) {
    if (address & 0x7f == 0) return true;
    for (final configuration in configurations) {
      for (final item in configuration.interfaces) {
        for (final endpoint in item.endpoints) {
          if (endpoint.address == address &&
              (endpoint.type == 2 || endpoint.type == 3)) {
            return true;
          }
        }
      }
    }
    return false;
  }
}

class _InterfaceBuilder {
  _InterfaceBuilder(
    this.number,
    this.alternate,
    this.declared,
    this.cls,
    this.subclass,
    this.protocol,
  );
  final int number, alternate, declared, cls, subclass, protocol;
  final endpoints = <UsbEndpoint>[];
  UsbInterface build() => UsbInterface(
    number: number,
    alternate: alternate,
    cls: cls,
    subclass: subclass,
    protocol: protocol,
    endpoints: List.unmodifiable(endpoints),
  );
}

class UsbDecision {
  const UsbDecision.allowed(this.requires, this.classes)
    : allowed = true,
      reason = null;
  const UsbDecision.denied(this.reason)
    : allowed = false,
      requires = const {},
      classes = const {};
  final bool allowed;
  final String? reason;

  /// Other administrator permissions the interfaces need (drives, printers,
  /// smartCards, serialPorts).
  final Set<String> requires;
  final Set<int> classes;
}

const _classPermissions = <int, String>{
  0x02: 'serialPorts',
  0x0a: 'serialPorts',
  0x07: 'printers',
  0x08: 'drives',
  0x0b: 'smartCards',
};
const _deniedClasses = <int, String>{
  0x01: 'usb_isochronous_denied',
  0x09: 'usb_class_denied',
  0x0e: 'usb_isochronous_denied',
  0xe0: 'usb_class_denied',
};

/// The default deny list: hubs, wireless controllers (Bluetooth), audio and
/// video (isochronous), any isochronous endpoint, and boot-protocol HID
/// (keyboards and mice, which the VM keeps using from its own console).
UsbDecision usbDecide(UsbDescriptors device) {
  final deniedDevice = _deniedClasses[device.deviceClass];
  if (deniedDevice != null) return UsbDecision.denied(deniedDevice);
  final requires = <String>{};
  final classes = <int>{};
  for (final configuration in device.configurations) {
    for (final item in configuration.interfaces) {
      classes.add(item.cls);
      if (item.endpoints.any((endpoint) => endpoint.type == 1)) {
        return const UsbDecision.denied('usb_isochronous_denied');
      }
      final denied = _deniedClasses[item.cls];
      if (denied != null) return UsbDecision.denied(denied);
      if (item.cls == 0x03 && item.subclass == 1) {
        return const UsbDecision.denied('usb_hid_boot_denied');
      }
      final permission = _classPermissions[item.cls];
      if (permission != null) requires.add(permission);
    }
  }
  return UsbDecision.allowed(requires, classes);
}

// --- relay frames ----------------------------------------------------------------

const usbFrameSubmit = 1,
    usbFrameReturn = 2,
    usbFrameUnlink = 3,
    usbFrameUnlinkReturn = 4;

class UsbFrame {
  UsbFrame({
    required this.type,
    required this.device,
    this.endpoint = 0,
    this.transfer = 0,
    required this.seq,
    this.length = 0,
    this.status = 0,
    Uint8List? setup,
    Uint8List? payload,
  }) : setup = setup ?? Uint8List(8),
       payload = payload ?? Uint8List(0);
  final int type, device, endpoint, transfer, seq, length, status;
  final Uint8List setup, payload;

  Uint8List encode() {
    final out = Uint8List(usbHeaderBytes + payload.length);
    final view = ByteData.sublistView(out);
    out[0] = type;
    out[1] = device;
    out[2] = endpoint;
    out[3] = transfer;
    view.setUint32(4, seq, Endian.little);
    view.setUint32(8, length, Endian.little);
    view.setInt32(12, status, Endian.little);
    out.setRange(16, 24, setup);
    out.setRange(usbHeaderBytes, out.length, payload);
    return out;
  }

  static UsbFrame? decode(Uint8List data) {
    if (data.length < usbHeaderBytes ||
        data.length > usbHeaderBytes + usbMaxUrbBytes) {
      return null;
    }
    final view = ByteData.sublistView(data);
    return UsbFrame(
      type: data[0],
      device: data[1],
      endpoint: data[2],
      transfer: data[3],
      seq: view.getUint32(4, Endian.little),
      length: view.getUint32(8, Endian.little),
      status: view.getInt32(12, Endian.little),
      setup: Uint8List.fromList(data.sublist(16, 24)),
      payload: Uint8List.fromList(data.sublist(usbHeaderBytes)),
    );
  }
}

// --- the local side ----------------------------------------------------------------

/// A USB device of this PC as the capture backend lists it. [name] is shown to
/// the user on this PC only; it (and the vendor/product IDs inside
/// [descriptors]' device descriptor, which cross to the VM as descriptor
/// bytes) is never written to a log or a diagnostics report.
class UsbLocalDevice {
  const UsbLocalDevice({
    required this.id,
    required this.name,
    required this.descriptors,
    required this.speed,
    this.busy = false,
    this.subtitle = '',
    this.vidPid = '',
    this.kind,
    this.neededByPc = false,
    this.chips = const [],
    this.arrivalMs = 0,
    this.memoryKey = '',
    this.busyElsewhere = false,
    this.noReadableDrive = false,
    this.otherWindow,
    this.unreadable = false,
  });
  final String id, name, speed;
  final Uint8List descriptors;

  /// Another program on this PC holds the device (the capture would fail).
  final bool busy;

  /// The second line ("SanDisk Cruzer Blade · 32GB · exFAT"), small VID:PID
  /// text, the kind the runner classified it as (null: not classified, see
  /// [UsbEntry.kind]) and extra chips ("공동인증서").
  final String subtitle, vidPid;
  final UsbKind? kind;

  /// Keyboard, mouse, hub or network adapter: sharing it would cut this PC off
  /// from its own input or from this very connection, so it is never offered.
  final bool neededByPc;
  final List<String> chips;

  /// When Windows last saw the device arrive (Unix ms, 0 unknown).
  final int arrivalMs;

  /// "vid:pid[:serial]", only ever hashed (usb_memory.dart), never logged.
  final String memoryKey;

  /// Held by another session or program (the runner could not name it).
  final bool busyElsewhere;

  /// A storage device this PC shows no readable drive for. The VM may need
  /// Disk Management; this does not prove it is unformatted or cannot mount it.
  final bool noReadableDrive;

  /// Another remote window of this client holds it for its VM: that window's
  /// VM name ('' when unknown, shown on this PC only); null when none does.
  final String? otherWindow;

  /// UsbDk listed it but would not give its configuration descriptor (only the
  /// 18-byte device descriptor is here): it cannot be offered.
  final bool unreadable;
}

enum UsbKind {
  storage,
  token,
  printer,
  phone,
  camera,
  audio,
  bluetooth,
  hub,
  input,
  network,
  other;

  static UsbKind? parse(Object? value) {
    for (final kind in UsbKind.values) {
      if (kind.name == value) return kind;
    }
    return null;
  }
}

enum UsbDriverState { ready, missing, unsupported }

class UsbEnvironment {
  const UsbEnvironment(this.driver);
  final UsbDriverState driver;
}

class UsbBackendException implements Exception {
  const UsbBackendException(this.code, {this.error = 0});
  final String code;

  /// UsbDk's Win32 error of a refused capture (0 none): a number for the
  /// diagnostics, never shown.
  final int error;
  @override
  String toString() => 'UsbBackendException($code)';
}

class UsbTransfer {
  const UsbTransfer({
    required this.token,
    required this.endpoint,
    required this.type,
    required this.length,
    this.setup,
    required this.data,
  });

  /// Unique among the transfers in flight on one device (the relay sequence
  /// number), so it can be cancelled.
  final int token;
  final int endpoint, type, length;
  final Uint8List? setup;
  final Uint8List data;
}

class UsbTransferResult {
  const UsbTransferResult(this.status, this.data);
  final int status;
  final Uint8List data;
}

/// Counters of one capture, from the runner (fixed numbers only, never data).
/// [firstStatus] is the errno of the first request that was neither a success
/// nor a cancellation, [firstClass] its class (bmRequestType<<8 | bRequest for
/// a control transfer, else 0x10000 | transfer type<<8 | endpoint address),
/// [slowClass] the first request that took longer than 5 s.
class UsbStats {
  const UsbStats({
    this.urbs = 0,
    this.kib = 0,
    this.stalls = 0,
    this.failed = 0,
    this.timeouts = 0,
    this.slow = 0,
    this.firstClass = 0,
    this.firstStatus = 0,
    this.slowClass = 0,
    this.firstUsbd = 0,
  });
  final int urbs,
      kib,
      stalls,
      failed,
      timeouts,
      slow,
      firstClass,
      firstStatus,
      slowClass;

  /// The raw Windows USBD_STATUS the driver gave for that first request.
  final int firstUsbd;
}

/// The class of a request for the diagnostics (never its data).
int usbRequestClass(int transfer, int endpoint, Uint8List setup) =>
    transfer == 0
    ? (setup[0] << 8 | setup[1])
    : (0x10000 | transfer << 8 | endpoint);

abstract class UsbCapture {
  Future<UsbTransferResult> transfer(UsbTransfer transfer);
  void cancel(int token);

  /// Completes when the device left this PC's bus.
  Future<void> get gone;

  /// Gives the device back to Windows (idempotent) and returns the counters.
  Future<UsbStats> release();
}

/// Whether a storage device may leave this PC: [ready] false means files are
/// open on it ([blocker] names the program when Windows says so). [checked]
/// false: it could not be checked (the hand-over proceeds).
class UsbPrepareResult {
  const UsbPrepareResult({
    this.ready = true,
    this.checked = true,
    this.blocker = '',
  });
  final bool ready, checked;
  final String blocker;
}

abstract class UsbBackend {
  const UsbBackend();
  Future<UsbEnvironment> environment();
  Future<List<UsbLocalDevice>> list();

  /// Asks the PC whether anything is using a storage device before it is taken
  /// (files open on it). May briefly lock/flush volumes, without capturing or
  /// explicitly dismounting them. Completion means those query locks closed.
  Future<UsbPrepareResult> prepare(UsbLocalDevice device) async =>
      const UsbPrepareResult(checked: false);

  /// Requests cancellation between native prepare steps. A blocked Windows
  /// call can still finish later, so its original Future retains ownership.
  void cancelPrepare(UsbLocalDevice device) {}

  /// Starts or stops telling [changes] about devices arriving and leaving.
  Future<void> watch(bool on) async {}

  /// A USB device, disk or volume arrived or left this PC (while watching).
  Stream<void> get changes => const Stream.empty();

  /// Detaches [device] from this PC's own driver. Throws [UsbBackendException]
  /// (`usb_device_busy`, `usb_device_redirected`, `usb_capture_failed`).
  Future<UsbCapture> capture(UsbLocalDevice device);

  /// Asks the other remote window of this client that holds [device] to give
  /// it back through its own session (이 창으로 옮기기). False when no other
  /// window holds it.
  Future<bool> handOver(UsbLocalDevice device) async => false;

  /// Does another remote window of this client still hold [device] (until
  /// UsbDk has let it go)?
  Future<bool> heldElsewhere(UsbLocalDevice device) async => false;

  /// Another remote window asks for a device this window holds: the device
  /// and that window's VM name.
  Stream<({String id, String window})> get handOvers => const Stream.empty();

  /// After a capture was released: did Windows take [device] back (started
  /// again, a disk for storage)? 1 yes, 2 not in time, 0 cannot be told.
  Future<int> returned(UsbLocalDevice device) async => 0;

  /// After [returned] said 2: what Windows was still missing (1 no device node,
  /// 2 not started, 3 no disk under it, 100 + a Windows problem code), 0 unknown.
  int returnDetail(UsbLocalDevice device) => 0;

  /// Installs the capture driver; only ever after the user chose to
  /// (`usbdk_signature_invalid`, `usbdk_install_failed`, `usbdk_elevation_cancelled`, `usbdk_restart_required`,
  /// `usbdk_missing`).
  Future<void> installDriver();
}

// --- the session ---------------------------------------------------------------------

enum UsbEntryState { available, connecting, shared, returning, refused, failed }

class UsbEntry {
  UsbEntry(this.device, this.decision);
  UsbLocalDevice device;
  final UsbDecision decision;
  UsbEntryState state = UsbEntryState.available;

  /// The fixed code of a refusal or failure (see [usbMessage]).
  String? code;

  /// The program that kept a storage device from being handed over, when
  /// Windows named one (shown on this PC only).
  String? blocker;

  /// This device was given to the VM in this session: its re-arrival after the
  /// capture ends is not "just plugged in".
  bool everShared = false;

  /// 이 창으로 옮기기 is waiting for the other window to give it back.
  bool moving = false;

  /// The capture was released but Windows did not start the device again in
  /// time (the user can unplug and replug it). Cleared by the next hand-over or
  /// when the device arrives again.
  bool notReturned = false;
  bool returnUncertain = false;
  UsbEntryState _afterReturnState = UsbEntryState.available;
  String? _afterReturnCode;
  int _returnEpoch = 0;
  int? id;

  /// What the picker sorts it as: the runner's classification, else a guess from
  /// the permissions its interfaces need.
  UsbKind get kind =>
      device.kind ??
      (decision.requires.contains('drives')
          ? UsbKind.storage
          : decision.requires.contains('smartCards')
          ? UsbKind.token
          : decision.requires.contains('printers')
          ? UsbKind.printer
          : UsbKind.other);
}

typedef UsbDialer = Future<WebSocket> Function(Uri url, String token);

/// The user declined the Windows administrator approval for installing UsbDk.
/// Local only: diagnostics record it as a failed installation.
const usbdkElevationCancelled = 'usbdk_elevation_cancelled';

/// Local only (never a diagnostic code): files are open on a storage device, so
/// it was not handed over; and the device is held by another session.
const usbPcInUse = 'usb_pc_in_use', usbBusyElsewhere = 'usb_busy_elsewhere';

/// Local only: Windows did not start the device again after it was given back
/// (the diagnostic code is usb_device_not_returned).
const usbNotReturned = 'usb_device_not_returned';
const usbReturnUnknown = 'usb_return_unknown';

/// UsbDk already gives the device to someone else: another remote window of
/// this client or another program. Local only until the broker lists it
/// (server/src/client-diagnostics.mjs); diagnostics record usb_device_busy.
const usbDeviceRedirected = 'usb_device_redirected';

/// Local only: 이 창으로 옮기기 and the other window did not let the device go
/// in time (diagnostics record usb_device_busy).
const usbMoveTimeout = 'usb_move_timeout';

/// The code a usb outcome is recorded with: a local-only one as the nearest
/// code the broker keeps.
String? usbDiagnosticCode(String? code) =>
    code == usbDeviceRedirected || code == usbMoveTimeout
    ? 'usb_device_busy'
    : code;

/// A device another remote window of this client holds, as its row says it.
String usbOtherWindowMessage(String name) =>
    name.isEmpty ? l10n.useAnotherWindow : l10n.useWindow(name);

/// What the user reads for a fixed usb code.
String usbMessage(String? code) => switch (code) {
  'usb_class_not_granted' =>
    l10n.deviceAlsoNeedsPermissionIts,
  'usb_class_denied' ||
  'usb_isochronous_denied' => l10n.devicesSuchAsKeyboardsMice,
  'usb_hid_boot_denied' => l10n.keyboardsMiceArentConnectedDesktop,
  'usb_descriptor_invalid' => l10n.couldntReadDeviceInformationSo,
  'usb_limit_reached' => l10n.youCanConnectUpDevices(usbMaxDevices),
  'usb_attach_failed' || 'usb_setup_failed' => l10n.desktopDidntReceiveDevice,
  // Another program's USB virtual controller (e.g. usbip-win2) on the remote
  // computer, which may be a PC, not a VM.
  'usb_vhci_unavailable' =>
    l10n.anotherUsbVirtualDeviceProgram,
  'usb_driver_missing' => l10n.desktopsHostHasNoUsb,
  'usb_driver_install_failed' => l10n.couldntInstallUsbConnectionDriver,
  'usb_restart_required' => l10n.restartDesktopUseUsbDevices,
  'usb_vhci_unprotected' => l10n.desktopsUsbConnectionDriverIsnt,
  'usb_policy_changed' => l10n.administratorTurnedOffUsbPermission,
  'usb_device_removed' => l10n.deviceWasUnpluggedFromPc,
  // The channel ended and gave the device back: not a failed connection.
  'usb_session_closed' => l10n.usbReturnedChannelEnded,
  'usb_vm_detached' => l10n.deviceWasRemovedDesktop,
  'usb_device_busy' => l10n.anotherProgramUsingDevice,
  usbPcInUse => l10n.itsUsePcSoCant,
  usbBusyElsewhere => l10n.useSomewhereElse,
  'usb_capture_failed' => l10n.couldntTakeDeviceFromPc,
  usbDeviceRedirected =>
    l10n.anotherWindowProgramAlreadyUsing,
  usbMoveTimeout => l10n.couldntGetDeviceBackFrom,
  'usb_device_error' => l10n.couldntCommunicateDeviceTryAgain,
  'usb_capture_timeout' => l10n.takingDeviceFromPcTook,
  'usb_prepare_timeout' => l10n.openFileCheckDidntFinish,
  usbReturnUnknown =>
    l10n.couldntCheckWhetherPcCan,
  usbNotReturned =>
    l10n.deviceDidntReturnPcSelect,
  'usb_limit_session' => l10n.noMoreDevicesCanConnected,
  // One name for UsbDk on screen: "USB 드라이버" (the install dialog names it).
  'usbdk_missing' =>
    l10n.couldntFindUsbDriverIf,
  'usbdk_signature_invalid' => l10n.couldntVerifyInstallersSignatureSo,
  'usbdk_install_failed' => l10n.couldntInstallUsbDriverTry,
  usbdkElevationCancelled =>
    l10n.windowsAdministratorApprovalWasCanceled,
  'usbdk_restart_required' => l10n.restartPcUseUsbDevices,
  _ => l10n.couldntConnectDevice,
};

/// The administrator console's names for the per-VM device permissions, in
/// its 장치 공유 group.
Map<String, String> get deviceSharingLabels => {
  'drives': l10n.sharedFolder,
  'usbDevices': 'USB',
  'printers': l10n.printer,
  'smartCards': l10n.smartCard,
  'serialPorts': l10n.serialComPort,
};

/// usb_class_not_granted for one device: exactly which other permissions it
/// needs, in the administrator console's words.
String usbClassNotGrantedMessage(Set<String> requires) {
  final names = [
    for (final entry in deviceSharingLabels.entries)
      if (requires.contains(entry.key)) '‘${entry.value}’',
  ];
  return names.isEmpty
      ? usbMessage('usb_class_not_granted')
      : l10n.deviceAlsoNeedsPermissionAsk(names.join(l10n.listSeparator));
}

/// A USB device this client makes up itself instead of capturing one (the
/// game mouse, usb_game_mouse.dart). The session routes its relay messages and
/// frames here; it never appears in the panel.
abstract interface class UsbVirtualDevice {
  /// The VM took it (`usb.attached`).
  void attached();

  /// It is gone: refused, detached by the Host or the broker, or the channel
  /// ended. [reason] is a fixed `usb_*` code.
  void ended(String reason);

  /// A Host frame for it (submit or unlink), as it came.
  void frame(Uint8List bytes);
}

class _Shared {
  _Shared(this.id, this.entry);
  final int id;
  final UsbEntry entry;
  UsbCapture? capture;
  Future<UsbCapture>? acquiring;
  final captured = Completer<bool>();
  final inflight = <int>{};

  /// The VM's requests that are running, and the ones the VM gave up on that
  /// the PC did not let go of (answered for it; their late result is dropped).
  final requests = <int, UsbFrame>{};
  final abandoned = <int>{};
  final cancelled = <int>{};
  int stuckNoted = 0;
  final accepted = Stopwatch();
  bool done = false;
}

class UsbSession extends ChangeNotifier {
  UsbSession({
    required this.url,
    required this._token,
    required this.sessionId,
    required this.backend,
    UsbDialer? dialer,
    this._memory,
    DateTime Function()? clock,
    this.captureTimeout = const Duration(seconds: 20),
    this.prepareTimeout = const Duration(seconds: 15),
    this.cleanupWait = const Duration(seconds: 5),
    this.stuckCancel = const Duration(seconds: 3),
    this.moveWait = const Duration(seconds: 8),
  }) : _dialer = dialer ?? _dial,
       clock = clock ?? DateTime.now;

  /// How long the PC may take to capture a device the VM accepted (the VM's
  /// first requests wait for it), and how long a request the VM cancelled may
  /// keep running on the PC before it is answered for it. [moveWait]: how
  /// long 이 창으로 옮기기 waits for the other window to let the device go.
  final Duration captureTimeout, prepareTimeout, cleanupWait, stuckCancel;
  final Duration moveWait;

  /// Devices this session gave to another remote window on its request, and
  /// that window's VM name (the session window says so once per move).
  int moves = 0;
  String lastMovedTo = '';
  StreamSubscription<({String id, String window})>? _handOvers;

  /// "Now" for the "방금 연결됨" badge and the sort order.
  final DateTime Function() clock;
  final UsbMemory? _memory;

  /// Hashes of the devices given to a VM before (usb_memory.dart).
  Set<String> remembered = const {};

  /// Devices this PC needs (keyboard, mouse, hub, network adapter) that the
  /// list left out; the panel says so in one line.
  int hiddenNeeded = 0;

  /// Devices this PC has whose descriptors could not be read in the last
  /// listing (shown as one line in the panel: plug it in again), and the
  /// ones refused for running at SuperSpeed (its own line: use a USB 2.0
  /// port). Neither is ever offered.
  int unreadable = 0, superSpeed = 0;
  String _lastListing = '';
  final _pending = <UsbEntry, int>{};
  final _preparing = <UsbEntry, Future<UsbPrepareResult>>{};
  final _finishing = <UsbEntry, Future<void>>{};
  StreamSubscription<void>? _changes;
  Timer? _refreshTimer;
  bool _refreshAgain = false;

  static Future<WebSocket> _dial(Uri url, String token) =>
      connectRelaySocket(url, token, maxPayloadLength: usbMaxUrbBytes + 1024);

  final Uri url;
  final String sessionId;
  final UsbBackend backend;
  final String _token;
  final UsbDialer _dialer;
  WebSocket? _socket;
  StreamSubscription<dynamic>? _subscription;

  bool _ready = false, _hostReady = false, _closed = false;
  String? _unavailable;
  String? error;

  /// The broker refused the dial (not a lost network): never dialled again.
  bool dialRefused = false;

  /// channel-resume-v1: this ended channel is being brought back by itself
  /// (or waits for the stream to continue); the views do not say it is lost.
  bool get recovering => _recovering;
  bool _recovering = false;
  set recovering(bool value) {
    if (_recovering == value) return;
    _recovering = value;
    notifyListeners();
  }

  /// Completes when this ended channel gave every device back to the PC
  /// (at once while it runs): a channel that replaces it waits for that.
  Future<void> get settled => _ending ?? Future<void>.value();
  Future<void>? _ending;
  UsbDriverState driver = UsbDriverState.missing;
  bool listing = false, installing = false;

  /// The driver was installed in this session and Windows must restart
  /// before it works: another install would not help.
  bool restartRequired = false;
  List<UsbEntry> entries = const [];
  int _nextId = 1, _attaches = 0, _urbs = 0, _bytes = 0;
  final _shared = <int, _Shared>{};

  /// The channel is up and the VM can take devices.
  bool get ready => _ready && _hostReady && _unavailable == null && !_closed;

  /// The channel is up but the VM is still preparing its USB controller.
  bool get preparing =>
      _ready && !_hostReady && _unavailable == null && !_closed;
  bool get closed => _closed;

  /// Why the VM cannot take devices (a fixed `usb_*` code), or null.
  String? get unavailableReason => _unavailable;
  int get sharedCount => _shared.values.where((item) => !item.done).length;

  Future<void> start() async {
    if (_closed || _socket != null) return;
    try {
      final socket = await _dialer(url, _token);
      if (_closed) {
        unawaited(socket.close().catchError((Object _) {}));
        return;
      }
      _socket = socket;
      _subscription = socket.listen(
        _receive,
        onDone: () => unawaited(_end('usb_transport_closed')),
        onError: (Object _) => unawaited(_end('usb_transport_closed')),
        cancelOnError: true,
      );
    } catch (failure) {
      dialRefused = channelDialRefused(failure);
      await _end('usb_transport_closed');
    }
  }

  Future<void> refresh() async {
    if (_closed) return;
    if (listing) {
      _refreshAgain = true; // something changed while listing: list once more
      return;
    }
    listing = true;
    notifyListeners();
    try {
      final environment = await backend.environment();
      driver = environment.driver;
      final found = environment.driver == UsbDriverState.ready
          ? await backend.list()
          : const <UsbLocalDevice>[];
      if (_closed) return;
      final next = <UsbEntry>[];
      var hidden = 0, denied = 0, broken = 0, fast = 0;
      for (final device in found) {
        final descriptors = device.unreadable
            ? null
            : UsbDescriptors.parse(device.descriptors);
        if (descriptors == null) {
          if (!device.unreadable &&
              UsbDescriptors.superSpeed(device.descriptors)) {
            fast++;
          } else {
            broken++;
          }
          continue;
        }
        final decision = usbDecide(descriptors);
        // Never offered: what this PC needs for itself (keyboard, mouse, hub,
        // USB network adapter: sharing it would cut the connection or the
        // input), counted for the panel's one-line explanation; and what the
        // policy always denies (Bluetooth, audio, cameras), not listed at all.
        if (device.neededByPc ||
            descriptors.hasHub ||
            decision.reason == 'usb_hid_boot_denied') {
          hidden++;
          continue;
        }
        if (!decision.allowed) {
          denied++;
          continue;
        }
        final existing = entries.where((item) => item.device.id == device.id);
        if (existing.isNotEmpty) {
          if (existing.first.device.arrivalMs != device.arrivalMs) {
            existing.first.notReturned = false; // plugged in again
            if (existing.first.returnUncertain) {
              // A new arrival is a reason to recheck, not proof of readiness.
              // Update the stamp before the read-only check binds it.
              existing.first.device = device;
              unawaited(recheckReturn(existing.first));
            }
          }
          existing.first.device = device;
          next.add(existing.first);
        } else {
          next.add(UsbEntry(device, decision));
        }
      }
      // A device being shared stays in the list even when Windows no longer
      // lists it (its PC driver is gone while it is captured).
      for (final item in entries) {
        final live = next.any((entry) => identical(entry, item));
        if (!live &&
            (item.state == UsbEntryState.shared ||
                item.state == UsbEntryState.returning ||
                item.state == UsbEntryState.connecting)) {
          next.add(item);
        }
      }
      entries = next;
      hiddenNeeded = hidden;
      unreadable = broken;
      superSpeed = fast;
      // A USB stick in Explorer and not in the panel must leave a trace: one
      // record per change of what a listing saw (counts only).
      final listing =
          '${found.length}/$hidden/$denied/$broken/$fast/${next.length}';
      if (listing != _lastListing) {
        _lastListing = listing;
        ProductLog.instance.record(LogEvent.usb, {
          'stage': 15,
          'ok': broken + fast == 0,
          'usbFound': found.length,
          'usbHidden': hidden,
          'usbDenied': denied,
          // The record's count stays what it was: every refused descriptor.
          'usbUnreadable': broken + fast,
          'usbDevices': next.length,
        });
      }
    } catch (_) {
      driver = UsbDriverState.unsupported;
    } finally {
      listing = false;
      if (!_closed) notifyListeners();
    }
    if (!_refreshAgain) _tryResume();
    if (_refreshAgain && !_closed) {
      _refreshAgain = false;
      await refresh();
    }
  }

  /// Follows devices arriving and leaving while the channel is up, so the
  /// panel stays right without being reopened.
  Future<void> _watch() async {
    _changes ??= backend.changes.listen((_) {
      _refreshTimer?.cancel();
      // Windows reports a plug-in in several steps (device, disk, volume).
      _refreshTimer = Timer(
        const Duration(milliseconds: 400),
        () => unawaited(refresh()),
      );
    });
    try {
      await backend.watch(true);
    } catch (_) {}
  }

  Future<void> _loadMemory() async {
    try {
      remembered = await _memory?.load() ?? const {};
      if (!_closed) notifyListeners();
    } catch (_) {}
  }

  /// The list as the panel shows it: storage and tokens first, the rest under
  /// "기타", newest first, devices given to a VM before after the new ones.
  UsbPicker picker() => buildUsbPicker(
    entries: entries,
    now: clock(),
    remembered: remembered,
    hiddenNeeded: hiddenNeeded,
  );

  /// Installs the capture driver (only ever from the user's explicit action).
  /// Returns a fixed code on failure.
  Future<String?> installDriver() async {
    if (installing || _closed) return 'usb_busy';
    installing = true;
    notifyListeners();
    try {
      await backend.installDriver();
      await refresh();
      return driver == UsbDriverState.ready ? null : 'usbdk_missing';
    } on UsbBackendException catch (failure) {
      ProductLog.instance.recordFailure(
        LogEvent.usb,
        failure.code == usbdkElevationCancelled
            ? 'usbdk_install_failed'
            : failure.code,
        {'stage': 6, 'ok': false},
      );
      if (failure.code == 'usbdk_restart_required') restartRequired = true;
      return failure.code;
    } catch (_) {
      return 'usbdk_install_failed';
    } finally {
      installing = false;
      if (!_closed) notifyListeners();
    }
  }

  /// Offers [entry] to the VM. It stays on this PC until the VM has taken it.
  /// Returns a fixed code when it cannot be offered.
  String? share(UsbEntry entry) {
    if (!ready) return _unavailable ?? 'usb_vhci_unavailable';
    if (entry.state == UsbEntryState.connecting ||
        entry.state == UsbEntryState.shared ||
        entry.state == UsbEntryState.returning) {
      return null;
    }
    if (entry.returnUncertain) {
      return entry.notReturned ? usbNotReturned : usbReturnUnknown;
    }
    if (entry.device.otherWindow != null) return usbDeviceRedirected;
    if (entry.device.busy) return 'usb_device_busy';
    if (entry.device.busyElsewhere) return usbBusyElsewhere;
    if (sharedCount + _pending.length >= usbMaxDevices) {
      return 'usb_limit_reached';
    }
    if (_attaches >= usbMaxAttaches || _nextId > 255) {
      return 'usb_limit_session';
    }
    entry.notReturned = false;
    entry._returnEpoch++;
    if (entry.kind == UsbKind.storage) {
      // Files open on a drive would be cut off: ask Windows first (nothing is
      // changed on the drive), and only then offer it to the VM.
      entry
        ..id = null
        ..state = UsbEntryState.connecting
        ..code = null
        ..blocker = null;
      final epoch = entry._returnEpoch;
      _pending[entry] = epoch;
      notifyListeners();
      unawaited(_prepareThenOffer(entry, epoch));
      return null;
    }
    _offer(entry);
    return null;
  }

  Future<void> _prepareThenOffer(UsbEntry entry, int epoch) async {
    UsbPrepareResult result;
    final original = Future<UsbPrepareResult>.sync(
      () => backend.prepare(entry.device),
    );
    _preparing[entry] = original;
    try {
      result = await original.timeout(prepareTimeout);
    } on TimeoutException {
      if (_pending[entry] != epoch) return;
      _pending.remove(entry);
      if (_closed || entry._returnEpoch != epoch) return;
      backend.cancelPrepare(entry.device);
      _retainPrepareCleanup(
        entry,
        original,
        epoch,
        UsbEntryState.failed,
        'usb_prepare_timeout',
      );
      ProductLog.instance.recordFailure(LogEvent.usb, entry.code, {
        'stage': 12,
        'ok': false,
      });
      return;
    } catch (_) {
      result = const UsbPrepareResult(
        checked: false,
      ); // not checkable: do not block
    }
    if (_pending[entry] != epoch) return;
    _pending.remove(entry);
    if (identical(_preparing[entry], original)) _preparing.remove(entry);
    if (_closed ||
        entry._returnEpoch != epoch ||
        entry.state != UsbEntryState.connecting) {
      return;
    }
    if (!result.ready) {
      entry
        ..state = UsbEntryState.failed
        ..code = usbPcInUse
        ..blocker = result.blocker.isEmpty ? null : result.blocker;
      notifyListeners();
      return;
    }
    if (!ready || _attaches >= usbMaxAttaches || _nextId > 255) {
      entry
        ..state = UsbEntryState.failed
        ..code = !ready
            ? (_unavailable ?? 'usb_vhci_unavailable')
            : 'usb_limit_session';
      notifyListeners();
      return;
    }
    _offer(entry);
  }

  void _retainPrepareCleanup(
    UsbEntry entry,
    Future<UsbPrepareResult> original,
    int epoch,
    UsbEntryState state,
    String? code,
  ) {
    entry
      ..state = UsbEntryState.returning
      ..code = code;
    final finishing = () async {
      try {
        await original;
      } catch (_) {}
      if (identical(_preparing[entry], original)) _preparing.remove(entry);
      if (entry._returnEpoch == epoch) {
        entry
          ..id = null
          ..state = state;
      }
      if (!_closed) notifyListeners();
    }();
    _trackCleanup(entry, finishing);
    if (!_closed) notifyListeners();
  }

  void _trackCleanup(UsbEntry entry, Future<void> finishing) {
    _finishing[entry] = finishing;
    unawaited(
      finishing.then((_) {
        if (identical(_finishing[entry], finishing)) _finishing.remove(entry);
      }),
    );
  }

  void _offer(UsbEntry entry) {
    final id = _nextId++;
    _attaches++;
    entry
      ..id = id
      ..state = UsbEntryState.connecting
      ..code = null;
    _shared[id] = _Shared(id, entry);
    final classes = entry.decision.classes;
    ProductLog.instance.record(LogEvent.usb, {
      'stage': 7,
      'ok': true,
      'usbClass': classes.contains(8)
          ? 8
          : (classes.isEmpty ? 0 : classes.first),
      'usbDevices': sharedCount,
    });
    _send({
      'type': 'usb.attach',
      'device': id,
      'descriptors': base64Encode(entry.device.descriptors),
      'speed': entry.device.speed,
    });
    notifyListeners();
  }

  /// Gives [entry] back to this PC. Without [verifyReturn] (a device another
  /// window takes next) that window checks Windows took it back, not this one.
  Future<void> stop(
    UsbEntry entry, {
    String reason = 'usb_user_stopped',
    bool verifyReturn = true,
  }) async {
    if (_pending.remove(entry) != null) {
      backend.cancelPrepare(entry.device);
      entry._returnEpoch++;
      final original = _preparing[entry];
      if (original != null) {
        _retainPrepareCleanup(
          entry,
          original,
          entry._returnEpoch,
          UsbEntryState.available,
          null,
        );
      }
      return;
    }
    final id = entry.id;
    final shared = id == null ? null : _shared[id];
    if (shared == null || shared.done) return;
    _send({'type': 'usb.detach', 'device': id, 'reason': reason});
    await _finish(
      shared,
      UsbEntryState.available,
      null,
      verifyReturn: verifyReturn,
    );
  }

  /// 이 창으로 옮기기: the remote window of this client that holds
  /// [entry] gives it back through its own session (its VM is told), Windows
  /// takes it back, and only then is it offered here as usual. The wait is
  /// bounded by [moveWait]; a fixed code says what did not happen.
  Future<String?> moveHere(UsbEntry entry) async {
    if (!ready) return _unavailable ?? 'usb_vhci_unavailable';
    if (entry.moving ||
        entry.state == UsbEntryState.connecting ||
        entry.state == UsbEntryState.shared ||
        entry.state == UsbEntryState.returning) {
      return null;
    }
    entry.moving = true;
    notifyListeners();
    String? failure;
    try {
      failure = await _takeBack(entry);
    } catch (_) {
      failure = usbMoveTimeout;
    } finally {
      entry.moving = false;
    }
    if (_closed) return null;
    if (failure != null) {
      ProductLog.instance.recordFailure(
        LogEvent.usb,
        usbDiagnosticCode(failure),
        {'stage': 13, 'ok': false},
      );
      notifyListeners();
      // Who holds it now shows in the list: the other window still, or
      // nobody (a device that did not come back gets PC로 되돌리기).
      await refresh();
      return failure;
    }
    await refresh(); // the device as Windows lists it now
    // A listing already running lists once more; its result is the one used.
    while (listing && !_closed) {
      await Future<void>.delayed(const Duration(milliseconds: 20));
    }
    if (_closed) return null;
    final refused = share(entry);
    if (refused != null) notifyListeners();
    return refused;
  }

  Future<String?> _takeBack(UsbEntry entry) async {
    if (!await backend.handOver(entry.device)) return null; // nobody holds it
    const step = Duration(milliseconds: 100);
    var waited = Duration.zero;
    while (await backend.heldElsewhere(entry.device)) {
      if (_closed) return null;
      if (waited >= moveWait) return usbMoveTimeout;
      await Future<void>.delayed(step);
      waited += step;
    }
    // Windows starts it again (a storage device with its disk) before this
    // window takes it; the giving window does not check this itself.
    var back = 0;
    try {
      back = await backend
          .returned(entry.device)
          .timeout(const Duration(seconds: 20));
    } catch (_) {}
    if (back == 2) {
      entry
        ..notReturned = true
        ..returnUncertain = true
        ..state = UsbEntryState.failed
        ..code = usbNotReturned;
      return usbNotReturned;
    }
    if (back == 1) {
      entry
        ..notReturned = false
        ..returnUncertain = false;
    }
    return null;
  }

  /// Another remote window of this client asked for a device this session
  /// has (이 창으로 옮기기 there): it is given back like 연결 끊기, so this VM
  /// is told, and this window says where it went.
  Future<void> _giveTo(String id, String window) async {
    final found = entries.where(
      (item) =>
          item.device.id == id &&
          (item.state == UsbEntryState.shared ||
              item.state == UsbEntryState.connecting && item.id != null),
    );
    if (_closed || found.isEmpty) return;
    moves++;
    lastMovedTo = window;
    ProductLog.instance.record(LogEvent.usb, {'stage': 13, 'ok': true});
    notifyListeners();
    await stop(found.first, verifyReturn: false);
  }

  Future<void> close({String reason = 'usb_session_closed'}) => _end(null);

  final _virtual = <int, UsbVirtualDevice>{};

  /// Offers a made-up device to the VM, through the same checks as a real one;
  /// its device number, or null when nothing can be offered now.
  int? attachVirtual(UsbVirtualDevice device, Uint8List descriptors) {
    if (!ready || _attaches >= usbMaxAttaches || _nextId > 255) return null;
    final id = _nextId++;
    _attaches++;
    _virtual[id] = device;
    _send({
      'type': 'usb.attach',
      'device': id,
      'descriptors': base64Encode(descriptors),
      'speed': 'full',
    });
    return id;
  }

  /// Takes a made-up device back; it hears nothing more.
  void detachVirtual(int id) {
    if (_virtual.remove(id) == null) return;
    _send({'type': 'usb.detach', 'device': id, 'reason': 'usb_user_stopped'});
  }

  /// A frame a made-up device answers with.
  void sendVirtualFrame(Uint8List frame) => _sendBytes(frame);

  /// The relay message was about a made-up device: handled here.
  bool _virtualMessage(Map<String, dynamic> value) {
    final device = _virtual[value['device']];
    if (device == null) return false;
    switch (value['type']) {
      case 'usb.attached':
        device.attached();
      case 'usb.rejected' || 'usb.detach':
        _virtual.remove(value['device']);
        device.ended(value['reason'] as String? ?? 'usb_attach_failed');
      default:
        return false;
    }
    return true;
  }

  // --- relay messages -----------------------------------------------------------

  void _receive(dynamic data) {
    if (_closed) return;
    try {
      if (data is String) {
        final value = jsonDecode(data);
        if (value is! Map<String, dynamic>) throw const FormatException();
        _message(value);
      } else if (data is List<int>) {
        final bytes = data is Uint8List ? data : Uint8List.fromList(data);
        final frame = UsbFrame.decode(bytes);
        if (frame == null) throw const FormatException();
        final virtual = _virtual[frame.device];
        if (virtual != null &&
            (frame.type == usbFrameSubmit || frame.type == usbFrameUnlink)) {
          virtual.frame(bytes);
        } else {
          _frame(frame);
        }
      }
    } catch (_) {
      unawaited(_end('usb_invalid_message'));
    }
  }

  void _message(Map<String, dynamic> value) {
    if (_virtualMessage(value)) return;
    switch (value['type']) {
      case 'usb.ready':
        if (value['sessionId'] != sessionId ||
            value['protocol'] != usbProtocol) {
          throw const FormatException();
        }
        _ready = true;
        ProductLog.instance.record(LogEvent.usb, {'stage': 1, 'ok': true});
        unawaited(_loadMemory());
        unawaited(refresh());
        unawaited(_watch());
        _handOvers ??= backend.handOvers.listen(
          (request) => unawaited(_giveTo(request.id, request.window)),
        );
      case 'usb.status':
        _hostReady = value['state'] == 'ready';
        _unavailable = _hostReady
            ? null
            : value['reason'] as String? ?? 'usb_setup_failed';
        if (_unavailable != null) {
          ProductLog.instance.recordFailure(LogEvent.usb, _unavailable, {
            'stage': 2,
            'ok': false,
          });
        }
        _tryResume();
        notifyListeners();
      case 'usb.attached':
        final shared = _shared[value['device']];
        if (shared == null || shared.done) return;
        shared.accepted.start();
        ProductLog.instance.record(LogEvent.usb, {
          'stage': 8,
          'ok': true,
          'usbDevices': sharedCount,
        });
        unawaited(_captured(shared));
      case 'usb.rejected':
        final shared = _shared[value['device']];
        if (shared == null || shared.done) return;
        final code = value['reason'] as String? ?? 'usb_attach_failed';
        ProductLog.instance.recordFailure(LogEvent.usb, code, {
          'stage': 4,
          'ok': false,
        });
        unawaited(_finish(shared, UsbEntryState.refused, code));
      case 'usb.detach':
        final shared = _shared[value['device']];
        if (shared == null || shared.done) return;
        final code = value['reason'] as String? ?? 'usb_vm_detached';
        ProductLog.instance.recordFailure(LogEvent.usb, code, {
          'stage': 3,
          'ok': code == 'usb_user_stopped' || normalChannelEnds.contains(code),
          'usbDevices': sharedCount - 1,
        });
        unawaited(_finish(shared, UsbEntryState.failed, code));
      case 'usb.error':
        unawaited(_end(value['reason'] as String? ?? 'usb_transport_closed'));
      default:
        throw const FormatException();
    }
  }

  /// The VM took the device: now (and only now) it is detached from this PC.
  Future<void> _captured(_Shared shared) async {
    try {
      final pending = backend.capture(shared.entry.device);
      shared.acquiring = pending;
      UsbCapture capture;
      try {
        capture = await pending.timeout(captureTimeout);
      } on TimeoutException {
        // The VM's first requests wait for this capture: do not let them wait
        // for ever. _finish owns this pending capture, gives it back when it
        // finally resolves and blocks a competing hand-over until then.
        await _captureFailed(shared, 'usb_capture_timeout');
        return;
      }
      if (_closed || shared.done) {
        // The cleanup already owns [acquiring], including this late result.
        return;
      }
      shared.capture = capture;
      shared.acquiring = null;
      shared.entry.state = UsbEntryState.shared;
      shared.entry.everShared = true;
      final key = shared.entry.device.memoryKey;
      if (key.isNotEmpty) {
        remembered = {...remembered, usbMemoryHash(key)};
        unawaited(_memory?.remember(key).catchError((Object _) {}));
      }
      shared.captured.complete(true);
      ProductLog.instance.record(LogEvent.usb, {
        'stage': 2,
        'ok': true,
        'usbDevices': sharedCount,
        'elapsedMs': shared.accepted.elapsedMilliseconds,
      });
      unawaited(
        capture.gone.then((_) async {
          if (shared.done || _closed) return;
          _send({
            'type': 'usb.detach',
            'device': shared.id,
            'reason': 'usb_device_removed',
          });
          await _finish(
            shared,
            UsbEntryState.failed,
            'usb_device_removed',
            removed: true,
          );
        }),
      );
      notifyListeners();
    } on UsbBackendException catch (failure) {
      await _captureFailed(shared, failure.code, failure.error);
    } catch (_) {
      await _captureFailed(shared, 'usb_capture_failed');
    }
  }

  Future<void> _captureFailed(
    _Shared shared,
    String code, [
    int error = 0,
  ]) async {
    if (shared.done) return;
    if (!shared.captured.isCompleted) shared.captured.complete(false);
    ProductLog.instance.recordFailure(LogEvent.usb, usbDiagnosticCode(code), {
      'stage': 2,
      'ok': false,
      // UsbDk's Win32 error, so an unknown capture failure can be told apart.
      if (error > 0 && error <= 0xffffffff) 'error': error,
    });
    _send({
      'type': 'usb.detach',
      'device': shared.id,
      'reason': 'usb_device_error',
    });
    await _finish(shared, UsbEntryState.failed, code);
    // Who holds it shows in the list (another window: 이 창으로 옮기기).
    if (code == usbDeviceRedirected && !_closed) unawaited(refresh());
  }

  void _frame(UsbFrame frame) {
    final shared = _shared[frame.device];
    if (shared == null || shared.done) {
      return; // a late request for a device that is gone
    }
    if (frame.type == usbFrameSubmit) {
      unawaited(_submit(shared, frame));
    } else if (frame.type == usbFrameUnlink) {
      final cancelled = shared.inflight.contains(frame.length);
      shared.capture?.cancel(frame.length);
      if (cancelled) {
        shared.cancelled.add(frame.length);
        _watchCancelled(shared, frame.length);
      }
      _sendFrame(
        UsbFrame(
          type: usbFrameUnlinkReturn,
          device: shared.id,
          seq: frame.seq,
          status: cancelled ? usbStatusReset : usbStatusOk,
        ),
      );
    } else {
      throw const FormatException();
    }
  }

  /// A request the VM cancelled that the PC still has not let go of after
  /// [stuckCancel] (a device that ignores the abort): cancel again and answer it
  /// for the VM, so neither the VM nor the relay's window waits for ever. The
  /// late result of the real transfer is dropped.
  void _watchCancelled(_Shared shared, int seq) {
    Timer(stuckCancel, () {
      final request = shared.requests[seq];
      if (_closed ||
          shared.done ||
          request == null ||
          !shared.inflight.contains(seq)) {
        return;
      }
      shared.capture?.cancel(seq);
      shared.abandoned.add(seq);
      shared.inflight.remove(seq);
      shared.requests.remove(seq);
      if (shared.stuckNoted++ < 3) {
        ProductLog.instance.recordFailure(LogEvent.usb, 'usb_request_stuck', {
          'stage': 11,
          'ok': false,
          'usbReq': usbRequestClass(
            request.transfer,
            request.endpoint,
            request.setup,
          ),
        });
      }
      _sendFrame(
        UsbFrame(
          type: usbFrameReturn,
          device: shared.id,
          endpoint: request.endpoint,
          transfer: request.transfer,
          seq: seq,
          status: usbStatusReset,
        ),
      );
    });
  }

  Future<void> _submit(_Shared shared, UsbFrame frame) async {
    shared.inflight.add(frame.seq);
    shared.requests[frame.seq] = frame;
    UsbTransferResult result;
    try {
      // The first requests can arrive while the capture is still being made.
      if (!await shared.captured.future || shared.capture == null) {
        shared.inflight.remove(frame.seq);
        shared.requests.remove(frame.seq);
        return;
      }
      // An unlink can arrive while capture is still pending. Never submit the
      // cancelled request to a newly captured device (an OUT may change it).
      if (_closed || shared.done) return;
      if (shared.abandoned.remove(frame.seq)) {
        shared.cancelled.remove(frame.seq);
        return;
      }
      if (shared.cancelled.remove(frame.seq)) {
        result = UsbTransferResult(usbStatusReset, Uint8List(0));
      } else {
        final isControl = frame.transfer == 0;
        result = await shared.capture!.transfer(
          UsbTransfer(
            token: frame.seq,
            endpoint: frame.endpoint,
            type: frame.transfer,
            length: frame.length,
            setup: isControl ? frame.setup : null,
            data: frame.payload,
          ),
        );
      }
    } catch (_) {
      result = UsbTransferResult(usbStatusIo, Uint8List(0));
    }
    shared.inflight.remove(frame.seq);
    shared.requests.remove(frame.seq);
    shared.cancelled.remove(frame.seq);
    if (shared.abandoned.remove(frame.seq)) {
      return; // already answered for the VM
    }
    if (_closed || shared.done) return;
    final input = frame.endpoint & 0x80 != 0;
    var status = usbWireStatuses.contains(result.status)
        ? result.status
        : usbStatusProtocol;
    var data = result.data;
    if (status != 0 || !input) data = Uint8List(0);
    if (data.length > frame.length) {
      status = usbStatusOverflow;
      data = Uint8List(0);
    }
    _urbs++;
    _bytes += input ? data.length : frame.payload.length;
    _sendFrame(
      UsbFrame(
        type: usbFrameReturn,
        device: shared.id,
        endpoint: frame.endpoint,
        transfer: frame.transfer,
        seq: frame.seq,
        // OUT: the bytes written (all or none); IN: the bytes read.
        length: status != 0 ? 0 : (input ? data.length : frame.length),
        status: status,
        payload: data,
      ),
    );
  }

  // --- ending ---------------------------------------------------------------------------

  Future<void> _finish(
    _Shared shared,
    UsbEntryState state,
    String? code, {
    bool verifyReturn = true,
    bool removed = false,
  }) async {
    if (shared.done) return;
    shared.done = true;
    if (!shared.captured.isCompleted) shared.captured.complete(false);
    final entry = shared.entry;
    final epoch = entry._returnEpoch;
    final arrival = entry.device.arrivalMs;
    final device = entry.device;
    final needsReturn = shared.capture != null || shared.acquiring != null;
    entry._afterReturnState = state;
    entry._afterReturnCode = code;
    _shared.remove(shared.id);
    final capture = shared.capture;
    final acquiring = shared.acquiring;
    shared.capture = null;
    shared.acquiring = null;
    entry
      ..state = capture != null || acquiring != null
          ? UsbEntryState.returning
          : state
      ..code = code;
    if (!_closed) notifyListeners();
    final finishing = () async {
      var owned = capture;
      var stats = const UsbStats();
      var releaseFailed = false;
      try {
        if (owned == null && acquiring != null) owned = await acquiring;
        if (owned != null) {
          for (final token in shared.inflight.toList()) {
            owned.cancel(token);
          }
          stats = await owned.release();
        }
      } catch (_) {
        releaseFailed = owned != null;
      }
      if (owned != null || shared.accepted.isRunning) {
        _recordSummary(shared, code, stats);
      }
      // [removed]: the device left the bus while the desktop had it. There is
      // nothing for Windows to take back, so waiting for its return would
      // only end in "did not return": the reason stays that it was unplugged.
      var returned = removed
          ? 1
          : needsReturn && !releaseFailed
          ? (verifyReturn
                ? await _verifyReturn(entry, device, epoch, arrival)
                : 1)
          : 0;
      if (verifyReturn &&
          !removed &&
          needsReturn &&
          !releaseFailed &&
          !_closed &&
          entry._returnEpoch == epoch &&
          entry.device.arrivalMs != arrival) {
        // Listing may observe a replug on the query lane while the old native
        // release is still pending on the device lane. Its old observation is
        // not readiness evidence for that arrival. Confirm the latest device
        // once, without recapturing; another change stays honestly unknown.
        final currentDevice = entry.device;
        returned = await _verifyReturn(
          entry,
          currentDevice,
          epoch,
          currentDevice.arrivalMs,
        );
      }
      if (entry._returnEpoch == epoch) {
        entry.id = null;
        if (needsReturn) {
          _applyReturnState(entry, returned);
        } else {
          entry.state = state;
        }
      }
      if (!_closed) notifyListeners();
      // The list as Windows has it now: without the unplugged device, or
      // with it as a new arrival when it was plugged in again.
      if (removed && !_closed) unawaited(refresh());
    }();
    _trackCleanup(entry, finishing);
    // Bound the UI/session close wait, not ownership of an unfinished native
    // Start/Stop. A late result still belongs to this cleanup, never a retry.
    try {
      await finishing.timeout(cleanupWait);
    } on TimeoutException {
      /* still returning */
    }
  }

  /// One record per device when it goes back: what the VM asked of it and how
  /// the PC answered, in counts and fixed numbers (usbReq: the first request that
  /// was not a plain success, error: its errno). Never a name, serial or data.
  void _recordSummary(_Shared shared, String? code, UsbStats stats) {
    final values = {
      'stage': 9,
      'ok':
          code == null ||
          code == 'usb_user_stopped' ||
          code == 'usb_session_closed' ||
          normalChannelEnds.contains(code),
      'usbUrbs': stats.urbs,
      'usbKib': stats.kib,
      'usbStalls': stats.stalls,
      'usbFailed': stats.failed,
      'usbTimeouts': stats.timeouts,
      'usbSlow': stats.slow,
      'usbReq': stats.firstClass,
      'error': stats.firstStatus,
      if (stats.firstUsbd != 0) 'usbUsbd': stats.firstUsbd,
      'elapsedMs': shared.accepted.elapsedMilliseconds,
    };
    if (values['ok'] == true) {
      ProductLog.instance.record(LogEvent.usb, values);
    } else {
      ProductLog.instance.recordFailure(
        LogEvent.usb,
        usbDiagnosticCode(code),
        values,
      );
    }
  }

  /// After the capture was released: has Windows taken the device back? If not,
  /// the user is told to unplug and replug it (a device UsbDk gave back that
  /// Windows did not restart stays unusable on the PC).
  Future<int> _verifyReturn(
    UsbEntry entry,
    UsbLocalDevice device,
    int epoch,
    int arrival,
  ) async {
    var state = 0;
    try {
      state = await backend
          .returned(device)
          .timeout(const Duration(seconds: 20));
    } catch (_) {}
    // An old return check must not describe a newer hand-over or replug.
    if (entry._returnEpoch != epoch || entry.device.arrivalMs != arrival) {
      return 0;
    }
    if (state == 2) {
      entry.notReturned = true;
      final missing = backend.returnDetail(device);
      ProductLog.instance.recordFailure(LogEvent.usb, usbNotReturned, {
        'stage': 10,
        'ok': false,
        'usbBack': state,
        if (missing > 0) 'error': missing,
      });
    } else if (state == 1) {
      ProductLog.instance.record(LogEvent.usb, {
        'stage': 10,
        if (state == 1) 'ok': true,
        'usbBack': state,
      });
    } else {
      ProductLog.instance.recordFailure(LogEvent.usb, usbReturnUnknown, {
        'stage': 10,
        'ok': false,
        'usbBack': 0,
      });
    }
    return state;
  }

  void _applyReturnState(UsbEntry entry, int returned) {
    entry
      ..returnUncertain = returned != 1
      ..notReturned = returned == 2
      ..state = returned == 1 ? entry._afterReturnState : UsbEntryState.failed
      ..code = returned == 1
          ? entry._afterReturnCode
          : returned == 2
          ? usbNotReturned
          : usbReturnUnknown;
  }

  /// Read-only recovery check; never offers or captures the device itself.
  Future<void> recheckReturn(UsbEntry entry) async {
    if (_closed ||
        !entry.returnUncertain ||
        entry.state == UsbEntryState.returning) {
      return;
    }
    entry.state = UsbEntryState.returning;
    notifyListeners();
    final epoch = entry._returnEpoch, arrival = entry.device.arrivalMs;
    final returned = await _verifyReturn(entry, entry.device, epoch, arrival);
    if (entry._returnEpoch == epoch) _applyReturnState(entry, returned);
    if (!_closed) notifyListeners();
  }

  /// "PC로 되돌리기" for a device Windows did not take back: UsbDk takes it once
  /// more and gives it straight back, so this PC starts it again as if it were
  /// plugged in (each step makes Windows enumerate it anew), then the return is
  /// checked. Never for a device that is offered, shared or still going back;
  /// a storage device with files open on this PC is refused by the capture's
  /// own check. When it cannot be taken (no longer on the bus, held elsewhere)
  /// only the read-only check runs.
  Future<void> bringBack(UsbEntry entry) async {
    if (_closed ||
        !entry.returnUncertain ||
        entry.state == UsbEntryState.returning ||
        _finishing.containsKey(entry)) {
      return;
    }
    final epoch = entry._returnEpoch;
    entry.state = UsbEntryState.returning;
    notifyListeners();
    final finishing = () async {
      final device = entry.device, arrival = device.arrivalMs;
      var cycled = false, released = true;
      String? code;
      try {
        final capture = await backend.capture(device);
        cycled = true;
        try {
          await capture.release();
        } catch (_) {
          released = false;
        }
      } on UsbBackendException catch (failure) {
        code = failure.code;
      } catch (_) {
        code = 'usb_capture_failed';
      }
      if (cycled) {
        ProductLog.instance.record(LogEvent.usb, {'stage': 14, 'ok': released});
      } else {
        ProductLog.instance.recordFailure(
          LogEvent.usb,
          usbDiagnosticCode(code),
          {'stage': 14, 'ok': false},
        );
      }
      var returned = released
          ? await _verifyReturn(entry, device, epoch, arrival)
          : 0;
      // The cycle itself is a new arrival: confirm the newest one once.
      if (released &&
          !_closed &&
          entry._returnEpoch == epoch &&
          entry.device.arrivalMs != arrival) {
        returned = await _verifyReturn(
          entry,
          entry.device,
          epoch,
          entry.device.arrivalMs,
        );
      }
      if (entry._returnEpoch == epoch) _applyReturnState(entry, returned);
      if (!_closed) notifyListeners();
    }();
    _trackCleanup(entry, finishing);
    await finishing;
  }

  // --- after a stream reconnect ---------------------------------------------------

  /// The devices that were shared when this channel ended: the window offers
  /// them again in the session that replaces this one.
  List<String> sharedAtEnd = const [];

  /// One line about the devices [resume] offered again, and how many such
  /// lines there were (the window shows each once).
  String? resumeNotice;
  bool resumeFailed = false;
  int resumeNotices = 0;
  Set<String> _resume = const {};
  final _resuming = <UsbEntry>{};
  String? _resumeFailure;
  bool _resumedAny = false;

  /// The devices [ids] were shared in the session this one replaces: each that
  /// is still plugged in is offered again once the remote computer can take
  /// devices, through the same checks as a click (the broker and the Host
  /// still decide whether it is allowed). Never silent: [resumeNotice].
  void resume(Iterable<String> ids) {
    if (_closed) return;
    _resume = {..._resume, ...ids};
    _tryResume();
  }

  void _tryResume() {
    if (_resume.isEmpty || _closed) return;
    if (_unavailable != null) {
      _resume = const {};
      _resumeFailure ??= _unavailable;
      _settleResume();
      return;
    }
    if (!ready || listing) return; // the next listing tries again
    final ids = _resume;
    _resume = const {};
    for (final id in ids) {
      final found = entries.where((entry) => entry.device.id == id).toList();
      final refused = found.isEmpty
          ? 'usb_device_removed'
          : share(found.first);
      if (refused != null) {
        _resumeFailure ??= refused;
      } else {
        _resuming.add(found.first);
      }
    }
    _settleResume();
  }

  void _settleResume() {
    for (final entry in _resuming.toList()) {
      switch (entry.state) {
        case UsbEntryState.shared:
          _resumedAny = true;
          _resuming.remove(entry);
        case UsbEntryState.failed || UsbEntryState.refused:
          _resumeFailure ??= entry.code ?? 'usb_attach_failed';
          _resuming.remove(entry);
        case UsbEntryState.available:
          _resuming.remove(entry); // given back meanwhile
        case UsbEntryState.connecting || UsbEntryState.returning:
          break;
      }
    }
    if (_resuming.isNotEmpty || (!_resumedAny && _resumeFailure == null)) {
      return;
    }
    final failure = _resumeFailure;
    resumeFailed = failure != null;
    resumeNotice = failure == null
        ? l10n.usbDeviceReconnected
        : l10n.couldntReconnectUsbDevice(usbMessage(failure));
    resumeNotices++;
    _resumedAny = false;
    _resumeFailure = null;
    if (failure == null) {
      ProductLog.instance.record(LogEvent.usb, {'stage': 15, 'ok': true});
    } else {
      ProductLog.instance.recordFailure(
        LogEvent.usb,
        usbDiagnosticCode(failure),
        {'stage': 15, 'ok': false},
      );
    }
  }

  @override
  void notifyListeners() {
    if (_resuming.isNotEmpty) _settleResume();
    super.notifyListeners();
  }

  /// The channel ended (or the session did): every device goes back to this PC.
  Future<void> _end(String? reason) =>
      _closed ? settled : _ending = _endNow(reason);
  Future<void> _endNow(String? reason) async {
    if (_closed) return;
    // Those still waiting to be offered again count too (a second reconnect).
    sharedAtEnd = {
      for (final shared in _shared.values)
        if (!shared.done && shared.capture != null) shared.entry.device.id,
      ..._resume,
      for (final entry in _resuming) entry.device.id,
    }.toList();
    _closed = true;
    error = reason;
    // Retire connected UI now, even while the native device release is pending.
    notifyListeners();
    _refreshTimer?.cancel();
    final changes = _changes;
    _changes = null;
    unawaited(changes?.cancel());
    final handOvers = _handOvers;
    _handOvers = null;
    unawaited(handOvers?.cancel());
    unawaited(backend.watch(false).catchError((Object _) {}));
    for (final entry in _pending.keys.toList()) {
      unawaited(stop(entry));
    }
    final virtual = Map.of(_virtual);
    _virtual.clear();
    for (final entry in virtual.entries) {
      if (reason == null) {
        _send({
          'type': 'usb.detach',
          'device': entry.key,
          'reason': 'usb_session_closed',
        });
      }
      entry.value.ended(reason ?? 'usb_session_closed');
    }
    final devices = _shared.values.toList();
    if (reason == null) {
      for (final shared in devices) {
        if (!shared.done) {
          _send({
            'type': 'usb.detach',
            'device': shared.id,
            'reason': 'usb_session_closed',
          });
        }
      }
    }
    await Future.wait([
      for (final shared in devices)
        _finish(shared, UsbEntryState.failed, 'usb_session_closed'),
    ]);
    final summary = {
      'stage': 5,
      'ok': reason == null || normalChannelEnds.contains(reason),
      'usbUrbs': _urbs,
      'usbKib': _bytes ~/ 1024,
    };
    if (reason == null) {
      ProductLog.instance.record(LogEvent.usb, summary);
    } else {
      ProductLog.instance.recordFailure(LogEvent.usb, reason, summary);
    }
    final subscription = _subscription;
    _subscription = null;
    unawaited(subscription?.cancel());
    final socket = _socket;
    _socket = null;
    try {
      await socket?.close().timeout(const Duration(seconds: 1));
    } catch (_) {}
    notifyListeners();
  }

  void _send(Map<String, Object?> value) {
    if (_socket == null || _closed && value['reason'] != 'usb_session_closed') {
      return;
    }
    try {
      _socket!.add(jsonEncode(value));
    } catch (_) {}
  }

  void _sendFrame(UsbFrame frame) => _sendBytes(frame.encode());

  void _sendBytes(Uint8List bytes) {
    if (_socket == null || _closed) return;
    try {
      _socket!.add(bytes);
    } catch (_) {}
  }
}

/// The toast when sharing ends while the stream goes on (null: say nothing).
String? usbEndedMessage(UsbSession usb) =>
    !usb.closed || usb.error == null || usb.recovering
    ? null
    : usb.error == 'usb_policy_changed'
    ? l10n.administratorTurnedOffUsbPermission
    : l10n.usbConnectionLost;
