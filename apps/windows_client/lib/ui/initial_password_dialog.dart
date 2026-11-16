import 'package:flutter/material.dart';

import '../data/broker_api.dart';
import '../l10n/l10n.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'password_input_scope.dart';

/// 첫 로그인 비밀번호 (2026-11-16): the server answered a sign-in with
/// `password_change_required` (an administrator set this password). The
/// account picks its own here; [onSubmit] sends it. Returns the new password
/// once the server took it, or null when the person cancelled.
Future<String?> showInitialPasswordDialog(
  BuildContext context, {
  required Future<void> Function(String newPassword, String confirmation)
  onSubmit,
}) => showDialog<String>(
  context: context,
  barrierDismissible: false,
  builder: (_) => InitialPasswordDialog(onSubmit: onSubmit),
);

class InitialPasswordDialog extends StatefulWidget {
  const InitialPasswordDialog({super.key, required this.onSubmit});
  final Future<void> Function(String newPassword, String confirmation)
  onSubmit;
  @override
  State<InitialPasswordDialog> createState() => _InitialPasswordDialogState();
}

class _InitialPasswordDialogState extends State<InitialPasswordDialog> {
  AppPalette get _palette => AppPalette.of(context);
  final _password = TextEditingController();
  final _confirmation = TextEditingController();
  bool _busy = false;
  String? _error;

  Future<void> _save() async {
    if (_busy) return;
    final password = _password.text, confirmation = _confirmation.text;
    if (password.runes.length < 10 || password.runes.length > 256) {
      setState(() => _error = l10n.passwordsMust10256Characters);
      return;
    }
    if (password != confirmation) {
      setState(() => _error = l10n.newPasswordConfirmationDontMatch);
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      await widget.onSubmit(password, confirmation);
      if (mounted) Navigator.of(context).pop(password);
    } on BrokerException catch (error) {
      if (!mounted) return;
      setState(() => _error = error.message);
      if (error.code == 'password_unchanged') {
        _password.clear();
        _confirmation.clear();
      }
    } catch (_) {
      if (mounted) setState(() => _error = l10n.couldntChangePasswordTryAgain);
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Widget _field(String label, String key, TextEditingController controller) =>
      AppField(
        label: label,
        child: PasswordInputScope(
          builder: (focus) => TextField(
            key: Key(key),
            controller: controller,
            focusNode: focus,
            enabled: !_busy,
            autofocus: controller == _password,
            obscureText: true,
            keyboardType: TextInputType.visiblePassword,
            enableSuggestions: false,
            autocorrect: false,
            maxLength: 256,
            style: uiText(size: 14, color: _palette.ink),
            onSubmitted: (_) {
              if (controller == _confirmation) _save();
            },
            decoration: AppField.decoration(context),
          ),
        ),
      );

  @override
  void dispose() {
    _password.dispose();
    _confirmation.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final p = _palette;
    return PopScope(
      canPop: !_busy,
      child: AppDialog(
        icon: Icons.key_rounded,
        busy: _busy,
        title: l10n.setNewPasswordTitle,
        subtitle: l10n.setNewPasswordSubtitle,
        maxWidth: 420,
        body: [
          _field(
            l10n.newPasswordLeast10Characters,
            'initial-new-password',
            _password,
          ),
          const SizedBox(height: 12),
          _field(
            l10n.confirmNewPassword,
            'initial-confirm-password',
            _confirmation,
          ),
          if (_error != null)
            Padding(
              padding: const EdgeInsets.only(top: 12),
              child: AppStatusLine(
                _error!,
                textKey: const Key('initial-password-error'),
              ),
            ),
        ],
        actions: [
          TextButton(
            key: const Key('cancel-initial-password'),
            onPressed: _busy ? null : () => Navigator.pop(context),
            style: AppButtons.quiet(p),
            child: KeepAllText(l10n.cancel),
          ),
          FilledButton(
            key: const Key('save-initial-password'),
            onPressed: _busy ? null : _save,
            style: AppButtons.filled(p),
            child: KeepAllText(_busy ? l10n.changing : l10n.changeAndSignIn),
          ),
        ],
      ),
    );
  }
}
