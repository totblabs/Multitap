import 'dart:async';

import 'package:flutter/material.dart';

import '../data/host_settings_api.dart';
import '../data/host_encoder_settings.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// The Host encoder choice on the 가상 화면 page: a short radio list that is
/// applied as soon as a choice is made.
/// Order: 자동 (권장), GPU, CPU (x264), CPU (Windows 기본). With several GPU
/// adapters a second plain list asks which one.
class HostEncoderPanel extends StatefulWidget {
  const HostEncoderPanel({
    super.key,
    required this.api,
    required this.administrator,
    required this.blocked,
    required this.protectMutation,
    required this.onDirty,
    this.onPending,
  });
  final HostSettingsApi api;
  final bool administrator, blocked;
  final Future<bool> Function(bool begin) protectMutation;

  /// True only while GPU is chosen on a Host with several GPUs and no GPU has
  /// been picked yet: the one choice that is not applied at once.
  final ValueChanged<bool> onDirty;
  final ValueChanged<bool>? onPending;
  @override
  State<HostEncoderPanel> createState() => _HostEncoderPanelState();
}

const _conflicts = ['encoder_settings_changed', 'encoder_adapter_changed'];

class _HostEncoderPanelState extends State<HostEncoderPanel> {
  late final _api = HostEncoderSettingsApi(widget.api);
  HostEncoderSettings _settings = const HostEncoderSettings();
  bool _reading = false, _saving = false, _gpuDraft = false, _error = false;
  int _readRevision = 0;
  String? _pendingId, _pendingBackend, _pendingAdapter, _notice;
  @override
  void initState() {
    super.initState();
    unawaited(_read());
  }

  @override
  void dispose() {
    _readRevision++;
    super.dispose();
  }

  bool get _locked =>
      widget.blocked || _reading || _saving || _pendingId != null;
  bool get _canEdit =>
      !_locked && widget.administrator && _settings.canSave;
  List<HostEncoderAdapter> get _candidates => [
    if (_settings.inventoryKnown)
      for (final a in _settings.adapters)
        if (a.candidate) a,
  ];

  void _setDraft(bool value) {
    if (_gpuDraft == value) return;
    _gpuDraft = value;
    widget.onDirty(value);
  }

  bool _matches(HostEncoderSettings value) =>
      value.supported &&
      value.operationId == _pendingId &&
      value.backend == _pendingBackend &&
      value.adapterId == _pendingAdapter;

  Future<void> _read() async {
    if (_reading || _saving) return;
    final revision = ++_readRevision;
    setState(() => _reading = true);
    try {
      final value = await _api.read();
      if (!mounted || revision != _readRevision) return;
      setState(() {
        _settings = value;
        if (_pendingId != null) {
          if (_matches(value)) {
            _pendingId = null;
            widget.onPending?.call(false);
            _notice = l10n.encoderApplied;
            _error = false;
          } else {
            _notice = l10n.couldNotConfirmSaveYet;
            _error = true;
          }
        }
        if (_gpuDraft && _candidates.length < 2) _setDraft(false);
      });
    } catch (error) {
      if (mounted && revision == _readRevision) {
        setState(() {
          _error = true;
          _notice = encoderSettingsFailure(error);
        });
      }
    } finally {
      if (mounted && revision == _readRevision) {
        setState(() => _reading = false);
      }
    }
  }

  void _chooseBackend(String? backend) {
    if (backend == null || !_canEdit) return;
    if (backend != 'gpu') {
      setState(() => _setDraft(false));
      if (backend != _settings.backend) unawaited(_apply(backend, null));
      return;
    }
    final candidates = _candidates;
    if (candidates.isEmpty) return;
    if (_settings.backend == 'gpu' &&
        candidates.any((a) => a.id == _settings.adapterId)) {
      setState(() => _setDraft(false));
      return;
    }
    if (candidates.length == 1) {
      setState(() => _setDraft(false));
      unawaited(_apply('gpu', candidates.single.id));
      return;
    }
    // Several GPUs and none picked yet: ask which one, apply on that pick.
    setState(() {
      _setDraft(true);
      _error = false;
      _notice = null;
    });
  }

  void _chooseAdapter(String? id) {
    if (id == null || !_canEdit) return;
    if (!_candidates.any((a) => a.id == id)) return;
    if (_settings.backend == 'gpu' && _settings.adapterId == id) {
      setState(() => _setDraft(false));
      return;
    }
    unawaited(_apply('gpu', id));
  }

