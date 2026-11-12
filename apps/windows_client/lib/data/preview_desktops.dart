import 'package:flutter/material.dart';

import '../models/desktop_machine.dart';
import '../models/vm_resources.dart';
import '../l10n/l10n.dart';

// Explicit sample data. Never use these records as an authorization decision.
// One list per language: the same records on every rebuild.
final _previewDesktops = <String, List<DesktopMachine>>{};
List<DesktopMachine> get previewDesktops =>
    _previewDesktops[AppLanguageController.instance.code] ??= _buildPreviewDesktops();

List<DesktopMachine> _buildPreviewDesktops() => [
  DesktopMachine(
    id: 'sample-office',
    name: l10n.myDesktop,
    description: l10n.samplePcServerRelay,
    kind: MachineKind.pc,
    status: MachineStatus.ready,
    specification: l10n.windows11PhysicalPc,
    accent: Color(0xFF3478F6),
  ),
  DesktopMachine(
    id: 'sample-proxmox',
    name: l10n.proxmoxWorkspace,
    description: l10n.lightweightVirtualDesktopStart,
    kind: MachineKind.vm,
    status: MachineStatus.ready,
    specification: 'Windows 11 · 4 vCPU · 8 GB',
    accent: Color(0xFF6371D9),
    powerState: 'running',
    powerStale: false,
    // Sample usage for the design demo, not a measurement.
    resources: VmResources(
      cpuUsagePercent: 18,
      vcpus: 4,
      memoryUsedBytes: 4724464025,
      memoryTotalBytes: 8589934592,
      stale: false,
    ),
  ),
  DesktopMachine(
    id: 'sample-lab',
    name: l10n.devDesktop,
    description: l10n.spaceNewIdeas,
    kind: MachineKind.vm,
    status: MachineStatus.poweredOff,
    specification: 'Windows 11 · 8 vCPU · 16 GB',
    accent: Color(0xFF238F98),
    powerState: 'stopped',
    powerStale: false,
    resources: VmResources(vcpus: 8, memoryTotalBytes: 17179869184),
  ),
];
