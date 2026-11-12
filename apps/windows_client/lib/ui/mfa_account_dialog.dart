import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/mfa_account_controller.dart';
import '../data/mfa_account.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'password_input_scope.dart';
import 'mfa_sensitive_text.dart';
import 'mfa_enrollment_qr.dart';
import '../l10n/l10n.dart';

// Scope touch geometry to MFA actions only. Preserve each existing action's
// colors/shape/tone while overriding desktop density and smaller own minima.
ButtonStyle _mfaActionStyle([ButtonStyle? base]) => (base ?? const ButtonStyle()).copyWith(
  minimumSize: const WidgetStatePropertyAll(Size(48, 48)),
  visualDensity: VisualDensity.standard,
  tapTargetSize: MaterialTapTargetSize.padded,
);

/// The parent owns the controller and the root completion inbox. Removing this
/// auth-scoped route must not dispose an acknowledged recovery-code receipt.
class MfaAccountDialog extends StatefulWidget {
  const MfaAccountDialog({super.key, required this.controller, required this.onClose});
  final MfaAccountController controller;
  final VoidCallback onClose;
  @override
  State<MfaAccountDialog> createState() => _MfaAccountDialogState();
}

class _MfaAccountDialogState extends State<MfaAccountDialog> {
  final _password = TextEditingController(), _factor = TextEditingController();
  bool _recovery = false, _reveal = false;
  Timer? _tick;
  MfaAccountController get c => widget.controller;
  @override
  void initState() {
    super.initState();
    c.addListener(_changed);
    _tick = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted && c.phase == MfaAccountPhase.enrollment) { setState(() {}); }
    });
    unawaited(c.refresh());
  }
  void _changed() {
    if (c.phase != MfaAccountPhase.enrollment) { _reveal = false; }
    if (c.phase == MfaAccountPhase.closed || c.phase == MfaAccountPhase.completed || c.phase == MfaAccountPhase.pendingResult) {
      _password.clear(); _factor.clear();
    }
    if (mounted) { setState(() {}); }
  }
  @override
  void didUpdateWidget(covariant MfaAccountDialog oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.controller, c)) {
      oldWidget.controller.removeListener(_changed); c.addListener(_changed);
      _password.clear(); _factor.clear(); _recovery = false; _reveal = false;
      unawaited(c.refresh());
    }
  }
  void _close() {
    if (c.closeByUser()) { widget.onClose(); }
  }
  Future<void> _submit() async {
    if (c.busy) { return; }
    if (c.phase == MfaAccountPhase.enrollment) {
      final code = _factor.text.trim(); _factor.clear();
      await c.enable(code);
    } else {
      final password = _password.text, factor = _recovery ? _factor.text : _factor.text.trim();
      _password.clear(); _factor.clear();
      await c.submitProof(password,
          code: c.action != MfaAccountAction.enable && !_recovery ? factor : null,
          recoveryCode: c.action != MfaAccountAction.enable && _recovery ? factor : null);
    }
  }
  @override
  void dispose() {
    _tick?.cancel(); c.removeListener(_changed);
    _password.dispose(); _factor.dispose(); super.dispose();
  }
  Widget _text(String value, {bool secondary = false}) => KeepAllText(value,
      style: uiText(size: secondary ? 12.5 : 14, height: 1.5,
          color: secondary ? AppPalette.of(context).muted : AppPalette.of(context).ink));
  Widget _passwordField() => AppField(label: l10n.currentPassword, child: PasswordInputScope(builder: (focus) => TextField(
    key: const Key('mfa-account-password'), controller: _password, focusNode: focus,
    enabled: !c.busy, obscureText: true, enableSuggestions: false, autocorrect: false, enableIMEPersonalizedLearning: false,
    keyboardType: TextInputType.visiblePassword, maxLength: 256,
    decoration: AppField.decoration(context),
    onSubmitted: (_) => unawaited(_submit()),
  )));
  Widget _factorField({required bool enrollment}) => AppField(
    label: !enrollment && _recovery ? l10n.recoveryCode : l10n.n6DigitCodeFromAuthenticator, child: TextField(
    key: const Key('mfa-account-factor'), controller: _factor, enabled: !c.busy,
    enableSuggestions: false, autocorrect: false, enableIMEPersonalizedLearning: false,
    obscureText: !enrollment && _recovery,
    keyboardType: !enrollment && _recovery ? TextInputType.visiblePassword : TextInputType.number,
    inputFormatters: !enrollment && _recovery ? [] : [FilteringTextInputFormatter.digitsOnly],
    maxLength: !enrollment && _recovery ? 39 : 6,
    decoration: AppField.decoration(context),
    onSubmitted: (_) => unawaited(_submit()),
  ));
  List<Widget> _body() {
    switch (c.phase) {
      case MfaAccountPhase.unavailable:
        return [_text(c.reading ? l10n.checkingSetupStatus : l10n.couldNotCheckSetupStatus),
          const SizedBox(height: 12), OutlinedButton(
            key: const Key('mfa-account-refresh'), onPressed: c.reading ? null : () => unawaited(c.refresh()),
            style: _mfaActionStyle(),
            child: KeepAllText(l10n.checkAgain))];
      case MfaAccountPhase.summary:
        final enabled = c.status!.enabled;
        return [_text(enabled ? l10n.twoStepVerification2 : l10n.twoStepVerificationOff),
          if (enabled) _text(l10n.text23(c.status!.recoveryCodesRemaining), secondary: true),
          const SizedBox(height: 20), _text(l10n.changingSignsYouOutAll2, secondary: true),
          const SizedBox(height: 12),
          if (!enabled) FilledButton(key: const Key('mfa-account-enable'),
            onPressed: () => c.choose(MfaAccountAction.enable), style: _mfaActionStyle(AppButtons.filled(AppPalette.of(context))),
            child: KeepAllText(l10n.setUpAuthenticatorApp)),
          if (enabled) ...[
            OutlinedButton(key: const Key('mfa-account-regenerate'), onPressed: () => c.choose(MfaAccountAction.regenerate),
              style: _mfaActionStyle(),
              child: KeepAllText(l10n.makeNewRecoveryCodes)),
            const SizedBox(height: 8), TextButton(key: const Key('mfa-account-disable'),
              onPressed: () => c.choose(MfaAccountAction.disable),
              style: _mfaActionStyle(),
              child: KeepAllText(l10n.turnOffTwoStepVerification)),
          ]];
      case MfaAccountPhase.proof:
        return [_text(c.action == MfaAccountAction.enable ? l10n.n13VerifyItsYou : l10n.verifyItsYou),
          const SizedBox(height: 12), _passwordField(),
          if (c.action != MfaAccountAction.enable) ...[
            const SizedBox(height: 12), _factorField(enrollment: false),
            _text(l10n.enterNextCodeFromAuthenticator, secondary: true),
            TextButton(onPressed: c.busy ? null : () { _factor.clear(); setState(() => _recovery = !_recovery); },
              style: _mfaActionStyle(),
              child: KeepAllText(_recovery ? l10n.useAuthenticatorCode : l10n.useRecoveryCode)),
          ],
          const SizedBox(height: 12), _text(c.action == MfaAccountAction.regenerate
              ? l10n.makingNewCodesStopsOld
              : l10n.whenChangeDoneYouSigned, secondary: true)];
      case MfaAccountPhase.enrollment:
        final controller = c, setup = c.enrollment!;
        return [_text(l10n.n23LinkAuthenticatorApp),
          const SizedBox(height: 12), _text(l10n.scanQrCodeAuthenticatorApp),
          TextButton(key: const Key('mfa-account-reveal'), onPressed: c.busy ? null : () => setState(() => _reveal = !_reveal),
              style: _mfaActionStyle(),
              child: KeepAllText(_reveal ? l10n.hideQrSetupKey : l10n.showQrSetupKey)),
          if (_reveal && controller.canDisplayEnrollment(setup)) ...[
            MfaEnrollmentQr(key: const Key('mfa-account-enrollment-qr'),
              controller: controller, enrollment: setup,
              visible: () => mounted && _reveal && identical(widget.controller, controller)),
            const SizedBox(height: 12),
            MfaSensitiveText(setup.secret, key: const Key('mfa-account-secret'),
              binding: setup, canCopy: () => mounted && _reveal && identical(widget.controller, controller) && controller.canCopyEnrollment(setup),
              style: uiText(size: 15)),
          ],
          // Rounded up: the last second reads 1, never 0 while time is left.
          _text(l10n.timeLeftSYouCan((setup.remaining.inMilliseconds / 1000).ceil()), secondary: true),
          const SizedBox(height: 16), _factorField(enrollment: true)];
      case MfaAccountPhase.pendingResult:
        return [KeepAllText(l10n.setupKeyDisplayTimeEnded,
          key: const Key('mfa-account-pending-result'), style: uiText(size: 14, height: 1.5, color: AppPalette.of(context).ink)),
          const SizedBox(height: 10), _text(l10n.waitingResultSetupRequestAlready, secondary: true)];
      case MfaAccountPhase.uncertain:
        return [_text(l10n.changeRequestsNotSentAgain),
          const SizedBox(height: 10), _text(l10n.signAgainThenCheckTwo, secondary: true)];
      case MfaAccountPhase.completed:
        return [_text(l10n.settingsChangedSignAgain)];
      case MfaAccountPhase.closed:
        return [_text(l10n.signChangedSoSetupWas)];
    }
  }
  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context), editing = c.phase == MfaAccountPhase.proof || c.phase == MfaAccountPhase.enrollment;
    return PopScope(canPop: false, onPopInvokedWithResult: (didPop, result) { if (!didPop) { _close(); } },
      child: AppDialog(icon: Icons.security_rounded, busy: c.busy, title: l10n.twoStepVerification,
        subtitle: c.owner.username, maxWidth: 420,
        body: [..._body(), if (c.error != null) Padding(padding: const EdgeInsets.only(top: 12),
          child: AppStatusLine(c.error!, textKey: const Key('mfa-account-error')))],
        actions: [TextButton(key: const Key('mfa-account-close'), onPressed: c.mutating ? null : _close,
            style: _mfaActionStyle(AppButtons.quiet(p)), child: KeepAllText(l10n.close)),
          if (editing) FilledButton(key: const Key('mfa-account-submit'), onPressed: c.busy ? null : () => unawaited(_submit()),
              style: _mfaActionStyle(AppButtons.filled(p, tone: c.action == MfaAccountAction.disable ? AppTone.danger : AppTone.accent)),
              child: KeepAllText(c.busy ? l10n.checking2 : c.phase == MfaAccountPhase.enrollment ? l10n.finishSetup : l10n.verifyItsYou))]));
  }
}

