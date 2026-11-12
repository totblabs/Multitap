import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/ui/desktop_tile.dart';

Map<String, dynamic> vm(String os) => {
  'id': 'pve-170',
  'kind': 'vm',
  'name': 'ToTBCode',
  'description': '',
  'os': os,
  'status': 'online',
  'canConnect': false,
};

void main() {
  // The broker's placeholder for a Proxmox-synced VM's unknown OS must not show
  // as a "미확인" badge beside a running Windows VM.
  test('the broker placeholder 미확인 shows the machine kind, not the placeholder', () {
    final machine = machineFromBroker(BrokerDesktop.fromJson(vm('미확인')));
    expect(machine.specification, isEmpty);
    expect(desktopOsLabel(machine), 'Windows VM');
  });
  test('a real OS name is still shown', () {
    final machine = machineFromBroker(BrokerDesktop.fromJson(vm('Windows 11 Pro')));
    expect(desktopOsLabel(machine), 'Windows 11 Pro');
  });
}
