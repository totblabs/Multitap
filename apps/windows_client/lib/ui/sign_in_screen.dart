import 'dart:async';

import 'package:flutter/material.dart';

import 'appearance_switch.dart';

import '../data/login_memory.dart';
import '../data/client_build_version.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'app_toast.dart';
import 'components.dart';
import 'server_settings_dialog.dart';
import 'password_input_scope.dart';
import 'korean_text.dart';
import 'app_info_dialog.dart';
import '../l10n/l10n.dart';

/// [rejected]: the server said the ID or password is wrong. [unreachable]:
/// the server did not answer at all. [failed]: anything else (a cancelled
/// two-step verification, a refusal for another reason).
enum SignInResult { success, rejected, unreachable, failed }

class SignInScreen extends StatefulWidget {
  const SignInScreen({
    super.key,
    required this.onLogin,
    required this.serverOrigin,
    required this.onServerChanged,
    required this.loginMemory,
    this.onPreview,
    this.busy = false,
    this.error,
    this.onEdited,
    this.restorePassword = true,
  });
  final Future<SignInResult> Function(String, String) onLogin;
  final Uri? serverOrigin;
  final Future<void> Function(Uri) onServerChanged;
  final LoginMemory loginMemory;
  final VoidCallback? onPreview;
  final bool busy;
  final String? error;

  /// The ID or the password was edited: the owner drops [error], which was
  /// about the attempt before the edit.
  final VoidCallback? onEdited;
  final bool restorePassword;
  @override
  State<SignInScreen> createState() => _SignInScreenState();
}

class _SignInScreenState extends State<SignInScreen> {
  final _id = TextEditingController();
  final _password = TextEditingController();
  bool _rememberPassword = false;
  bool _submitting = false;
  // With no server yet, the address form is the first thing to fill in, so
  // it starts open.
  late bool _serverOpen = widget.serverOrigin == null;
  bool _serverSaving = false;
  bool _infoOpen = false;

  /// The last sign-in got no answer from this server: its dot is not green.
  bool _unreachable = false;
  int _editRevision = 0;
  int _loadSequence = 0;
  String? _memoryError;
  Future<void> _memoryWrites = Future<void>.value();

  @override
  void initState() {
    super.initState();
    unawaited(_restoreRemembered());
  }