/// Mount this from a ROOT overlay above the authentication boundary. It has no
/// bearer, never acknowledges native cleanup and never starts a login itself.
class MfaCompletedRecoveryDialog extends StatefulWidget {
  const MfaCompletedRecoveryDialog({super.key, required this.inbox, required this.onFinished, this.canCopy});
  final MfaCompletionInbox inbox;
  final VoidCallback onFinished;
  final bool Function(MfaCompletedOperation receipt)? canCopy;
  @override
  State<MfaCompletedRecoveryDialog> createState() => _MfaCompletedRecoveryDialogState();
}
class _MfaCompletedRecoveryDialogState extends State<MfaCompletedRecoveryDialog> {
  bool _saved = false;
  MfaCompletedOperation? _shown;
  @override
  void initState() { super.initState(); widget.inbox.addListener(_changed); }
  @override
  void didUpdateWidget(covariant MfaCompletedRecoveryDialog oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.inbox, widget.inbox)) {
      oldWidget.inbox.removeListener(_changed); widget.inbox.addListener(_changed);
      _shown = null; _saved = false;
    }
  }
  void _changed() { if (mounted) { setState(() {}); } }
  @override
  void dispose() { widget.inbox.removeListener(_changed); super.dispose(); }
  bool _canCopy(MfaCompletedOperation receipt) {
    if (!mounted || !identical(widget.inbox.completed, receipt) || !receipt.owner.current) { return false; }
    return (widget.canCopy?.call(receipt) ?? true) &&
        identical(widget.inbox.completed, receipt) && receipt.owner.current;
  }
  Future<void> _discard() async {
    final receipt = widget.inbox.completed;
    final yes = await showDialog<bool>(context: context, builder: (context) => AlertDialog(
      title: Text(l10n.closeRecoveryCodes), content: Text(l10n.youCannotSeeTheseCodes),
      actions: [TextButton(onPressed: () => Navigator.pop(context, false), style: _mfaActionStyle(), child: Text(l10n.goBack)),
        TextButton(onPressed: () => Navigator.pop(context, true), style: _mfaActionStyle(), child: Text(l10n.closeWithoutSaving2))]));
    if (!mounted || yes != true || !identical(receipt, widget.inbox.completed)) { return; }
    widget.inbox.discard(); widget.onFinished();
  }
  @override
  Widget build(BuildContext context) {
    final inbox = widget.inbox;
    final receipt = inbox.completed;
    if (receipt == null) { return const SizedBox.shrink(); }
    if (!identical(_shown, receipt)) { _shown = receipt; _saved = false; }
    bool currentConsent() {
      if (!mounted || !identical(widget.inbox, inbox) || !identical(_shown, receipt)) { return false; }
      if (!identical(inbox.completed, receipt) || !receipt.owner.current) { return false; }
      // Recheck identity after the owner's current predicate; no auth gate.
      return mounted && identical(widget.inbox, inbox) && identical(_shown, receipt) &&
          identical(inbox.completed, receipt);
    }
    final p = AppPalette.of(context), codes = receipt.codes;
    return PopScope(canPop: false, child: AppDialog(icon: Icons.verified_user_outlined,
      title: codes.isEmpty ? l10n.twoStepVerificationOff2 : l10n.saveRecoveryCodes,
      subtitle: receipt.owner.username, maxWidth: 420,
      body: [
        // Setting it up ends here: the third of the steps the dialog counted.
        if (receipt.action == MfaAccountAction.enable && codes.isNotEmpty) ...[
          KeepAllText(l10n.n33SaveRecoveryCodes, key: const Key('mfa-completed-step'), style: uiText(size: 14, height: 1.5, color: p.ink)),
          const SizedBox(height: 12),
        ],
        KeepAllText(l10n.youSignedOutAllDevices, style: uiText(size: 14, height: 1.5, color: p.ink)),
        if (codes.isNotEmpty) ...[
          const SizedBox(height: 16), KeepAllText(l10n.eachCodeWorksOnlyOnce, style: uiText(size: 12.5, height: 1.5, color: p.muted)),
          const SizedBox(height: 12), MfaSensitiveText(codes.map((code) => [for (var i = 0; i < 32; i += 4) code.substring(i, i + 4)].join('-')).join('\n'),
            key: const Key('mfa-completed-codes'), binding: receipt, canCopy: () => _canCopy(receipt), style: uiText(size: 13, height: 1.7, color: p.ink)),
          const SizedBox(height: 12), CheckboxListTile(key: const Key('mfa-completed-saved'), contentPadding: EdgeInsets.zero,
            dense: false, visualDensity: VisualDensity.standard,
            title: KeepAllText(l10n.iSavedRecoveryCodesSomewhere), value: _saved, onChanged: (value) {
              if (!currentConsent()) { return; }
              setState(() => _saved = value == true);
            }),
        ]],
      actions: [if (codes.isNotEmpty) TextButton(key: const Key('mfa-completed-discard'), onPressed: () => unawaited(_discard()),
          style: _mfaActionStyle(AppButtons.quiet(p)), child: KeepAllText(l10n.closeWithoutSaving2)),
        FilledButton(key: const Key('mfa-completed-done'), onPressed: codes.isNotEmpty && !_saved ? null : () {
          if (!currentConsent()) { return; }
          if (inbox.acknowledgeSaved(saved: _saved)) { widget.onFinished(); }
        }, style: _mfaActionStyle(AppButtons.filled(p)), child: KeepAllText(l10n.ok))]));
  }
}
