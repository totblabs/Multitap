import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_overview.dart';

// A desktop a client can stream now: every fact as a healthy Host reports it.
HostFacts facts({
  bool loaded = true,
  bool readFailed = false,
  bool? serviceInstalled = true,
  bool? serviceRunning = true,
  bool displayMissing = false,
  bool displayBlocked = false,
  bool restartRequired = false,
  bool recoveryRequired = false,
  bool registered = true,
  bool awaitingApproval = false,
  bool cancellingConnection = false,
  bool brokerOnline = true,
  bool interactive = true,
  bool displayReady = true,
  bool transferEligible = false,
  bool displayPreparing = false,
  bool loginSetup = false,
  bool loginAttention = false,
  bool winfspMissing = false,
  bool devicesAttention = false,
}) => HostFacts(
  loaded: loaded,
  readFailed: readFailed,
  serviceInstalled: serviceInstalled,
  serviceRunning: serviceRunning,
  displayMissing: displayMissing,
  displayBlocked: displayBlocked,
  restartRequired: restartRequired,
  recoveryRequired: recoveryRequired,
  registered: registered,
  awaitingApproval: awaitingApproval,
  cancellingConnection: cancellingConnection,
  brokerOnline: brokerOnline,
  interactive: interactive,
  displayReady: displayReady,
  transferEligible: transferEligible,
  displayPreparing: displayPreparing,
  loginSetup: loginSetup,
  loginAttention: loginAttention,
  winfspMissing: winfspMissing,
  devicesAttention: devicesAttention,
);

