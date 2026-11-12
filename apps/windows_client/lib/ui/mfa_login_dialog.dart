import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/broker_api.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'session_access_boundary.dart' show confirmManualLogin;
import '../l10n/l10n.dart';

// Only a typed proof and the non-secret submitted method survive this step.
class _MfaConsentContinuation {
  const _MfaConsentContinuation(this.confirmation, {
    required this.recoveryCodeConsumed,
  });
  final LoginConfirmationRequired confirmation;
  final bool recoveryCodeConsumed;
}

Future<BrokerSession?> showMfaLogin(
  BuildContext context, {
  required BrokerApi api,
  required MfaChallenge challenge,
  required bool Function() current,
}) async {
  if (api is! MfaBrokerApi) {
    challenge.forget();
    throw const BrokerException('mfa_client_update_required');
  }
  final result = await showDialog<Object>(
    context: context,
    barrierDismissible: false,
    builder: (_) =>
        MfaLoginDialog(api: api, challenge: challenge, current: current),
  );
  if (result is _MfaConsentContinuation) {
    if (!context.mounted ||
        !current() ||
        challenge.pcLoginIntent != PcLoginIntent.confirmable) {
      result.confirmation.challenge.forget();
      return null;
    }
    // The factor is complete. Reuse the existing fresh replacement dialog
    // using this exact proof; no password or OTP request is replayed.
    final confirmed = await confirmManualLogin(
      context,
      api: api,
      current: current,
      recoveryCodeConsumed: result.recoveryCodeConsumed,
      attempt: () => Future<BrokerSession>.error(result.confirmation),
    );
    if (confirmed != null && (!context.mounted || !current())) {
      unawaited(api.logout(confirmed.token).catchError((Object _) {}));
      return null;
    }
    return confirmed;
  }
  if (result is BrokerSession) {
    if (!context.mounted || !current()) {
      unawaited(api.logout(result.token).catchError((Object _) {}));
      return null;
    }
    return result;
  }
  return null;
}

/// This dialog owns only pending authentication. Late successful verification
/// after back/cancel is logged out; it cannot enter the launcher's session.
class MfaLoginDialog extends StatefulWidget {
  const MfaLoginDialog({
    super.key,
    required this.api,
    required this.challenge,
    required this.current,
  });
  final BrokerApi api;
  final MfaChallenge challenge;
  final bool Function() current;
  @override
  State<MfaLoginDialog> createState() => _MfaLoginDialogState();
}

class _MfaLoginDialogState extends State<MfaLoginDialog> {
  final _code = TextEditingController();
  final _codeFocus = FocusNode();
  late final Duration _initial = widget.challenge.remaining;
  late Duration _remaining = _initial;
  final _age = Stopwatch()..start();
  Timer? _timer;
  bool _busy = false, _closed = false, _expired = false;
  bool _freshLoginRequired = false;
  late bool _recovery = !widget.challenge.methods.contains('totp');
  int _request = 0;
  String? _error;

  @override
  void initState() {
    super.initState();
    _expired = _initial == Duration.zero;
    if (_expired) widget.challenge.forget();
    _timer = Timer.periodic(const Duration(seconds: 1), (timer) {
      if (!mounted || _closed) return;
      if (!widget.current()) {
        final route = ModalRoute.of(context);
        _retire();
        if (route != null && route.isActive) {
          Navigator.of(context).removeRoute(route);
        }
        return;
      }
      final ticks = Duration(seconds: timer.tick);
      final elapsed = ticks > _age.elapsed ? ticks : _age.elapsed;
      final bounded = _initial - elapsed;
      final actual = widget.challenge.remaining;
      final next = bounded < actual ? bounded : actual;
      setState(() {
        _remaining = next.isNegative ? Duration.zero : next;
        if (_remaining == Duration.zero) {
          _expired = true;
          _code.clear();
          widget.challenge.forget();
          _error = null;
          timer.cancel();
        }
      });
    });
  }

  void _retire() {
    _closed = true;
    ++_request;
    _timer?.cancel();
    _code.clear();
    widget.challenge.forget();
  }

  void _cancel() {
    _retire();
    Navigator.pop(context);
  }

