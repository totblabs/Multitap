#pragma once
// Windows client/Host release signatures (multitap-windows-release-signature-v1).
//
// Every published installer is signed with the project's own Ed25519 key
// (tools/windows-release-signature.mjs; the private key stays on the release
// PC). The public half is pinned here, and client and Host builds refuse an
// installer whose catalog signature does not verify against it. This is not
// Authenticode, a purchased certificate or Windows trust: nothing is imported
// into Windows and no external service is involved.
//
// The signed message binds the purpose (domain line), product, version, exact
// installer file name, exact size and SHA-256. The downloaded bytes are bound
// to it by the existing exact size and SHA-256 checks.
//
// Ed25519 verification below follows TweetNaCl (public domain, Bernstein et
// al.), verify-only, with the RFC 8032 S < L check added. SHA-512 is Windows
// CNG. All inputs are public, so constant time is not required.
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace multitap::release_signature {
// tools/windows-release-signature.mjs reads this exact constant to refuse
// signing with any other key.
inline constexpr char kReleasePublicKeyHex[] = "d563e35c391aa29b6739309fae28a8a09a9a7736ed8ac776cef0a82e3589c50c";
inline constexpr char kSignatureDomain[] = "multitap-windows-release-signature-v1";
using Key = std::array<uint8_t, 32>;
using Signature = std::array<uint8_t, 64>;

namespace detail {
using i64 = long long;
struct Gf { i64 v[16]; };
using Point = std::array<Gf, 4>;
inline constexpr Gf kZero{}, kOne{{1}};
inline constexpr Gf kD{{0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203}};
inline constexpr Gf kD2{{0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406}};
inline constexpr Gf kX{{0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169}};
inline constexpr Gf kY{{0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666}};
inline constexpr Gf kI{{0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83}};
// The group order L = 2^252 + 27742317777372353535851937790883648493, little endian.
inline constexpr i64 kL[32]{0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

inline void carry(Gf& o) {
  for (int i = 0; i < 16; ++i) {
    o.v[i] += i64(1) << 16;
    const i64 c = o.v[i] >> 16;
    o.v[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o.v[i] -= c * 65536;
  }
}
inline void conditionalSwap(Gf& p, Gf& q, int bit) {
  const i64 mask = ~(i64(bit) - 1);
  for (int i = 0; i < 16; ++i) { const i64 t = mask & (p.v[i] ^ q.v[i]); p.v[i] ^= t; q.v[i] ^= t; }
}
inline void pack(uint8_t out[32], const Gf& n) {
  Gf m{}, t = n;
  carry(t); carry(t); carry(t);
  for (int j = 0; j < 2; ++j) {
    m.v[0] = t.v[0] - 0xffed;
    for (int i = 1; i < 15; ++i) { m.v[i] = t.v[i] - 0xffff - ((m.v[i - 1] >> 16) & 1); m.v[i - 1] &= 0xffff; }
    m.v[15] = t.v[15] - 0x7fff - ((m.v[14] >> 16) & 1);
    const int b = int((m.v[15] >> 16) & 1);
    m.v[14] &= 0xffff;
    conditionalSwap(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) { out[2 * i] = uint8_t(t.v[i] & 0xff); out[2 * i + 1] = uint8_t((t.v[i] >> 8) & 0xff); }
}
inline bool equal(const Gf& a, const Gf& b) {
  uint8_t x[32], y[32]; pack(x, a); pack(y, b);
  return std::memcmp(x, y, 32) == 0;
}
inline int parity(const Gf& a) { uint8_t d[32]; pack(d, a); return d[0] & 1; }
inline void unpack(Gf& o, const uint8_t n[32]) {
  for (int i = 0; i < 16; ++i) o.v[i] = n[2 * i] + (i64(n[2 * i + 1]) << 8);
  o.v[15] &= 0x7fff;
}
inline Gf add(const Gf& a, const Gf& b) { Gf o; for (int i = 0; i < 16; ++i) o.v[i] = a.v[i] + b.v[i]; return o; }
inline Gf sub(const Gf& a, const Gf& b) { Gf o; for (int i = 0; i < 16; ++i) o.v[i] = a.v[i] - b.v[i]; return o; }
inline Gf mul(const Gf& a, const Gf& b) {
  i64 t[31]{};
  for (int i = 0; i < 16; ++i) for (int j = 0; j < 16; ++j) t[i + j] += a.v[i] * b.v[j];
  for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
  Gf o; for (int i = 0; i < 16; ++i) o.v[i] = t[i];
  carry(o); carry(o);
  return o;
}
inline Gf square(const Gf& a) { return mul(a, a); }
inline Gf inverse(const Gf& in) {
  Gf c = in;
  for (int a = 253; a >= 0; --a) { c = square(c); if (a != 2 && a != 4) c = mul(c, in); }
  return c;
}
inline Gf pow2523(const Gf& in) {
  Gf c = in;
  for (int a = 250; a >= 0; --a) { c = square(c); if (a != 1) c = mul(c, in); }
  return c;
}
// p = p + q (extended coordinates).
inline void pointAdd(Point& p, const Point& q) {
  const Gf a = mul(sub(p[1], p[0]), sub(q[1], q[0]));
  const Gf b = mul(add(p[0], p[1]), add(q[0], q[1]));
  const Gf c = mul(mul(p[3], q[3]), kD2);
  Gf d = mul(p[2], q[2]); d = add(d, d);
  const Gf e = sub(b, a), f = sub(d, c), g = add(d, c), h = add(b, a);
  p[0] = mul(e, f); p[1] = mul(h, g); p[2] = mul(g, f); p[3] = mul(e, h);
}
inline void pointSwap(Point& p, Point& q, int bit) { for (int i = 0; i < 4; ++i) conditionalSwap(p[i], q[i], bit); }
inline void pointPack(uint8_t r[32], const Point& p) {
  const Gf zi = inverse(p[2]), tx = mul(p[0], zi), ty = mul(p[1], zi);
  pack(r, ty);
  r[31] ^= uint8_t(parity(tx) << 7);
}
inline void scalarMultiply(Point& p, Point q, const uint8_t s[32]) {
  p = {kZero, kOne, kOne, kZero};
  for (int i = 255; i >= 0; --i) {
    const int bit = (s[i / 8] >> (i & 7)) & 1;
    pointSwap(p, q, bit); pointAdd(q, p); pointAdd(p, p); pointSwap(p, q, bit);
  }
}
inline void scalarBase(Point& p, const uint8_t s[32]) { scalarMultiply(p, Point{kX, kY, kOne, mul(kX, kY)}, s); }
inline void modL(uint8_t r[32], i64 x[64]) {
  for (int i = 63; i >= 32; --i) {
    i64 c = 0; int j = i - 32;
    for (; j < i - 12; ++j) { x[j] += c - 16 * x[i] * kL[j - (i - 32)]; c = (x[j] + 128) >> 8; x[j] -= c * 256; }
    x[j] += c; x[i] = 0;
  }
  i64 c = 0;
  for (int j = 0; j < 32; ++j) { x[j] += c - (x[31] >> 4) * kL[j]; c = x[j] >> 8; x[j] &= 255; }
  for (int j = 0; j < 32; ++j) x[j] -= c * kL[j];
  for (int i = 0; i < 32; ++i) { x[i + 1] += x[i] >> 8; r[i] = uint8_t(x[i] & 255); }
}
inline void reduce(uint8_t r[64]) {
  i64 x[64]; for (int i = 0; i < 64; ++i) x[i] = r[i];
  std::memset(r, 0, 64); modL(r, x);
}
// -A from its encoding; false for an encoding that is not a curve point.
inline bool unpackNegative(Point& r, const uint8_t p[32]) {
  r[2] = kOne; unpack(r[1], p);
  Gf num = square(r[1]), den = mul(num, kD);
  num = sub(num, r[2]); den = add(r[2], den);
  const Gf den2 = square(den), den4 = square(den2), den6 = mul(den4, den2);
  Gf t = mul(mul(den6, num), den);
  t = pow2523(t); t = mul(mul(mul(t, num), den), den);
  r[0] = mul(t, den);
  if (!equal(mul(square(r[0]), den), num)) r[0] = mul(r[0], kI);
  if (!equal(mul(square(r[0]), den), num)) return false;
  if (parity(r[0]) == (p[31] >> 7)) r[0] = sub(kZero, r[0]);
  r[3] = mul(r[0], r[1]);
  return true;
}
// RFC 8032: S must be below the group order (no malleable second encoding).
inline bool scalarCanonical(const uint8_t s[32]) {
  for (int i = 31; i >= 0; --i) { if (s[i] < kL[i]) return true; if (s[i] > kL[i]) return false; }
  return false;
}
inline bool sha512(const std::vector<uint8_t>& data, uint8_t out[64]) noexcept {
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; bool ok = false;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA512_ALGORITHM, nullptr, 0) < 0) return false;
  DWORD objectSize = 0, got = 0;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &got, 0) >= 0) {
    std::vector<uint8_t> object(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0) {
      ok = BCryptHashData(hash, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0) >= 0 && BCryptFinishHash(hash, out, 64, 0) >= 0;
      BCryptDestroyHash(hash);
    }
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok;
}
inline int hexValue(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
inline int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}
}  // namespace detail

inline Key PinnedKey() {
  Key key{};
  for (size_t i = 0; i < key.size(); ++i) key[i] = uint8_t(detail::hexValue(kReleasePublicKeyHex[2 * i]) * 16 + detail::hexValue(kReleasePublicKeyHex[2 * i + 1]));
  return key;
}

// The exact signed bytes, or nothing when a field is outside the catalog
// rules (so no value can add or change a line).
inline std::optional<std::string> Message(const std::string& product, const std::string& version, const std::string& filename,
                                          uint64_t size, const std::string& sha256) {
  if (product != "client" && product != "host") return std::nullopt;
  int parts = 0, digits = 0;
  for (const char c : version) {
    if (c >= '0' && c <= '9') { if (++digits > 6) return std::nullopt; }
    else if (c == '.' && digits && parts < 2) { ++parts; digits = 0; }
    else return std::nullopt;
  }
  if (parts != 2 || !digits) return std::nullopt;
  if (filename != "Multitap" + std::string(product == "host" ? "-Host" : "") + "-" + version + "-windows-x64.exe") return std::nullopt;
  if (!size || size > 128ULL * 1024 * 1024 || sha256.size() != 64) return std::nullopt;
  for (const char c : sha256) if (detail::hexValue(c) < 0) return std::nullopt;
  return std::string(kSignatureDomain) + "\nproduct=" + product + "\nversion=" + version + "\nfilename=" + filename +
      "\nsize=" + std::to_string(size) + "\nsha256=" + sha256 + "\n";
}

// Canonical base64 of exactly 64 bytes: 86 symbols, "==", zero padding bits.
inline bool DecodeSignature(std::string_view text, Signature& out) {
  if (text.size() != 88 || text[86] != '=' || text[87] != '=') return false;
  uint32_t buffer = 0; int bits = 0; size_t at = 0;
  for (size_t i = 0; i < 86; ++i) {
    const int value = detail::base64Value(text[i]);
    if (value < 0) return false;
    buffer = (buffer << 6) | uint32_t(value); bits += 6;
    if (bits >= 8) { bits -= 8; out[at++] = uint8_t((buffer >> bits) & 0xff); }
  }
  return at == 64 && bits == 4 && (buffer & 0xf) == 0;
}

inline bool VerifyEd25519(std::string_view message, const Signature& signature, const Key& key) {
  using namespace detail;
  if (!scalarCanonical(signature.data() + 32)) return false;
  Point q;
  if (!unpackNegative(q, key.data())) return false;
  std::vector<uint8_t> input;
  input.reserve(64 + message.size());
  input.insert(input.end(), signature.begin(), signature.begin() + 32);
  input.insert(input.end(), key.begin(), key.end());
  input.insert(input.end(), message.begin(), message.end());
  uint8_t h[64];
  if (!sha512(input, h)) return false;
  reduce(h);
  Point p, base;
  scalarMultiply(p, q, h);
  scalarBase(base, signature.data() + 32);
  pointAdd(p, base);
  uint8_t r[32];
  pointPack(r, p);
  return std::memcmp(r, signature.data(), 32) == 0;
}

// A release passes only with a well-formed signature over exactly these
// fields under the pinned key (or a test key).
inline bool VerifyRelease(const std::string& product, const std::string& version, const std::string& filename, uint64_t size,
                          const std::string& sha256, std::string_view signatureText, const Key& key = PinnedKey()) {
  try {
    const auto message = Message(product, version, filename, size, sha256);
    Signature signature{};
    return message && DecodeSignature(signatureText, signature) && VerifyEd25519(*message, signature, key);
  } catch (...) { return false; }
}
}  // namespace multitap::release_signature