  Future<void> _apply(String backend, String? adapter) async {
    if (!_canEdit) return;
    final expected = _settings.revision!;
    var protected = false, conflict = false;
    try {
      protected = await widget.protectMutation(true);
      if (!protected) {
        if (mounted) {
          setState(() {
            _error = true;
            _notice = l10n.couldNotStartSavingTry;
          });
        }
        return;
      }
      if (!mounted) return;
      setState(() {
        _saving = true;
        _notice = null;
        _pendingId = newEncoderSettingsOperationId();
        _pendingBackend = backend;
        _pendingAdapter = adapter;
      });
      widget.onPending?.call(true);
      final value = await _api.apply(
        operationId: _pendingId!,
        expectedRevision: expected,
        backend: backend,
        adapterId: adapter,
      );
      if (!mounted) return;
      setState(() {
        _settings = value;
        if (_matches(value)) {
          _pendingId = null;
          widget.onPending?.call(false);
          _setDraft(false);
          _error = false;
          _notice = l10n.encoderApplied;
        } else {
          _error = true;
          _notice = l10n.saveResponseCurrentSettingsNeed;
        }
      });
    } catch (error) {
      if (mounted) {
        final code = error.toString();
        conflict = _conflicts.any(code.contains);
        final rejected = const [
          ..._conflicts,
          'encoder_settings_busy',
          'encoder_settings_invalid_request',
          'confirmation_required',
          'encoder_settings_untrusted',
        ].any(code.contains);
        setState(() {
          if (rejected) {
            _pendingId = null;
            widget.onPending?.call(false);
          }
          if (conflict) _setDraft(false);
          _error = true;
          _notice = conflict
              ? l10n.encoderReloadedAfterConflict
              : encoderSettingsFailure(error);
        });
      }
    } finally {
      if (mounted) setState(() => _saving = false);
      if (protected) await widget.protectMutation(false);
    }
    // Someone else changed the choice: show what is saved now.
    if (conflict && mounted) await _read();
  }

