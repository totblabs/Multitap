import 'package:flutter/material.dart';

import '../data/broker_api.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'password_input_scope.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

class ChangePasswordDialog extends StatefulWidget {
  const ChangePasswordDialog({super.key, required this.onChange});
  final Future<void> Function(String, String, String) onChange;
  @override
  State<ChangePasswordDialog> createState() => _ChangePasswordDialogState();
}

class _ChangePasswordDialogState extends State<ChangePasswordDialog> {
  AppPalette get _palette => AppPalette.of(context);
  final _current = TextEditingController();
  final _password = TextEditingController();
  final _confirmation = TextEditingController();
  bool _busy = false;
  String? _error;

  Future<void> _save() async {
    if (_busy) return;
    if (_password.text != _confirmation.text) {
      setState(() => _error = l10n.newPasswordConfirmationDontMatch);
      return;
    }
    if (_current.text.isEmpty ||
        _password.text.runes.length < 10 ||
        _password.text.runes.length > 256) {
      setState(() => _error = l10n.enterCurrentPasswordChooseNew);
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      await widget.onChange(_current.text, _password.text, _confirmation.text);
      if (mounted) Navigator.of(context).popUntil((route) => route.isFirst);
    } on BrokerException catch (error) {
      if (mounted) setState(() => _error = error.message);
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
    _current.dispose();
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
        title: l10n.changePassword,
        subtitle: l10n.changingSignsYouOutAll,
        maxWidth: 420,
        body: [
          _field(l10n.currentPassword, 'current-password', _current),
          const SizedBox(height: 12),
          _field(l10n.newPasswordLeast10Characters, 'new-password', _password),
          const SizedBox(height: 12),
          _field(l10n.confirmNewPassword, 'confirm-new-password', _confirmation),
          if (_error != null)
            Padding(
              padding: const EdgeInsets.only(top: 12),
              // The colour is the icon's; the sentence is ordinary text.
              child: AppStatusLine(
                _error!,
                textKey: const Key('password-change-error'),
              ),
            ),
        ],
        actions: [
          TextButton(
            onPressed: _busy ? null : () => Navigator.pop(context),
            style: AppButtons.quiet(p),
            child: KeepAllText(l10n.cancel),
          ),
          FilledButton(
            key: const Key('save-password-change'),
            onPressed: _busy ? null : _save,
            style: AppButtons.filled(p),
            child: KeepAllText(_busy ? l10n.changing : l10n.change),
          ),
        ],
      ),
    );
  }
}
