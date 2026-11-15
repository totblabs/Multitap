import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../data/broker_api.dart' show BrokerException;
import '../models/desktop_organize.dart';
import '../l10n/l10n.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'korean_text.dart';

/// Desktop groups in the launcher (Windows and Android): the section
/// header, the name dialog and the changes to the account's list.

/// Saves this account's whole group list in its order and resolves to the
/// list the broker kept.
typedef SaveDesktopGroups =
    Future<List<DesktopGroup>> Function(List<DesktopGroupDraft> groups);

/// What a failed group change says.
String desktopGroupFailureText(Object error) {
  if (error is BrokerException) {
    switch (error.code) {
      case 'invalid_group_name':
        return l10n.groupNameRules;
      case 'duplicate_group_name':
        return l10n.groupNameTaken;
      case 'too_many_groups':
        return l10n.tooManyGroups;
      case 'unknown_group':
        return l10n.groupNoLongerExists;
      case 'desktop_not_found':
        return l10n.desktopNoLongerAssignedYou;
    }
  }
  return l10n.couldntChangeGroupsTryAgain;
}

/// The section's count: N, or shown/N while a search or filter hides some
/// (as the title's count).
String desktopSectionCount(DesktopSection section) =>
    section.machines.length == section.total
    ? '${section.total}'
    : '${section.machines.length}/${section.total}';

/// A section's heading over its cards: a fold button (a chevron, the group's
/// name and its count; a screen reader hears a button that is expanded or
/// collapsed), a hairline to the right edge and, for a group, ⋯ with 이름
/// 바꾸기, 위로 옮기기, 아래로 옮기기 and 그룹 삭제. 기타 has no ⋯. [touch]:
/// 48 dp targets and menu rows (Android).
class DesktopSectionHeader extends StatefulWidget {
  const DesktopSectionHeader({
    super.key,
    required this.section,
    required this.folded,
    required this.onToggle,
    this.onRename,
    this.onMoveUp,
    this.onMoveDown,
    this.onDelete,
    this.manage = false,
    this.touch = false,
    this.focusStyle,
  });

  final DesktopSection section;
  final bool folded;
  final VoidCallback onToggle;

  /// The group's ⋯ is shown ([section] is a group and the broker keeps
  /// them); a null action is a greyed row (the first cannot move up, a
  /// change is being saved).
  final bool manage;
  final VoidCallback? onRename, onMoveUp, onMoveDown, onDelete;
  final bool touch;

  /// Paints keyboard focus on ⋯ (the Android TV remote).
  final ButtonStyle Function(BuildContext context, ButtonStyle style)?
  focusStyle;

  @override
  State<DesktopSectionHeader> createState() => _DesktopSectionHeaderState();
}

class _DesktopSectionHeaderState extends State<DesktopSectionHeader> {
  final _menu = MenuController();
  bool _focused = false;

  @override
  void dispose() {
    AppMenu.closed(_menu);
    super.dispose();
  }