  Widget _adapters(AppPalette p, bool canEdit, String? shownAdapter) => Padding(
    key: const Key('host-encoder-adapters'),
    padding: const EdgeInsets.only(left: 52, bottom: 4),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        KeepAllText(
          _gpuDraft ? l10n.encoderPickGpu : l10n.encoderWhichGpu,
          key: const Key('host-encoder-adapter-label'),
          style: TextStyle(color: _gpuDraft ? p.ink : p.muted, fontSize: 13),
        ),
        RadioGroup<String>(
          groupValue: shownAdapter,
          onChanged: _chooseAdapter,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              for (final a in _settings.adapters)
                RadioListTile<String>(
                  key: Key('host-encoder-adapter-${a.id}'),
                  value: a.id,
                  enabled: canEdit && _settings.inventoryKnown && a.candidate,
                  dense: true,
                  contentPadding: EdgeInsets.zero,
                  visualDensity: VisualDensity.compact,
                  activeColor: p.amberInk,
                  title: KeepAllText(
                    a.label,
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(color: p.ink, fontSize: 14),
                  ),
                ),
            ],
          ),
        ),
      ],
    ),
  );

  String _label(String backend) => switch (backend) {
    'auto' => l10n.encoderAuto,
    'gpu' => l10n.encoderGpu,
    'mft' => l10n.encoderMft,
    _ => l10n.encoderX264,
  };

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final canEdit = _canEdit;
    final usage = _settings.recentUsage;
    final candidates = _candidates;
    final shown =
        _pendingBackend != null && _pendingId != null
        ? _pendingBackend!
        : _gpuDraft
        ? 'gpu'
        : _settings.backend;
    final shownAdapter = _pendingId != null
        ? _pendingAdapter
        : _gpuDraft
        ? null
        : _settings.adapterId;
    final muted = TextStyle(color: p.muted, height: 1.5);
    Widget option(String value, String title, {String? detail, bool on = true}) =>
        RadioListTile<String>(
          key: Key('host-encoder-option-$value'),
          value: value,
          enabled: canEdit && on,
          dense: true,
          contentPadding: EdgeInsets.zero,
          visualDensity: VisualDensity.compact,
          activeColor: p.amberInk,
          title: KeepAllText(
            title,
            style: TextStyle(color: p.ink, fontSize: 14),
          ),
          subtitle: detail == null
              ? null
              : KeepAllText(detail, style: muted.copyWith(fontSize: 12.5)),
        );
    return Material(
      key: const Key('host-encoder-settings'),
      // A plain section of the Host settings page (no card): a small heading,
      // the choice, one line of the last confirmed output, then 자세히.
      type: MaterialType.transparency,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text(
            l10n.hostEncoder,
            style: TextStyle(
              color: p.ink,
              fontSize: 15,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 8),
          if (!_reading && !_settings.supported)
            KeepAllText(l10n.hostCannotReportEncoderChoice, style: muted),
          if (_settings.supported) ...[
            RadioGroup<String>(
              groupValue: shown,
              onChanged: _chooseBackend,
              child: Column(
                key: const Key('host-encoder-options'),
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  if (_settings.autoSupported)
                    option(
                      'auto',
                      l10n.encoderAuto,
                      detail: l10n.encoderAutoDetail,
                    ),
                  option(
                    'gpu',
                    l10n.encoderGpu,
                    detail: candidates.isEmpty
                        ? !_settings.inventoryKnown
                              ? l10n.couldNotCheckGpuList
                              : l10n.noConfirmedGpuCandidatesYou
                        : l10n.encoderGpuDetail,
                    on: candidates.isNotEmpty,
                  ),
                  // Which GPU, right under GPU, when the Host has several.
                  if (shown == 'gpu' && _settings.adapters.length > 1)
                    _adapters(p, canEdit, shownAdapter),
                  option('x264', l10n.encoderX264),
                  option('mft', l10n.encoderMft),
                ],
              ),
            ),
            if (!widget.administrator || !_settings.administrator)
              Padding(
                padding: const EdgeInsets.only(top: 4),
                child: KeepAllText(
                  l10n.encoderAdminOnly,
                  key: const Key('host-encoder-admin-only'),
                  style: muted,
                ),
              ),
          ],
          if (_reading || _saving)
            Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Row(
                children: [
                  const SizedBox.square(
                    dimension: 14,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: KeepAllText(
                      _saving
                          ? l10n.savingChoice
                          : l10n.checkingCurrentChoiceGpuCandidates,
                      style: muted,
                    ),
                  ),
                ],
              ),
            )
          else if (_notice != null)
            Padding(
              padding: const EdgeInsets.only(top: 8),
              child: KeepAllText(
                _notice!,
                key: const Key('host-encoder-notice'),
                style: TextStyle(color: _error ? p.red : p.ink, height: 1.5),
              ),
            ),
          const SizedBox(height: 12),
          // A historical validated record, not the live encoder.
          if (usage != null)
            KeepAllText(
              l10n.encoderRecentLine(usage.label, _shortTime(usage.time)),
              key: const Key('host-encoder-recent-usage'),
              style: TextStyle(color: p.ink, height: 1.5),
            )
          else
            KeepAllText(
              l10n.noOutputRecord,
              key: const Key('host-encoder-usage-unknown'),
              style: muted,
            ),
          if (_pendingId != null || _error || !_settings.supported)
            Align(
              alignment: Alignment.centerLeft,
              child: TextButton(
                key: const Key('host-encoder-refresh'),
                style: AppButtons.quiet(p),
                onPressed: _reading || _saving ? null : _read,
                child: KeepAllText(
                  _pendingId != null
                      ? l10n.checkSaveStatus
                      : l10n.refreshStatus2,
                ),
              ),
            ),
          AppDisclosure(
            label: l10n.details,
            headerKey: const Key('host-encoder-details'),
            children: [
              for (final line in [
                if (_settings.supported) l10n.savedChoice(_label(_settings.backend)),
                l10n.chooseMethodNextStreamingConnection,
                if (shown == 'auto') l10n.triesGpuUsedCaptureEarlier,
                if (shown == 'gpu' && candidates.isNotEmpty)
                  l10n.listedCandidateDoesNotMean,
                if (usage != null)
                  l10n.lastOutputLine(
                    usage.trackId == '0' ? '1' : '2',
                    usage.label,
                    _fullTime(usage.time),
                  ),
                ?usage?.fallback,
                usage != null
                    ? l10n.encoderConfirmedRecordedTimeDoes
                    : l10n.noOutputRecordMeaning,
                l10n.gpuChoiceRefersAdapterCurrent,
              ])
                Padding(
                  padding: const EdgeInsets.only(bottom: 6),
                  child: KeepAllText(line, style: muted),
                ),
            ],
          ),
        ],
      ),
    );
  }
}

String _two(int number) => number.toString().padLeft(2, '0');

/// "22:29" for today, "2026-10-30 10:02" otherwise.
String _shortTime(DateTime time, {DateTime? now}) {
  final local = time.toLocal(), today = (now ?? DateTime.now()).toLocal();
  final clock = '${_two(local.hour)}:${_two(local.minute)}';
  return local.year == today.year &&
          local.month == today.month &&
          local.day == today.day
      ? clock
      : '${local.year}-${_two(local.month)}-${_two(local.day)} $clock';
}

String _fullTime(DateTime time) {
  final local = time.toLocal();
  return '${local.year}-${_two(local.month)}-${_two(local.day)} ${_two(local.hour)}:${_two(local.minute)}:${_two(local.second)}';
}