void main() {
  test('The priority of problems is the order of the enum, written out', () {
    // What Windows itself needs, then the route in menu order, then what
    // does not stop a stream. Changing this order changes what the overview
    // tells the user to do first.
    expect(HostProblem.values, [
      HostProblem.statusUnread,
      HostProblem.serviceMissing,
      HostProblem.serviceStopped,
      HostProblem.serviceUnknown,
      HostProblem.installUnfinished,
      HostProblem.installBlocked,
      HostProblem.restartRequired,
      HostProblem.displayRecovery,
      HostProblem.serverUnregistered,
      HostProblem.serverApproval,
      HostProblem.serverOffline,
      HostProblem.windowsSignedOut,
      HostProblem.displayTransfer,
      HostProblem.displayPreparing,
      HostProblem.displayNotReady,
      HostProblem.loginSetup,
      HostProblem.loginAttention,
      HostProblem.winfspMissing,
      HostProblem.devicesAttention,
    ]);
    for (final problem in HostProblem.values) {
      expect(
        problem.blocksStreaming,
        problem.index <= HostProblem.displayNotReady.index,
        reason: problem.name,
      );
      expect(problem.label, isNotEmpty);
      expect(const [1, 2, 4, 5, 6], contains(problem.page));
    }
  });

  test('A healthy desktop has no problem and can be streamed', () {
    expect(hostProblems(facts()), isEmpty);
    expect(hostMainProblem(facts()), isNull);
    expect(hostStreamable(facts()), isTrue);
  });

  test('Nothing is reported before the first status; a failed read is', () {
    expect(hostMainProblem(const HostFacts()), isNull);
    expect(hostStreamable(const HostFacts()), isFalse);
    expect(
      hostMainProblem(const HostFacts(readFailed: true)),
      HostProblem.statusUnread,
    );
    // Facts of an unread status are never judged.
    expect(hostProblems(facts(loaded: false, serviceRunning: false)), isEmpty);
  });

  test('Each single fault is the one problem it names', () {
    final cases = <HostProblem, HostFacts>{
      HostProblem.serviceMissing: facts(
        serviceInstalled: false,
        serviceRunning: false,
        brokerOnline: false,
      ),
      HostProblem.serviceStopped: facts(
        serviceRunning: false,
        brokerOnline: false,
      ),
      HostProblem.serviceUnknown: facts(
        serviceInstalled: null,
        serviceRunning: null,
        brokerOnline: false,
      ),
      HostProblem.installUnfinished: facts(
        displayMissing: true,
        displayReady: false,
      ),
      HostProblem.installBlocked: facts(
        displayBlocked: true,
        displayReady: false,
      ),
      HostProblem.restartRequired: facts(
        restartRequired: true,
        displayReady: false,
      ),
      HostProblem.displayRecovery: facts(
        recoveryRequired: true,
        displayReady: false,
      ),
      HostProblem.serverUnregistered: facts(
        registered: false,
        brokerOnline: false,
      ),
      HostProblem.serverApproval: facts(
        registered: false,
        awaitingApproval: true,
        brokerOnline: false,
      ),
      HostProblem.serverOffline: facts(brokerOnline: false),
      HostProblem.windowsSignedOut: facts(interactive: false),
      HostProblem.displayTransfer: facts(
        displayReady: false,
        transferEligible: true,
      ),
      HostProblem.displayPreparing: facts(
        displayReady: false,
        displayPreparing: true,
      ),
      HostProblem.displayNotReady: facts(displayReady: false),
      HostProblem.loginSetup: facts(loginSetup: true),
      HostProblem.loginAttention: facts(loginAttention: true),
      HostProblem.winfspMissing: facts(winfspMissing: true),
      HostProblem.devicesAttention: facts(devicesAttention: true),
    };
    // Every problem but the unread status has a case here.
    expect(cases.keys.toSet(), {
      ...HostProblem.values,
    }..remove(HostProblem.statusUnread));
    for (final MapEntry(key: problem, value: fault) in cases.entries) {
      expect(hostMainProblem(fault), problem, reason: problem.name);
      expect(
        hostStreamable(fault),
        !problem.blocksStreaming,
        reason: problem.name,
      );
    }
  });

  test('With several faults the most important one is shown', () {
    // A fresh install: nothing is there yet. The service comes first.
    final everything = facts(
      serviceRunning: false,
      displayMissing: true,
      restartRequired: true,
      recoveryRequired: true,
      registered: false,
      brokerOnline: false,
      interactive: false,
      displayReady: false,
      loginSetup: true,
      winfspMissing: true,
      devicesAttention: true,
    );
    expect(hostProblems(everything), [
      HostProblem.serviceStopped,
      HostProblem.installUnfinished,
      HostProblem.restartRequired,
      HostProblem.displayRecovery,
      HostProblem.serverUnregistered,
      HostProblem.windowsSignedOut,
      HostProblem.loginSetup,
      HostProblem.winfspMissing,
      HostProblem.devicesAttention,
    ]);
    // Pairwise, in the order the overview promises.
    expect(
      hostMainProblem(
        facts(displayMissing: true, displayReady: false, registered: false),
      ),
      HostProblem.installUnfinished,
    );
    expect(
      hostMainProblem(
        facts(restartRequired: true, displayReady: false, brokerOnline: false),
      ),
      HostProblem.restartRequired,
    );
    expect(
      hostMainProblem(facts(brokerOnline: false, interactive: false)),
      HostProblem.serverOffline,
    );
    expect(
      hostMainProblem(facts(interactive: false, displayReady: false)),
      HostProblem.windowsSignedOut,
    );
    expect(
      hostMainProblem(facts(displayReady: false, loginAttention: true)),
      HostProblem.displayNotReady,
    );
    expect(
      hostMainProblem(facts(loginSetup: true, devicesAttention: true)),
      HostProblem.loginSetup,
    );
    // A request being cancelled is still the server's turn.
    expect(
      hostMainProblem(
        facts(registered: false, cancellingConnection: true, brokerOnline: false),
      ),
      HostProblem.serverApproval,
    );
    // A locked Windows is the Windows sign-in's problem, not the screen's.
    expect(
      hostProblems(facts(interactive: false, displayReady: false)),
      [HostProblem.windowsSignedOut],
    );
    // One display fault is named once: the restart, not "not ready" as well.
    expect(
      hostProblems(facts(restartRequired: true, displayReady: false)),
      [HostProblem.restartRequired],
    );
  });

  test('The route marks the place that is broken and nothing else', () {
    List<HostRouteState> states(HostFacts f) {
      final r = hostRoute(f);
      return [r.desktop, r.toServer, r.server, r.toClient, r.client];
    }

    const ok = HostRouteState.ok,
        warn = HostRouteState.warn,
        idle = HostRouteState.idle;
    // All fine: three green points, two green legs.
    expect(states(facts()), [ok, ok, ok, ok, ok]);
    // What does not stop a stream leaves the route whole.
    for (final fault in [
      facts(loginSetup: true),
      facts(loginAttention: true),
      facts(winfspMissing: true),
      facts(devicesAttention: true),
    ]) {
      expect(states(fault), [ok, ok, ok, ok, ok]);
    }
    // Nothing read yet: nothing is claimed.
    expect(states(const HostFacts()), [idle, idle, idle, idle, idle]);
    expect(states(const HostFacts(readFailed: true)), [idle, idle, idle, idle, idle]);
    // Windows signed out: the desktop is the broken place; the server is still fine.
    expect(states(facts(interactive: false)), [warn, warn, ok, idle, idle]);
    // The server side: not registered, waiting for approval, not connected.
    for (final fault in [
      facts(registered: false, brokerOnline: false),
      facts(registered: false, awaitingApproval: true, brokerOnline: false),
      facts(brokerOnline: false),
    ]) {
      expect(states(fault), [ok, warn, warn, idle, idle]);
    }
    // Both ends at once.
    expect(states(facts(displayReady: false, brokerOnline: false)), [warn, warn, warn, idle, idle]);
    // With the service off the Host cannot know the server: no claim.
    expect(states(facts(serviceRunning: false, brokerOnline: false)), [warn, warn, idle, idle, idle]);
    // The client end is never a problem of its own: the settings status says nothing about clients.
    for (final fault in [
      facts(interactive: false),
      facts(brokerOnline: false),
      facts(serviceRunning: false, brokerOnline: false),
      facts(restartRequired: true, displayReady: false),
    ]) {
      expect(hostRoute(fault).client, isNot(warn));
      expect(hostRoute(fault).toClient, isNot(warn));
    }
  });
  test('What does not stop a stream keeps the desktop streamable', () {
    for (final fault in [
      facts(loginSetup: true),
      facts(loginAttention: true),
      facts(winfspMissing: true),
      facts(devicesAttention: true),
    ]) {
      expect(hostStreamable(fault), isTrue);
      expect(hostMainProblem(fault)!.blocksStreaming, isFalse);
    }
    // Nothing read yet, or a blocking problem: never streamable.
    for (final fault in [
      const HostFacts(),
      const HostFacts(readFailed: true),
      facts(interactive: false),
      facts(brokerOnline: false),
      facts(serviceRunning: false, brokerOnline: false),
      facts(restartRequired: true, displayReady: false),
    ]) {
      expect(hostStreamable(fault), isFalse);
    }
  });

  test('The machine kind comes only from the id the server gave', () {
    expect(hostMachineKind('pve-173'), HostMachineKind.vm);
    expect(hostMachineKind('pve-totbvm-149'), HostMachineKind.vm);
    expect(
      hostMachineKind('pc-0a1b2c3d-1111-4222-8333-444455556666'),
      HostMachineKind.pc,
    );
    for (final other in ['', 'pc-', 'pc-office', 'desk-1', 'pve-', null, 7]) {
      expect(hostMachineKind(other), HostMachineKind.unknown, reason: '$other');
    }
    expect(hostMachineLine('pve-173', named: true), 'Proxmox VM · pve-173');
    expect(hostMachineLine('pve-173', named: false), 'Proxmox VM');
    // A local PC is never called a VM, and its UUID is not shown.
    for (final named in [true, false]) {
      final line = hostMachineLine(
        'pc-0a1b2c3d-1111-4222-8333-444455556666',
        named: named,
      );
      expect(line, '로컬 PC');
      expect(line, isNot(contains('VM')));
    }
    // An id that says nothing: no kind is claimed.
    expect(hostMachineLine('desk-1', named: true), '서버에 등록된 데스크톱 · desk-1');
    expect(hostMachineLine('desk-1', named: false), '서버에 등록된 데스크톱');
    expect(hostMachineLine(null, named: true), '서버에 등록된 데스크톱');
  });

  test('The server is named by its host; the acknowledgement by its age', () {
    expect(hostServerName('https://vdi.example.com'), 'vdi.example.com');
    expect(hostServerName('https://vdi.example.com:443'), 'vdi.example.com');
    expect(hostServerName('https://vdi.example.com:8443'), 'vdi.example.com:8443');
    expect(hostServerName('http://10.0.0.5:18480'), '10.0.0.5:18480');
    for (final other in ['', 'vdi.example.com', 'ftp://x', null, 3]) {
      expect(hostServerName(other), '', reason: '$other');
    }
    expect(hostAckWords(0), '방금 확인');
    expect(hostAckWords(9.9), '방금 확인');
    expect(hostAckWords(10), '10초 전 확인');
    expect(hostAckWords(29.7), '29초 전 확인');
  });
}
