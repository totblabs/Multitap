import '../data/broker_api.dart' show BrokerConnection;
import '../data/usb_redirection.dart';

/// usb-v1 on Android carries only 마우스 잠금's virtual USB mouse
/// (usb_game_mouse.dart): this device's own USB devices are never offered.
UsbSession androidUsbSession(BrokerConnection connection, String token) =>
    UsbSession(
      url: connection.usbWebSocketUrl!,
      token: token,
      sessionId: connection.sessionId,
      backend: const _NoLocalDevices(),
    );

class _NoLocalDevices extends UsbBackend {
  const _NoLocalDevices();
  @override
  Future<UsbEnvironment> environment() async =>
      const UsbEnvironment(UsbDriverState.unsupported);
  @override
  Future<List<UsbLocalDevice>> list() async => const [];
  @override
  Future<UsbCapture> capture(UsbLocalDevice device) async =>
      throw const UsbBackendException('usb_capture_failed');
  @override
  Future<void> installDriver() async =>
      throw const UsbBackendException('usbdk_missing');
}