  @override
  void didUpdateWidget(SignInScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.serverOrigin != widget.serverOrigin ||
        oldWidget.loginMemory != widget.loginMemory) {
      _editRevision++;
      _id.clear();
      _password.clear();
      _rememberPassword = false;
      _memoryError = null;
      _unreachable = false;
      unawaited(_restoreRemembered());
    } else if (oldWidget.restorePassword != widget.restorePassword) {
      unawaited(_restoreRemembered());
    }
  }

  Future<void> _restoreRemembered() async {
    final sequence = ++_loadSequence;
    final revision = _editRevision;
    final origin = widget.serverOrigin;
    if (origin == null) return;
    try {
      await _memoryWrites;
      final remembered = await widget.loginMemory.load(origin);
      if (!mounted ||
          sequence != _loadSequence ||
          revision != _editRevision ||
          origin != widget.serverOrigin) {
        return;
      }
      final allowed = passwordMemoryAllowed(origin);
      setState(() {
        _id.text = remembered.username;
        _password.text = widget.restorePassword && allowed
            ? remembered.password ?? ''
            : '';
        _rememberPassword = remembered.password != null && allowed;
      });
    } catch (_) {
      if (mounted && sequence == _loadSequence && revision == _editRevision) {
        setState(() => _memoryError = l10n.couldntLoadSavedSignInfo);
      }
    }
  }

  Future<void> _queueMemory(Future<void> Function() action) {
    final operation = _memoryWrites.then((_) => action());
    _memoryWrites = operation.catchError((Object _) {});
    return operation;
  }

  void _usernameChanged(String value) {
    final revision = ++_editRevision;
    if (widget.error != null) widget.onEdited?.call();
    final origin = widget.serverOrigin;
    if (origin == null) return;
    final memory = widget.loginMemory;
    _password.clear();
    setState(() => _memoryError = null);
    unawaited(
      _queueMemory(() async {
        await memory.forgetPassword(origin);
        await memory.saveUsername(origin, value.trim());
      }).catchError((Object _) {
        if (mounted && revision == _editRevision) {
          setState(() => _memoryError = l10n.couldntRememberUsernameYouCan);
        }
      }),
    );
  }

  void _rememberChanged(bool? value) {
    final revision = ++_editRevision;
    setState(() {
      _rememberPassword = value ?? false;
      _memoryError = null;
    });
    if (_rememberPassword) return;
    final origin = widget.serverOrigin;
    if (origin == null) return;
    final memory = widget.loginMemory;
    unawaited(
      _queueMemory(() => memory.forgetPassword(origin)).catchError((Object _) {
        if (mounted && revision == _editRevision) {
          setState(() => _memoryError = l10n.couldntTurnOffRememberPassword2);
        }
      }),
    );
  }

  Future<void> _submit() async {
    if (widget.busy || _submitting || widget.serverOrigin == null) return;
    final username = _id.text.trim();
    final password = _password.text;
    final origin = widget.serverOrigin!;
    // Over HTTP nothing is remembered, and a password stored earlier is
    // forgotten by this sign-in.
    final rememberPassword = _rememberPassword && passwordMemoryAllowed(origin);
    final revision = _editRevision;
    final memory = widget.loginMemory;
    final messenger = ScaffoldMessenger.of(context);
    setState(() {
      _submitting = true;
      _memoryError = null;
    });
    try {
      // Remembering credentials is optional and must never gate authentication.
      final result = await widget.onLogin(username, password);
      if (mounted) {
        setState(() => _unreachable = result == SignInResult.unreachable);
      }
      String? memoryWarning;
      try {
        final saved = _queueMemory(() async {
          await memory.saveUsername(origin, username);
          if (result == SignInResult.success && rememberPassword) {
            await memory.savePassword(origin, username, password);
          } else if (!rememberPassword || result == SignInResult.rejected) {
            await memory.forgetPassword(origin);
          }
        });
        // Bound only this UI observation. The actual write stays in the tail:
        // a late native reply must finish before later edits/opt-outs run.
        await saved.timeout(
          const Duration(seconds: 3),
          onTimeout: () {
            memoryWarning = l10n.couldntConfirmRememberSignSetting;
          },
        );
      } catch (_) {
        memoryWarning = l10n.couldntSaveRememberSignSetting;
      }
      if (revision != _editRevision ||
          origin != widget.serverOrigin ||
          !identical(memory, widget.loginMemory)) {
        return;
      }
      final warning = memoryWarning;
      if (mounted && result != SignInResult.success) {
        // Only a wrong ID or password is typed again; a server that did not
        // answer or a cancelled two-step verification keeps what was typed.
        if (result == SignInResult.rejected) _password.clear();
        setState(() => _memoryError = warning);
      } else if (warning != null && messenger.mounted) {
        messenger.showSnackBar(
          appToast(messenger.context, warning, kind: AppToastKind.failed),
        );
      }
    } finally {
      if (mounted) setState(() => _submitting = false);
    }
  }

  // The server chip opens the address edit and its connection test in place.
  void _toggleServer() {
    if (_serverSaving) return;
    setState(() => _serverOpen = !_serverOpen);
  }

  Future<void> _openInfo() async {
    if (_infoOpen) return;
    _infoOpen = true;
    try {
      await showAppInfoDialog(context);
    } finally {
      _infoOpen = false;
    }
  }

  @override
  void dispose() {
    _editRevision++;
    _id.dispose();
    _password.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    body: SceneBackground(
      child: SafeArea(
        child: LayoutBuilder(
          builder: (context, constraints) {
            final palette = AppPalette.of(context);
            final scale = MediaQuery.textScalerOf(context).scale(14) / 14;
            final short = constraints.maxHeight / scale < 640;
            final side = constraints.maxWidth < 600 ? 16.0 : 40.0;
            final cardWidth = (constraints.maxWidth - 2 * side).clamp(
              0.0,
              392.0,
            );
            // The product's own surfaces: the tile, 18 px
            // corners and a hairline, as on every dialog.
            final card = DecoratedBox(
              decoration: BoxDecoration(
                borderRadius: BorderRadius.circular(18),
                boxShadow: [
                  BoxShadow(
                    color: palette.dark
                        ? const Color(0x30000000)
                        : const Color(0x18000000),
                    blurRadius: 24,
                    spreadRadius: -8,
                    offset: const Offset(0, 12),
                  ),
                ],
              ),
              child: AnimatedSize(
                duration: const Duration(milliseconds: 180),
                curve: Curves.easeOutCubic,
                alignment: Alignment.topCenter,
                child: Container(
                  key: const Key('login-card'),
                  width: cardWidth,
                  padding: EdgeInsets.fromLTRB(
                    short ? 16 : 22,
                    short ? 10 : 14,
                    short ? 16 : 22,
                    short ? 16 : 22,
                  ),
                  decoration: BoxDecoration(
                    color: palette.tile,
                    borderRadius: BorderRadius.circular(18),
                    border: Border.all(color: palette.tileLine),
                  ),
                  child: _card(palette, compact: short),
                ),
              ),
            );
            // The guidance is a line under the card; the version is a small
            // mark in the window's corner that opens the information. A
            // window with no room beside the card keeps both under it.
            final corner = !short && constraints.maxWidth / scale >= 720;
            // Both sit on the lower part of the scene, which is dark in
            // either appearance (forest by day, the dimmed picture by
            // night): light ink with the dark halo, never the theme's ink.
            const onScene = AppPalette(true);
            final guidance = KeepAllText(
              l10n.askAdministratorAccount,
              key: const Key('login-footer'),
              textAlign: TextAlign.center,
              style: uiText(
                size: 13,
                height: 1.5,
                color: onScene.ink,
              ).copyWith(shadows: onScene.sceneHalo),
            );
            final version = Tooltip(
              message: l10n.aboutLicenses,
              child: TextButton(
                key: const Key('login-app-info'),
                onPressed: () => unawaited(_openInfo()),
                style: TextButton.styleFrom(
                  foregroundColor: onScene.ink,
                  minimumSize: const Size(0, 32),
                  maximumSize: const Size(double.infinity, double.infinity),
                  visualDensity: VisualDensity.standard,
                  tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  padding: const EdgeInsets.symmetric(
                    horizontal: 10,
                    vertical: 8,
                  ),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(8),
                  ),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(
                      Icons.info_outline_rounded,
                      size: 16,
                      shadows: onScene.sceneHalo,
                    ),
                    const SizedBox(width: 6),
                    Flexible(
                      child: Text(
                        runningClientBuild.version == null
                            ? runningClientBuild.label
                            : 'v${runningClientBuild.version}',
                        key: const Key('login-version'),
                        style:
                            uiText(
                              size: 12,
                              weight: FontWeight.w500,
                              color: onScene.ink,
                            ).copyWith(
                              fontFeatures: const [
                                FontFeature.tabularFigures(),
                              ],
                              shadows: onScene.sceneHalo,
                            ),
                      ),
                    ),
                  ],
                ),
              ),
            );
            return Stack(
              fit: StackFit.expand,
              children: [
                ScrollConfiguration(
                  behavior: ScrollConfiguration.of(context)
                      .copyWith(scrollbars: false),
                  child: SingleChildScrollView(
                    key: const Key('login-scroll'),
                    child: ConstrainedBox(
                      constraints: BoxConstraints(
                        minHeight: constraints.maxHeight,
                      ),
                      child: Padding(
                        padding: EdgeInsets.fromLTRB(
                          side,
                          short ? 8 : 24,
                          side,
                          short ? 8 : 18,
                        ),
                        child: Column(
                          mainAxisSize: MainAxisSize.min,
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            FittedBox(
                              fit: BoxFit.scaleDown,
                              child: Brand(
                                onDark: palette.dark,
                                scale: short ? 1.35 : 2,
                              ),
                            ),
                            SizedBox(height: short ? 12 : 26),
                            card,
                            SizedBox(height: short ? 10 : 16),
                            ConstrainedBox(
                              constraints: BoxConstraints(
                                maxWidth: constraints.maxWidth - 2 * side,
                              ),
                              child: guidance,
                            ),
                            if (!corner) ...[
                              const SizedBox(height: 2),
                              version,
                            ],
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
                if (corner) Positioned(left: 12, bottom: 10, child: version),
                Positioned(
                  key: const Key('login-header'),
                  top: short ? 6 : 14,
                  right: short ? 8 : 18,
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: palette.tile,
                      shape: BoxShape.circle,
                      border: Border.all(color: palette.tileLine),
                    ),
                    child: AppearanceSwitch(
                      compact: true,
                      iconColor: palette.ink,
                    ),
                  ),
                ),
              ],
            );
          },
        ),
      ),
    ),
  );

  Widget _card(AppPalette palette, {required bool compact}) => Column(
    mainAxisSize: MainAxisSize.min,
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: [
      _serverLine(palette),
      SizedBox(height: compact ? 8 : 10),
      if (_serverOpen)
        ServerSettingsForm(
          key: const Key('login-server-form'),
          origin: widget.serverOrigin,
          onSave: widget.onServerChanged,
          compact: true,
          autofocus: true,
          onSavingChanged: (value) {
            if (mounted) setState(() => _serverSaving = value);
          },
          onClose: (_) {
            if (mounted) setState(() => _serverOpen = false);
          },
        )
      else
        ..._loginFields(palette, compact: compact),
    ],
  );

  /// The card's head: which server this is, and the way to change it. One
  /// line, not a box: only what is typed into looks like a field.
  Widget _serverLine(AppPalette palette) {
    final configured = widget.serverOrigin != null;
    final working = widget.busy || _submitting;
    final origin = widget.serverOrigin;
    final label = configured
        ? '${origin!.host}${origin.hasPort ? ':${origin.port}' : ''}'
        : l10n.serverSetupNeeded;
    final still = MediaQuery.maybeDisableAnimationsOf(context) ?? false;
    return Tooltip(
      message: configured ? l10n.changeServerAddress : l10n.setServerAddress,
      child: Material(
        type: MaterialType.transparency,
        child: InkWell(
          key: const Key('open-server-settings'),
          onTap: working || _serverSaving ? null : _toggleServer,
          borderRadius: BorderRadius.circular(8),
          child: ConstrainedBox(
            constraints: const BoxConstraints(minHeight: 36),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 6),
              child: Row(
                children: [
                  Container(
                    key: const Key('login-server-dot'),
                    width: 7,
                    height: 7,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: configured && !_unreachable
                          ? palette.green
                          : palette.warn,
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: KeepAllText(
                      label,
                      key: const Key('login-server-label'),
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: uiText(
                        size: 13,
                        weight: FontWeight.w600,
                        color: palette.ink,
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),
                  // Without a server the line and the button already say
                  // what to do.
                  if (configured)
                    KeepAllText(
                      l10n.changeServer,
                      style: uiText(
                        size: 12,
                        weight: FontWeight.w500,
                        color: palette.ink2,
                      ),
                    ),
                  const SizedBox(width: 2),
                  AnimatedRotation(
                    turns: _serverOpen ? .5 : 0,
                    duration: still
                        ? Duration.zero
                        : const Duration(milliseconds: 180),
                    child: Icon(
                      Icons.keyboard_arrow_down_rounded,
                      size: 16,
                      color: palette.muted,
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  List<Widget> _loginFields(AppPalette palette, {required bool compact}) {
    final working = widget.busy || _submitting;
    final configured = widget.serverOrigin != null;
    // HTTP server: 비밀번호 기억 is off and disabled, with one plain line.
    final passwordMemory =
        !configured || passwordMemoryAllowed(widget.serverOrigin);
    final scale = MediaQuery.textScalerOf(context).scale(1);
    return [
      TextField(
        key: const Key('sign-in-id'),
        controller: _id,
        enabled: !working && configured,
        maxLength: 64,
        onChanged: _usernameChanged,
        textInputAction: TextInputAction.next,
        style: uiText(size: 14, color: palette.ink),
        decoration: AppField.decoration(
          context,
          hint: l10n.id,
          prefixIcon: const Icon(Icons.person_outline_rounded, size: 18),
        ),
      ),
      SizedBox(height: compact ? 8 : 10),
      PasswordInputScope(
        builder: (focus) => TextField(
          key: const Key('sign-in-password'),
          focusNode: focus,
          controller: _password,
          enabled: !working && configured,
          maxLength: 256,
          textInputAction: TextInputAction.done,
          onSubmitted: (_) => _submit(),
          obscureText: true,
          keyboardType: TextInputType.visiblePassword,
          onChanged: (_) {
            _editRevision++;
            if (widget.error != null) widget.onEdited?.call();
            if (_memoryError != null) {
              setState(() => _memoryError = null);
            }
          },
          autocorrect: false,
          enableSuggestions: false,
          style: uiText(size: 14, color: palette.ink),
          decoration: AppField.decoration(
            context,
            hint: l10n.password,
            prefixIcon: const Icon(Icons.key_outlined, size: 18),
          ),
        ),
      ),
      SizedBox(height: compact ? 4 : 8),
      Material(
        type: MaterialType.transparency,
        child: InkWell(
          borderRadius: BorderRadius.circular(8),
          onTap: working || !configured || !passwordMemory
              ? null
              : () => _rememberChanged(!_rememberPassword),
          child: Padding(
            padding: const EdgeInsets.fromLTRB(4, 2, 0, 2),
            // The label never shares a line with its hint: side by side,
            // narrow windows and large text cut "비밀번호 기억".
            child: Row(
              children: [
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      KeepAllText(
                        l10n.rememberPassword,
                        style: uiText(size: 13, color: palette.ink),
                      ),
                      if (!passwordMemory)
                        KeepAllText(
                          l10n.passwordsRememberedOnlyHttps,
                          key: const Key('remember-password-https-only'),
                          style: uiText(size: 12, color: palette.muted),
                        )
                      else if (!compact)
                        KeepAllText(
                          l10n.saveWindowsCredentialManager,
                          style: uiText(size: 12, color: palette.muted),
                        ),
                    ],
                  ),
                ),
                const SizedBox(width: 8),
                // The product's switch, at the product's size.
                Switch(
                  key: const Key('remember-password'),
                  value: _rememberPassword && passwordMemory,
                  onChanged: working || !configured || !passwordMemory
                      ? null
                      : _rememberChanged,
                ),
              ],
            ),
          ),
        ),
      ),
      // What went wrong, right above the button. One line of room is always
      // kept, so the card does not jump when the line appears.
      Padding(
        padding: EdgeInsets.only(top: compact ? 4 : 6, bottom: 8),
        child: ConstrainedBox(
          // (A line of text is laid out on whole pixels.)
          constraints: BoxConstraints(
            minHeight: (13 * 1.6 * scale).ceilToDouble(),
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              if (widget.error != null)
                AppStatusLine(widget.error!, textKey: const Key('login-error')),
              if (widget.error != null && _memoryError != null)
                const SizedBox(height: 4),
              if (_memoryError != null)
                AppStatusLine(
                  _memoryError!,
                  textKey: const Key('login-memory-warning'),
                  tone: AppTone.warn,
                ),
            ],
          ),
        ),
      ),
      SizedBox(
        width: double.infinity,
        child: FilledButton(
          key: const Key('login-submit'),
          onPressed: working
              ? null
              : !configured
              ? _toggleServer
              : _submit,
          style: AppButtons.filled(palette).copyWith(
            minimumSize: WidgetStatePropertyAll(Size(0, compact ? 42 : 46)),
            textStyle: WidgetStatePropertyAll(
              uiText(size: 15, weight: FontWeight.w600),
            ),
          ),
          // Enter still signs in; the key is not drawn on the button.
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              if (working) ...[
                SizedBox(
                  width: 14,
                  height: 14,
                  child: CircularProgressIndicator(
                    strokeWidth: 2,
                    color: palette.disabled,
                  ),
                ),
                const SizedBox(width: 10),
              ],
              Flexible(
                child: KeepAllText(
                  working
                      ? l10n.signing
                      : !configured
                      ? l10n.setUpServerStart
                      : l10n.sign,
                  textAlign: TextAlign.center,
                ),
              ),
            ],
          ),
        ),
      ),
      if (widget.onPreview != null) ...[
        const SizedBox(height: 6),
        Center(
          child: TextButton(
            key: const Key('open-preview'),
            onPressed: () {
              _id.clear();
              _password.clear();
              widget.onPreview!();
            },
            style: TextButton.styleFrom(
              foregroundColor: palette.muted,
              minimumSize: const Size(0, 30),
              textStyle: uiText(size: 12),
            ),
            child: KeepAllText(
              l10n.exploreDemo,
              style: TextStyle(fontSize: 12),
            ),
          ),
        ),
      ],
    ];
  }
}
