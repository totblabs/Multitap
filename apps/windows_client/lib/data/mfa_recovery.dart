/// One nullable, strict transport-shape helper shared by login/management.
/// No trimming/coercion/Unicode normalization or arbitrary separator removal.
String? normalizeMfaRecoveryCode(Object? value) {
  if (value is! String) { return null; }
  if (value.length == 32 && RegExp(r'^[A-Fa-f0-9]{32}$').hasMatch(value)) {
    return value.toLowerCase();
  }
  if (value.length == 39 && RegExp(r'^[A-Fa-f0-9]{4}(?:-[A-Fa-f0-9]{4}){7}$').hasMatch(value)) {
    return value.replaceAll('-', '').toLowerCase();
  }
  return null;
}
