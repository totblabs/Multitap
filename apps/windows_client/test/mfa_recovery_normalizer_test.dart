// NEW-helper unit source, UNCOMPILED. Run against proposed helper; the API/UI
// regressions remain separate so their original-product RED can compile.
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/mfa_auth.dart';
void main() {
  test('shared recovery normalizer accepts only raw32 or exact eight groups and lowers case', () {
    const lower = '0123456789abcdef0123456789abcdef';
    expect(normalizeMfaRecoveryCode(lower), lower);
    expect(normalizeMfaRecoveryCode(lower.toUpperCase()), lower);
    expect(normalizeMfaRecoveryCode('0123-4567-89AB-CDEF-0123-4567-89AB-CDEF'), lower);
    for (final value in <Object?>[null, 123, {}, '', 'a' * 31, 'a' * 33, 'a' * 64,
      '$lower\n', ' $lower', '$lower ', 'Ａ' * 32,
      '01234567-89ab-cdef-0123-456789abcdef', '0123--4567-89ab-cdef-0123-4567-89ab-cdef']) {
      expect(normalizeMfaRecoveryCode(value), isNull);
    }
  });
}