  Future<void> _submit() async {
    if (_busy || _expired || _closed || !widget.current()) return;
    final recovery = _recovery;
    final value = recovery ? _code.text : _code.text.trim();
    final recoveryCode = recovery ? normalizeMfaRecoveryCode(value) : null;
    if ((!recovery && !RegExp(r'^\d{6}$').hasMatch(value)) ||
        (recovery && recoveryCode == null)) {
      setState(
        () => _error = _recovery ? l10n.enterRecoveryCodeYouSaved : l10n.enter6DigitCode,
      );
      return;
    }
    final request = ++_request;
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final session = await (widget.api as MfaBrokerApi).verifyMfa(
        widget.challenge,
        code: recovery ? null : value,
        recoveryCode: recoveryCode,
      );
      if (!mounted || _closed || request != _request || !widget.current()) {
        unawaited(widget.api.logout(session.token).catchError((Object _) {}));
        return;
      }
      _retire();
      if (mounted) Navigator.pop(context, session);
    } on LoginConfirmationRequired catch (required) {
      if (!mounted || _closed || request != _request || !widget.current()) {
        required.challenge.forget();
        widget.challenge.forget();
        return;
      }
      _retire();
      Navigator.pop(context, _MfaConsentContinuation(
        required,
        recoveryCodeConsumed: recoveryCode != null,
      ));
    } on BrokerException catch (error) {
      if (mounted && !_closed && request == _request && widget.current()) {
        setState(() {
          _code.clear();
          _freshLoginRequired = error is MfaChallengeRetiredException;
          _expired = _expired || _freshLoginRequired ||
              error.code == 'mfa_challenge_expired';
          if (_expired) {
            widget.challenge.forget();
            _timer?.cancel();
          }
          _error = _freshLoginRequired ? null : error.message;
        });
      }
    } catch (_) {
      if (mounted && !_closed && request == _request) {
        setState(() {
          _code.clear();
          _error = l10n.couldNotConfirmResultTry;
        });
      }
    } finally {
      if (mounted && !_closed && request == _request) {
        setState(() => _busy = false);
        // The field was disabled while checking and lost the focus: the
        // next code is typed without another click.
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (mounted && !_closed && !_expired) _codeFocus.requestFocus();
        });
      }
    }
  }

  @override
  void dispose() {
    _retire();
    _codeFocus.dispose();
    _code.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final seconds = (_remaining.inMilliseconds / 1000).ceil();
    return PopScope<Object>(
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) _retire();
      },
      child: AppDialog(
        title: l10n.twoStepVerification,
        titleKey: const Key('mfa-title'),
        subtitle: widget.challenge.origin.host,
        icon: Icons.verified_user_outlined,
        onClose: _cancel,
        closeKey: const Key('mfa-close'),
        maxWidth: 440,
        body: [
          KeepAllText(
            _freshLoginRequired
                ? l10n.couldNotConfirmResultStart
                : _expired
                ? l10n.verificationTimeRanOutStart
                : _recovery
                ? l10n.enterRecoveryCodeEachOne
                : l10n.enter6DigitCodeFrom,
            key: _freshLoginRequired
                ? const Key('mfa-fresh-login-required')
                : null,
            style: uiText(
              size: 14,
              weight: FontWeight.w500,
              color: p.ink,
              height: 1.6,
            ),
          ),
          const SizedBox(height: 12),
          if (!_expired) ...[
            Text(
              l10n.timeLeft(seconds ~/ 60, (seconds % 60).toString().padLeft(2, '0')),
              key: const Key('mfa-remaining'),
              style: uiText(size: 13, weight: FontWeight.w500, color: p.muted),
            ),
            const SizedBox(height: 12),
            if (widget.challenge.methods.length > 1)
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  for (final recovery in [false, true])
                    ChoiceChip(
                      key: Key(
                        recovery ? 'mfa-recovery-mode' : 'mfa-totp-mode',
                      ),
                      label: Text(recovery ? l10n.recoveryCode : l10n.authenticatorApp),
                      selected: _recovery == recovery,
                      onSelected: _busy
                          ? null
                          : (_) => setState(() {
                              _recovery = recovery;
                              _code.clear();
                              _error = null;
                            }),
                    ),
                ],
              ),
            const SizedBox(height: 12),
            AppField(
              label: _recovery ? l10n.recoveryCode : l10n.code,
              child: TextField(
                key: const Key('mfa-code'),
                controller: _code,
                focusNode: _codeFocus,
                autofocus: true,
                enabled: !_busy,
                obscureText: _recovery,
                autocorrect: false,
                enableSuggestions: false,
                enableIMEPersonalizedLearning: false,
                keyboardType: _recovery
                    ? TextInputType.visiblePassword
                    : TextInputType.number,
                textInputAction: TextInputAction.done,
                inputFormatters: _recovery
                    ? [LengthLimitingTextInputFormatter(64)]
                    : [
                        FilteringTextInputFormatter.digitsOnly,
                        LengthLimitingTextInputFormatter(6),
                      ],
                decoration: AppField.decoration(
                  context,
                  hint: _recovery ? null : l10n.n6Digits,
                ),
                onSubmitted: (_) => unawaited(_submit()),
              ),
            ),
          ],
          if (_error != null) ...[
            const SizedBox(height: 12),
            AppStatusLine(_error!, textKey: const Key('mfa-error')),
          ],
        ],
        actions: [
          TextButton(
            key: const Key('mfa-cancel'),
            onPressed: _cancel,
            style: AppButtons.quiet(p),
            child: Text(_expired ? l10n.backSign : l10n.cancel),
          ),
          if (!_expired)
            FilledButton(
              key: const Key('mfa-submit'),
              onPressed: _busy ? null : () => unawaited(_submit()),
              style: AppButtons.filled(p),
              child: Text(_busy ? l10n.checking : l10n.verifySign),
            ),
        ],
      ),
    );
  }
}
