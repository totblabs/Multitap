import 'package:flutter/material.dart';

import '../data/broker_probe.dart';
import '../data/client_settings.dart';
import '../data/broker_transport.dart';
import 'broker_transport_controls.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// Server address entry with the mandatory connection test. Only the address
/// that passed a test for the current input can be saved; any edit revokes
/// that result. Used inline on the sign-in card and inside
/// [ServerSettingsDialog].
class ServerSettingsForm extends StatefulWidget {
  const ServerSettingsForm({
    super.key,
    required this.origin,
    required this.onSave,
    required this.onClose,
    this.onTest,
    this.onSavingChanged,
    this.compact = false,
    this.autofocus = false,
  });

  final Uri? origin;
  final Future<void> Function(Uri) onSave;
  final Future<BrokerProbeResult> Function(Uri)? onTest;

  /// Called with true after a verified address was saved, false on cancel.
  final ValueChanged<bool> onClose;
  final ValueChanged<bool>? onSavingChanged;

  /// The inline sign-in variant: tighter spacing and no explanatory note.
  final bool compact;
  final bool autofocus;

  @override
  State<ServerSettingsForm> createState() => _ServerSettingsFormState();
}

class _ServerSettingsFormState extends State<ServerSettingsForm> {
  AppPalette get _palette => AppPalette.of(context);
  late final _address = TextEditingController(
    text: widget.origin?.toString() ?? '',
  );
  bool _saving = false;
  bool _testing = false;
  bool _confirmingHttp = false;
  String? _error;
  // The test passed and only saving failed: the status title must not say
  // the connection could not be checked.
  bool _saveFailed = false;
  BrokerProbeResult? _result;
  String? _verifiedInput;
  Uri? _verifiedOrigin;
  late String _scheme = widget.origin?.scheme ?? 'https';
  String? _httpConsent;
  int _revision = 0;

  bool get _canSave =>
      !_saving &&
      !_testing &&
      _result != null &&
      _verifiedOrigin != null &&
      (_verifiedOrigin!.scheme != 'http' ||
          _result?.httpApproval?.current == true) &&
      _verifiedInput == _address.text;

  void _setSaving(bool value) {
    _saving = value;
    widget.onSavingChanged?.call(value);
  }

  Future<void> _testConnection() async {
    if (_saving || _testing) return;
    final input = _address.text;
    final revision = ++_revision;
    _result?.httpApproval?.release();
    setState(() {
      _testing = true;
      _error = null;
      _saveFailed = false;
      _result = null;
      _verifiedInput = null;
      _verifiedOrigin = null;
    });
    try {
      final uri = parseServerOrigin(input, defaultScheme: _scheme);
      if (uri.scheme == 'http' && _httpConsent != uri.origin) {
        try {
          _httpConsent = BrokerTransport.consentOrigin(uri);
        } on FormatException {
          /* New exact address. */
        }
        if (_httpConsent != uri.origin) {
          setState(() => _confirmingHttp = true);
          bool accepted;
          try {
            accepted = await confirmInsecureBroker(context, uri);
          } finally {
            if (mounted) setState(() => _confirmingHttp = false);
          }
          if (!accepted) return;
          if (!mounted || revision != _revision || _address.text != input) {
            return;
          }
          _httpConsent = uri.origin;
        }
      }
      final result =
          await (widget.onTest?.call(uri) ??
              probeBroker(uri, httpConsentOrigin: _httpConsent));
      if (uri.scheme == 'http' &&
          (result.httpApproval?.origin != uri ||
              result.httpApproval?.current != true)) {
        result.httpApproval?.release();
        throw const BrokerProbeException('http_consent');
      }
      if (mounted && revision == _revision && _address.text == input) {
        setState(() {
          _result = result;
          _verifiedInput = input;
          _verifiedOrigin = uri;
          _scheme = uri.scheme;
        });
      } else {
        result.httpApproval?.release();
      }
    } on FormatException catch (error) {
      if (mounted && revision == _revision) {
        setState(() => _error = error.message.toString());
      }
    } on BrokerProbeException catch (error) {
      if (mounted && revision == _revision) {
        setState(() => _error = error.message);
      }
    } catch (_) {
      if (mounted && revision == _revision) {
        setState(() => _error = l10n.tryAgainMoment);
      }
    } finally {
      if (mounted) setState(() => _testing = false);
    }
  }

