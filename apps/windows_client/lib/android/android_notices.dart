import 'dart:async';

import 'package:flutter/material.dart';

import '../data/notices.dart';
import '../ui/app_theme.dart';
import '../ui/notice_widgets.dart';
import '../ui/workspace_header.dart' show HeaderControl;
import 'android_session_keepalive.dart';
import 'android_tv.dart';
import '../l10n/l10n.dart';

/// 공지 on Android (notice-v1, lib/data/notices.dart). The same notice as on
/// Windows: a banner in the launcher and over a stream, the bell in the
/// launcher's header, and, while the app is not in front, an ordinary
/// notification when notifications are already allowed (never a new
/// permission question).

/// What the app shares with the layer above its pages: this sign-in's notices
/// and whether a stream is on screen.
class AndroidNoticeHost extends ChangeNotifier {
  NoticeCenter? _center;
  bool _streaming = false, _disposed = false;
  NoticeCenter? get center => _center;
  bool get streaming => _streaming;

  /// The home may still say goodbye after the app above it is gone.
  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }

  void bind(NoticeCenter? center) {
    if (_disposed || identical(center, _center)) return;
    _center = center;
    notifyListeners();
  }

  set streaming(bool value) {
    if (_disposed || value == _streaming) return;
    _streaming = value;
    notifyListeners();
  }
}

/// The session's floating buttons rest against a screen edge and are about
/// this deep with their margin: a banner inset by it never covers them,
/// whichever edge they are on.
const androidNoticeStreamInset = 76.0;

/// The banner above every page (MaterialApp.builder): under the header in the
/// launcher; over a stream at the top, clear of the floating buttons' edge
/// strip. Only the card takes touches; it takes no focus (a TV remote or a
/// keyboard keeps driving what is under it).
class AndroidNoticeLayer extends StatefulWidget {
  const AndroidNoticeLayer({
    super.key,
    required this.host,
    required this.child,
    this.post = androidPostNotice,
  });
  final AndroidNoticeHost host;
  final Widget child;

  /// Posts the notification of a notice that arrived while the app was not in
  /// front (tests substitute it).
  final Future<bool> Function(BrokerNotice notice) post;

  @override
  State<AndroidNoticeLayer> createState() => _AndroidNoticeLayerState();
}

class _AndroidNoticeLayerState extends State<AndroidNoticeLayer>
    with WidgetsBindingObserver {
  NoticeCenter? _center;
  bool _front = true;
  final _posted = <String>{};

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    final state = WidgetsBinding.instance.lifecycleState;
    _front = state == null || state == AppLifecycleState.resumed;
    widget.host.addListener(_hostChanged);
    _hostChanged();
  }

  @override
  void didUpdateWidget(AndroidNoticeLayer old) {
    super.didUpdateWidget(old);
    if (old.host != widget.host) {
      old.host.removeListener(_hostChanged);
      widget.host.addListener(_hostChanged);
      _hostChanged();
    }
  }

  void _hostChanged() {
    final next = widget.host.center;
    if (!identical(next, _center)) {
      _center?.removeListener(_noticesChanged);
      _center = next;
      _posted.clear();
      next?.addListener(_noticesChanged);
    }
    if (mounted) setState(() {});
  }

  /// Not in front: the notice is said in a notification (once); its banner
  /// waits for the app to come back.
  void _noticesChanged() {
    final banner = _center?.banner;
    if (banner == null || _front || !_posted.add(banner.id)) return;
    unawaited(widget.post(banner).catchError((Object _) => false));
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    final front = state == AppLifecycleState.resumed;
    if (front == _front) return;
    _front = front;
    if (mounted) setState(() {});
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    widget.host.removeListener(_hostChanged);
    _center?.removeListener(_noticesChanged);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final center = _center;
    final padding = MediaQuery.paddingOf(context);
    final streaming = widget.host.streaming;
    final inset = streaming ? androidNoticeStreamInset : 12.0;
    // Always the same shape: the pages keep their place (and their state)
    // whether or not there are notices.
    return Stack(
      fit: StackFit.expand,
      children: [
        widget.child,
        if (center != null)
        Positioned(
          // Under the launcher's header; over a stream below the top strip.
          top: padding.top + (streaming ? androidNoticeStreamInset : 64),
          left: padding.left + inset,
          right: padding.right + inset,
          child: Align(
            alignment: Alignment.topCenter,
            child: NoticeBanner(
              key: const Key('android-notice-banner'),
              center: center,
              onStream: streaming,
              active: _front,
              maxWidth: 360,
            ),
          ),
        ),
      ],
    );
  }
}

/// The bell of the Android launcher's header: a 48 dp target, the TV focus
/// frame, the same list as on Windows.
class AndroidNoticeBell extends StatelessWidget {
  const AndroidNoticeBell({super.key, required this.center});
  final NoticeCenter center;

  /// What the header reserves for it.
  static const width = 48.0;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final touch = HeaderControl.quiet(p).copyWith(
      minimumSize: const WidgetStatePropertyAll(Size.square(48)),
      maximumSize: const WidgetStatePropertyAll(
        Size(double.infinity, double.infinity),
      ),
    );
    return AndroidTvFocus(
      builder: (context, focused) => NoticeBell(
        center: center,
        iconSize: 22,
        touch: true,
        style: AndroidTvFocus.style(context, touch),
      ),
    );
  }
}

/// An ordinary notification for [notice] through the app's existing native
/// channel. The native side posts it only when notifications are already
/// allowed and never asks; false when it did not (not allowed, an older
/// native part, any failure).
Future<bool> androidPostNotice(BrokerNotice notice) async {
  try {
    return await AndroidSessionKeepalive.channel
            .invokeMethod<bool>('notice', {
              'id': notice.id.hashCode & 0x3ff,
              'title': notice.kind == NoticeKind.maintenance
                  ? l10n.maintenanceNotice(notice.title)
                  : notice.title,
              'body': notice.body,
            })
            .timeout(const Duration(seconds: 3)) ==
        true;
  } catch (_) {
    // An older native part, a refused call, no answer: no notification.
    return false;
  }
}
