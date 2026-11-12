import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:qr/qr.dart';

import '../data/mfa_account.dart';
import '../data/mfa_account_controller.dart';
import '../l10n/l10n.dart';

/// An in-app view of the exact current setup URI. No link/share/export action.
/// [visible] is a pure presentation predicate, never authentication activity.
class MfaEnrollmentQr extends StatefulWidget {
  const MfaEnrollmentQr({super.key, required this.controller,
    required this.enrollment, required this.visible});
  final MfaAccountController controller;
  final MfaEnrollment enrollment;
  final bool Function() visible;
  @override
  State<MfaEnrollmentQr> createState() => _MfaEnrollmentQrState();
}

class _QrLifetime {
  _QrLifetime(this.allowed);
  final bool Function() allowed;
  QrImage? image;
  bool retired = false, encodingFailed = false;
  bool get current {
    if (retired) { return false; }
    try { return allowed() && !retired; } catch (_) { return false; }
  }
  void forget() { retired = true; image = null; }
}

class _MfaEnrollmentQrState extends State<MfaEnrollmentQr> {
  _QrLifetime? _lifetime;
  @override
  void initState() { super.initState(); widget.controller.addListener(_changed); }
  @override
  void didUpdateWidget(covariant MfaEnrollmentQr oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.controller, widget.controller) ||
        !identical(oldWidget.enrollment, widget.enrollment)) {
      oldWidget.controller.removeListener(_changed);
      _forget(); widget.controller.addListener(_changed);
    }
  }
  void _forget() { _lifetime?.forget(); _lifetime = null; }
  void _changed() {
    if (_lifetime?.current != true) { _forget(); }
    if (mounted) { setState(() {}); }
  }
  @override
  void dispose() { _forget(); widget.controller.removeListener(_changed); super.dispose(); }
  bool _current(MfaAccountController controller, MfaEnrollment enrollment) {
    if (!mounted || !identical(widget.controller, controller) ||
        !identical(widget.enrollment, enrollment)) { return false; }
    try { return widget.visible() && controller.canDisplayEnrollment(enrollment); }
    catch (_) { return false; }
  }
  _QrLifetime? _image() {
    final controller = widget.controller, enrollment = widget.enrollment;
    if (!_current(controller, enrollment)) { _forget(); return null; }
    var lifetime = _lifetime;
    if (lifetime == null || lifetime.retired) {
      lifetime = _QrLifetime(() => _current(controller, enrollment));
      _lifetime = lifetime;
    }
    if (lifetime.image == null && !lifetime.encodingFailed) {
      try {
        // Encode only this validated DTO's URI. Never reconstruct its secret,
        // username/query or route, and never put the URI in a widget key/log.
        final uri = enrollment.uri;
        if (uri == null || !lifetime.current) { lifetime.forget(); return null; }
        final image = QrImage(QrCode(payload: QrPayload.fromString(uri.toString()),
          errorCorrectLevel: QrErrorCorrectLevel.medium));
        if (!lifetime.current || image.moduleCount < 21 || image.moduleCount > 177) {
          lifetime.forget(); return null;
        }
        lifetime.image = image;
      } catch (_) {
        // Encoding errors can contain implementation data; expose fixed copy.
        lifetime.image = null; lifetime.encodingFailed = true;
      }
    }
    return lifetime;
  }
  Widget _manualFallback() => Text(
    l10n.cannotShowQrCodeEnter,
    key: Key('mfa-qr-unavailable'), textAlign: TextAlign.center);
  @override
  Widget build(BuildContext context) {
    final lifetime = _image();
    if (lifetime == null || !lifetime.current) { return const SizedBox.shrink(); }
    final image = lifetime.image;
    if (image == null) { return _manualFallback(); }
    final observedDpr = MediaQuery.devicePixelRatioOf(context);
    final dpr = observedDpr.isFinite && observedDpr > 0 ? observedDpr : 1.0;
    return LayoutBuilder(builder: (context, constraints) {
      if (!lifetime.current) { lifetime.forget(); return const SizedBox.shrink(); }
      var available = 240.0;
      if (constraints.maxWidth.isFinite) { available = math.min(available, constraints.maxWidth); }
      if (constraints.maxHeight.isFinite) { available = math.min(available, constraints.maxHeight); }
      final modules = image.moduleCount + 8; // Four white modules on EACH side.
      final modulePixels = ((available * dpr) / modules).floor();
      if (modulePixels < 1) { return _manualFallback(); }
      final side = modules * modulePixels / dpr;
      return Center(heightFactor: 1, child: Semantics(image: true,
        label: l10n.setupQrCodeScanAuthenticator,
        child: SizedBox.square(dimension: side, child: CustomPaint(
          key: const Key('mfa-enrollment-qr-paint'),
          painter: _EnrollmentQrPainter(lifetime, dpr, modulePixels),
        ))));
    });
  }
}

class _EnrollmentQrPainter extends CustomPainter {
  _EnrollmentQrPainter(this.lifetime, this.dpr, this.modulePixels);
  final _QrLifetime lifetime;
  final double dpr;
  final int modulePixels;
  @override
  void paint(Canvas canvas, Size size) {
    // Clear even a retained old painter before considering any sensitive work.
    canvas.drawRect(Offset.zero & size, Paint()..color = Colors.white..isAntiAlias = false);
    if (!lifetime.current) { lifetime.forget(); return; }
    final image = lifetime.image;
    if (image == null) { return; }
    final module = modulePixels / dpr, border = 4 * module;
    final black = Paint()..color = Colors.black..isAntiAlias = false;
    for (var row = 0; row < image.moduleCount; row++) {
      for (var column = 0; column < image.moduleCount; column++) {
        if (image.isDark(row, column)) {
          canvas.drawRect(Rect.fromLTWH(border + column * module,
            border + row * module, module, module), black);
        }
      }
    }
  }
  @override
  bool shouldRepaint(covariant _EnrollmentQrPainter oldDelegate) =>
      !identical(lifetime, oldDelegate.lifetime) || dpr != oldDelegate.dpr ||
      modulePixels != oldDelegate.modulePixels;
}