  Future<void> _save() async {
    // The handler also enforces verification, including keyboard submission.
    if (!_canSave) return;
    final uri = parseServerOrigin(_address.text, defaultScheme: _scheme);
    if (uri != _verifiedOrigin) return;
    setState(() {
      _setSaving(true);
      _error = null;
      _saveFailed = false;
    });
    try {
      await widget.onSave(uri);
      BrokerTransport.stored(uri);
      if (mounted) {
        _setSaving(false);
        widget.onClose(true);
      }
    } catch (_) {
      if (mounted) {
        setState(() {
          _saveFailed = true;
          _error = l10n.tryAgain2;
        });
      }
    } finally {
      if (mounted && _saving) setState(() => _setSaving(false));
    }
  }

  void _addressChanged(String _) => setState(() {
    _revision++;
    _result?.httpApproval?.release();
    _httpConsent = null;
    final prefix = _address.text.split('://');
    if (prefix.length > 1 &&
        const {'http', 'https'}.contains(prefix.first.toLowerCase())) {
      _scheme = prefix.first.toLowerCase();
    }
    _result = null;
    _error = null;
    _saveFailed = false;
    _verifiedInput = null;
    _verifiedOrigin = null;
  });

  @override
  void didUpdateWidget(covariant ServerSettingsForm oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.origin != widget.origin) {
      _revision++;
      _result?.httpApproval?.release();
      _result = null;
      _verifiedInput = null;
      _verifiedOrigin = null;
      _httpConsent = null;
      _scheme = widget.origin?.scheme ?? 'https';
      _address.text = widget.origin?.origin ?? '';
    }
  }

  @override
  void dispose() {
    _revision++;
    _result?.httpApproval?.release();
    _address.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final compact = widget.compact;
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            KeepAllText(
              l10n.serverAddress,
              style: TextStyle(
                color: _palette.ink,
                fontSize: 13,
                fontWeight: FontWeight.w600,
              ),
            ),
            const SizedBox(width: 12),
            // Flush right (a Spacer beside a Flexible split the room and
            // left this floating mid-row).
            Expanded(
              child: Row(
                mainAxisAlignment: MainAxisAlignment.end,
                children: [
                  Icon(
                    _scheme == 'https'
                        ? Icons.lock_outline_rounded
                        : Icons.lock_open_rounded,
                    size: 13,
                    color: _palette.muted,
                  ),
                  const SizedBox(width: 4),
                  Flexible(
                    child: KeepAllText(
                      _scheme == 'https' ? l10n.httpsSecureConnection : l10n.httpNotEncrypted,
                      style: TextStyle(fontSize: 12, color: _palette.muted),
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
        SizedBox(height: compact ? 8 : 10),
        BrokerTransportChoice(
          scheme: _scheme,
          onChanged: _saving || _testing
              ? null
              : (value) {
                  final split = _address.text.indexOf('://');
                  if (split >= 0) {
                    _address.text = '$value${_address.text.substring(split)}';
                  }
                  _scheme = value;
                  _addressChanged(_address.text);
                },
        ),
        const SizedBox(height: 10),
        TextField(
          key: const Key('server-origin'),
          controller: _address,
          autofocus: widget.autofocus,
          enabled: !_saving && !_testing,
          maxLength: 200,
          autocorrect: false,
          enableSuggestions: false,
          keyboardType: TextInputType.url,
          textInputAction: TextInputAction.done,
          style: TextStyle(
            color: _palette.ink,
            fontFamily: 'Pretendard',
            fontSize: compact ? 14 : 15,
          ),
          onSubmitted: (_) => _save(),
          onChanged: _addressChanged,
          // The ambient field theme: the glass card on sign-in, the dialog
          // well elsewhere.
          decoration: InputDecoration(
            hintText: _scheme == 'http' ? 'IP:18480' : 'https://example.com',
            counterText: '',
            contentPadding: EdgeInsets.symmetric(horizontal: 16, vertical: 14),
          ),
        ),
        SizedBox(height: compact ? 10 : 12),
        // Amber-tinted until a test passes; then saving is the main action.
        FilledButton.icon(
          key: const Key('test-server-connection'),
          onPressed: _saving || _testing ? null : _testConnection,
          style:
              (_canSave
                      ? AppButtons.quiet(_palette)
                      : AppButtons.tonal(_palette))
                  .copyWith(
                    minimumSize: const WidgetStatePropertyAll(
                      Size(double.infinity, AppButtons.height),
                    ),
                  ),
          icon: _testing && !_confirmingHttp
              ? SizedBox(
                  width: 17,
                  height: 17,
                  child: CircularProgressIndicator(
                    strokeWidth: 2,
                    color: _palette.muted,
                  ),
                )
              : const Icon(Icons.network_check_rounded, size: 20),
          label: KeepAllText(
            _testing && !_confirmingHttp ? l10n.connectingServer : l10n.testConnection,
          ),
        ),
        SizedBox(height: compact ? 10 : 12),
        _connectionStatus(compact),
        SizedBox(height: compact ? 12 : 20),
        AppDialogActions(
          children: [
            TextButton(
              key: const Key('cancel-server-settings'),
              onPressed: _saving ? null : () => widget.onClose(false),
              style: AppButtons.quiet(_palette),
              child: KeepAllText(l10n.cancel),
            ),
            FilledButton(
              key: const Key('save-server'),
              onPressed: _canSave ? _save : null,
              style: AppButtons.filled(_palette),
              child: KeepAllText(_saving ? l10n.saving2 : l10n.save),
            ),
          ],
        ),
      ],
    );
  }

  Widget _connectionStatus(bool compact) {
    final success = _result != null && _error == null;
    final failed = _error != null;
    final color = success
        ? _palette.green
        : failed
        ? _palette.red
        : _palette.muted;
    final background = success
        ? _palette.okSoft
        : failed
        ? _palette.redSoft
        : _palette.subtle;
    final edge = success
        ? _palette.okLine
        : failed
        ? _palette.redLine
        : color.withValues(alpha: 0.15);
    return Semantics(
      liveRegion: true,
      child: Container(
        key: success
            ? const Key('server-test-success')
            : failed
            ? const Key('server-setting-error')
            : const Key('server-test-status'),
        padding: EdgeInsets.all(compact ? 12 : 16),
        decoration: BoxDecoration(
          color: background,
          borderRadius: BorderRadius.circular(10),
          border: Border.all(color: edge),
        ),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.only(top: 1),
              child: Icon(
                success
                    ? Icons.check_circle_rounded
                    : failed
                    ? Icons.error_outline_rounded
                    : Icons.shield_outlined,
                size: 20,
                color: color,
              ),
            ),
            const SizedBox(width: 11),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  KeepAllText(
                    success
                        ? l10n.connectionConfirmed
                        : _saveFailed
                        ? l10n.couldntSaveServerAddress2
                        : failed
                        ? l10n.couldntConfirmConnection
                        : _testing
                        ? l10n.checkingServerResponse
                        : l10n.checkConnection,
                    style: TextStyle(
                      color: color,
                      fontSize: 13,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const SizedBox(height: 5),
                  KeepAllText(
                    success
                        ? l10n.multitapResponseTimeMs(_result!.version, _result!.elapsedMs)
                        : failed
                        ? _error!
                        : _testing
                        ? l10n.pleaseWait
                        : l10n.youCanSaveAddressAfter,
                    // The colour is the icon and the title; the sentence
                    // under them reads as text.
                    style: TextStyle(
                      color: _palette.muted,
                      fontSize: 12,
                      height: 1.5,
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class ServerSettingsDialog extends StatefulWidget {
  const ServerSettingsDialog({
    super.key,
    required this.origin,
    required this.onSave,
    this.onTest,
  });

  final Uri? origin;
  final Future<void> Function(Uri) onSave;
  final Future<BrokerProbeResult> Function(Uri)? onTest;

  @override
  State<ServerSettingsDialog> createState() => _ServerSettingsDialogState();
}

class _ServerSettingsDialogState extends State<ServerSettingsDialog> {
  bool _saving = false;

  @override
  Widget build(BuildContext context) {
    return PopScope(
      canPop: !_saving,
      child: AppDialog(
        icon: Icons.dns_outlined,
        // 저장 (the amber main action) is inside the form below.
        tone: AppTone.accent,
        title: l10n.serverSettings,
        subtitle: l10n.addressServerYouSign,
        // The header X exists only while closing is allowed.
        onClose: _saving ? null : () => Navigator.pop(context, false),
        closeKey: const Key('close-server-settings'),
        maxWidth: 480,
        body: [
          ServerSettingsForm(
            origin: widget.origin,
            onSave: widget.onSave,
            onTest: widget.onTest,
            onSavingChanged: (value) {
              if (mounted) setState(() => _saving = value);
            },
            onClose: (saved) => Navigator.pop(context, saved),
          ),
        ],
      ),
    );
  }
}