  Widget _item(
    AppPalette p, {
    required String key,
    required IconData icon,
    required Color color,
    required String label,
    required VoidCallback? onPressed,
    bool danger = false,
  }) => MenuItemButton(
    key: Key(key),
    onPressed: onPressed,
    style: MenuItemButton.styleFrom(
      minimumSize: Size(0, widget.touch ? 48 : 40),
      maximumSize: const Size(double.infinity, double.infinity),
      padding: EdgeInsets.symmetric(horizontal: widget.touch ? 16 : 12),
      textStyle: widget.touch
          ? uiText(size: 14)
          : uiText(size: 13, weight: FontWeight.w500),
      foregroundColor: danger ? p.red : null,
    ),
    leadingIcon: AppMenu.icon(
      p,
      icon,
      color: color,
      enabled: onPressed != null,
    ),
    child: KeepAllText(label),
  );

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final section = widget.section;
    final touch = widget.touch;
    final key = section.key;
    final name = section.group?.name ?? l10n.otherGroup;
    final count = desktopSectionCount(section);
    final scaler = MediaQuery.textScalerOf(context);
    final height = math.max(
      touch ? 48.0 : 36.0,
      scaler.scale(touch ? 16 : 15) * 1.4 + 10,
    );
    final ring =
        _focused &&
        FocusManager.instance.highlightMode == FocusHighlightMode.traditional;
    final fold = Semantics(
      button: true,
      expanded: !widget.folded,
      label: l10n.groupSection(name, count),
      excludeSemantics: true,
      onTap: widget.onToggle,
      child: Tooltip(
        message: widget.folded ? l10n.expand : l10n.collapse,
        waitDuration: const Duration(milliseconds: 700),
        child: Material(
          type: MaterialType.transparency,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(10),
            side: ring
                ? BorderSide(color: p.ink, width: 1.5)
                : BorderSide.none,
          ),
          clipBehavior: Clip.antiAlias,
          child: InkWell(
            key: Key('section-$key'),
            onTap: widget.onToggle,
            onFocusChange: (value) => setState(() => _focused = value),
            hoverColor: p.hover,
            focusColor: Colors.transparent,
            child: SizedBox(
              height: height,
              child: Padding(
                padding: const EdgeInsets.only(left: 4, right: 10),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    AnimatedRotation(
                      turns: widget.folded ? -.25 : 0,
                      duration: const Duration(milliseconds: 150),
                      child: Icon(
                        Icons.expand_more_rounded,
                        size: touch ? 22 : 20,
                        color: p.muted,
                      ),
                    ),
                    const SizedBox(width: 4),
                    Flexible(
                      child: KeepAllText(
                        name,
                        key: Key('section-name-$key'),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: uiText(
                          size: touch ? 16 : 15,
                          weight: FontWeight.w600,
                          color: p.ink,
                        ).copyWith(shadows: p.sceneHalo),
                      ),
                    ),
                    const SizedBox(width: 8),
                    KeepAllText(
                      count,
                      key: Key('section-count-$key'),
                      maxLines: 1,
                      style: uiText(
                        size: 13,
                        weight: FontWeight.w600,
                        color: p.muted,
                      ).copyWith(
                        fontFeatures: const [FontFeature.tabularFigures()],
                        shadows: p.sceneHalo,
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
    final group = section.group;
    final more = !widget.manage || group == null
        ? null
        : MenuAnchor(
            controller: _menu,
            consumeOutsideTap: true,
            onOpen: () => AppMenu.opened(_menu),
            onClose: () => AppMenu.closed(_menu),
            style: AppMenu.menuStyle(p, minWidth: 176),
            menuChildren: [
              _item(
                p,
                key: 'section-rename-$key',
                icon: Icons.drive_file_rename_outline_rounded,
                color: p.blue,
                label: l10n.rename,
                onPressed: widget.onRename,
              ),
              _item(
                p,
                key: 'section-up-$key',
                icon: Icons.arrow_upward_rounded,
                color: p.muted,
                label: l10n.moveUp,
                onPressed: widget.onMoveUp,
              ),
              _item(
                p,
                key: 'section-down-$key',
                icon: Icons.arrow_downward_rounded,
                color: p.muted,
                label: l10n.moveDown,
                onPressed: widget.onMoveDown,
              ),
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 4),
                child: Divider(height: 1, color: p.line),
              ),
              _item(
                p,
                key: 'section-delete-$key',
                icon: Icons.delete_outline_rounded,
                color: p.red,
                label: l10n.deleteGroup,
                onPressed: widget.onDelete,
                danger: true,
              ),
            ],
            builder: (context, controller, _) {
              final size = touch ? 48.0 : 36.0;
              final style = IconButton.styleFrom(
                foregroundColor: p.muted,
                minimumSize: Size.square(size),
                maximumSize: Size.square(size),
                padding: EdgeInsets.zero,
                tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(10),
                ),
              );
              return IconButton(
                key: Key('section-more-$key'),
                tooltip: l10n.groupOptions(group.name),
                onPressed: () =>
                    controller.isOpen ? controller.close() : controller.open(),
                style: widget.focusStyle?.call(context, style) ?? style,
                icon: const Icon(Icons.more_horiz_rounded, size: 20),
              );
            },
          );
    // The name takes what it needs (a long one ends in …), the hairline the
    // rest of the row, so ⋯ sits at the right edge over the cards.
    return LayoutBuilder(
      builder: (context, bounds) => Row(
        children: [
          ConstrainedBox(
            constraints: BoxConstraints(maxWidth: bounds.maxWidth * .7),
            child: fold,
          ),
          const SizedBox(width: 10),
          Expanded(child: Container(height: 1, color: p.glassEdge)),
          if (more != null) ...[const SizedBox(width: 4), more],
        ],
      ),
    );
  }
}

/// Asks for a group's name ([initial] when renaming). [submit] saves it and
/// answers what to say when it could not (null: saved); the dialog stays
/// open with that line and the text, and closes once saved (true). Enter in
/// the field is the main button. [taken]: the other groups' names
/// ([DesktopGroup.nameKey]).
Future<bool> showDesktopGroupNameDialog(
  BuildContext context, {
  required String title,
  required String confirmLabel,
  required Future<String?> Function(String name) submit,
  String initial = '',
  Set<String> taken = const {},
  IconData icon = Icons.create_new_folder_outlined,
}) async {
  final saved = await showDialog<bool>(
    context: context,
    builder: (_) => _GroupNameDialog(
      title: title,
      confirmLabel: confirmLabel,
      submit: submit,
      initial: initial,
      taken: taken,
      icon: icon,
    ),
  );
  return saved == true;
}

class _GroupNameDialog extends StatefulWidget {
  const _GroupNameDialog({
    required this.title,
    required this.confirmLabel,
    required this.submit,
    required this.initial,
    required this.taken,
    required this.icon,
  });
  final String title, confirmLabel, initial;
  final Future<String?> Function(String name) submit;
  final Set<String> taken;
  final IconData icon;

  @override
  State<_GroupNameDialog> createState() => _GroupNameDialogState();
}

class _GroupNameDialogState extends State<_GroupNameDialog> {
  late final _name = TextEditingController(text: widget.initial)
    ..selection = TextSelection(
      baseOffset: 0,
      extentOffset: widget.initial.length,
    );
  String? _error;
  bool _busy = false;

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    if (_busy) return;
    final name = _name.text.trim();
    final String? problem;
    if (name.isEmpty) {
      problem = l10n.enterGroupName;
    } else if (!DesktopGroup.validName(name)) {
      problem = l10n.groupNameRules;
    } else if (DesktopGroup.nameKey(name) !=
            DesktopGroup.nameKey(widget.initial) &&
        widget.taken.contains(DesktopGroup.nameKey(name))) {
      problem = l10n.groupNameTaken;
    } else {
      problem = null;
    }
    if (problem != null) {
      setState(() => _error = problem);
      return;
    }
    if (widget.initial.isNotEmpty && name == widget.initial.trim()) {
      Navigator.pop(context, false);
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    final failure = await widget.submit(name);
    if (!mounted) return;
    if (failure == null) {
      Navigator.pop(context, true);
    } else {
      setState(() {
        _busy = false;
        _error = failure;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return AppDialog(
      key: const Key('group-name-dialog'),
      icon: widget.icon,
      busy: _busy,
      title: widget.title,
      body: [
        AppField(
          label: l10n.groupName,
          error: _error,
          errorKey: const Key('group-name-error'),
          child: TextField(
            key: const Key('group-name-input'),
            controller: _name,
            autofocus: true,
            enabled: !_busy,
            maxLength: DesktopGroup.maxName,
            textInputAction: TextInputAction.done,
            onSubmitted: (_) => unawaited(_save()),
            onChanged: (_) {
              if (_error != null) setState(() => _error = null);
            },
            style: uiText(size: 14, color: p.ink),
            decoration: AppField.decoration(context),
          ),
        ),
      ],
      actions: [
        TextButton(
          key: const Key('group-name-cancel'),
          onPressed: _busy ? null : () => Navigator.pop(context, false),
          style: AppButtons.quiet(p),
          child: KeepAllText(l10n.cancel),
        ),
        FilledButton(
          key: const Key('group-name-save'),
          onPressed: _busy ? null : () => unawaited(_save()),
          style: AppButtons.filled(p),
          child: KeepAllText(widget.confirmLabel),
        ),
      ],
    );
  }
}

/// The changes to this account's group list both launchers make: each one
/// saves the whole list ([save]) in its new order.
class DesktopGroupFlows {
  const DesktopGroupFlows(this.groups, this.save);
  final List<DesktopGroup> groups;
  final SaveDesktopGroups save;

  /// The names in use, as [DesktopGroup.nameKey].
  Set<String> get _names => {
    for (final group in groups) DesktopGroup.nameKey(group.name),
  };

  /// 새 그룹…: asks for the name and saves the group; resolves to its id
  /// (null: cancelled).
  Future<String?> create(BuildContext context) async {
    String? created;
    final known = {for (final group in groups) group.id};
    final saved = await showDesktopGroupNameDialog(
      context,
      title: l10n.newGroup,
      confirmLabel: l10n.create,
      taken: _names,
      submit: (name) async {
        try {
          final list = await save([
            ...groupDrafts(groups),
            (id: null, name: name),
          ]);
          final fresh = list.where((group) => !known.contains(group.id));
          created =
              (fresh.where((group) => group.name == name).firstOrNull ??
                      fresh.firstOrNull)
                  ?.id;
          return null;
        } catch (error) {
          return desktopGroupFailureText(error);
        }
      },
    );
    return saved ? created : null;
  }

  /// 이름 바꾸기.
  Future<void> rename(BuildContext context, DesktopGroup group) async {
    await showDesktopGroupNameDialog(
      context,
      title: l10n.renameGroup,
      confirmLabel: l10n.rename,
      initial: group.name,
      icon: Icons.drive_file_rename_outline_rounded,
      taken: _names..remove(DesktopGroup.nameKey(group.name)),
      submit: (name) async {
        try {
          await save([
            for (final item in groups)
              (id: item.id, name: item.id == group.id ? name : item.name),
          ]);
          return null;
        } catch (error) {
          return desktopGroupFailureText(error);
        }
      },
    );
  }

  /// 위로 옮기기 (-1) / 아래로 옮기기 (+1). Throws when it is not saved.
  Future<void> move(DesktopGroup group, int by) async {
    final order = [...groups];
    final from = order.indexWhere((item) => item.id == group.id);
    final to = from + by;
    if (from < 0 || to < 0 || to >= order.length) return;
    order.insert(to, order.removeAt(from));
    await save(groupDrafts(order));
  }

  /// 그룹 삭제 after its question; its desktops go to 기타 and nothing else
  /// is deleted. Resolves false when cancelled; throws when not saved.
  Future<bool> delete(BuildContext context, DesktopGroup group) async {
    final confirmed = await showAppConfirm(
      context,
      icon: Icons.folder_delete_outlined,
      tone: AppTone.danger,
      title: l10n.deleteGroupTitle(group.name),
      message: l10n.deleteGroupMessage,
      confirmLabel: l10n.delete,
      confirmKey: const Key('confirm-group-delete'),
      cancelKey: const Key('cancel-group-delete'),
    );
    if (!confirmed) return false;
    await save(
      groupDrafts([
        for (final item in groups)
          if (item.id != group.id) item,
      ]),
    );
    return true;
  }
}
