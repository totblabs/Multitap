// UDP media core checks: published vectors, framing, repair logic and a simulated
// link. Nothing here opens a socket. The link table is a simulation with a fixed
// seed, not a network measurement; the AES figure is this PC only.
#include "../../../native/windows_video/udp_media_crypto.hpp"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <new>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace multitap::udp {
struct TestAccess {
  // CNG takes a private blob only with the matching public key, in RFC 7748 byte
  // order, and the scalar clamped: found by trial on this PC.
  static bool importPrivate(X25519& pair, const std::vector<uint8_t>& scalar, const std::vector<uint8_t>& publicKey) {
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 3 * keyBytes> blob{};
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDH_PRIVATE_GENERIC_MAGIC; header->cbKey = keyBytes;
    std::memcpy(blob.data() + sizeof(*header), publicKey.data(), keyBytes);
    uint8_t* d = blob.data() + sizeof(*header) + 2 * keyBytes;
    std::memcpy(d, scalar.data(), keyBytes); d[0] &= 248; d[31] = uint8_t((d[31] & 127) | 64);
    BCryptDestroyKey(pair.key_); pair.key_ = nullptr; pair.valid_ = false;
    if (BCryptImportKeyPair(pair.algorithm_, nullptr, BCRYPT_ECCPRIVATE_BLOB, &pair.key_, blob.data(), ULONG(blob.size()), 0) < 0) return false;
    std::memcpy(pair.public_.data(), publicKey.data(), keyBytes); pair.valid_ = true;
    return true;
  }
  // The raw primitive with CNG's validation off. The product keeps it on, and
  // CNG then refuses the section 5.2 u-coordinates (they lie on the twist).
  static bool rawSecret(const X25519& pair, std::vector<uint8_t> u, std::vector<uint8_t>& out) {
    u[31] &= 0x7f; u.resize(2 * keyBytes);
    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB)); blob.insert(blob.end(), u.begin(), u.end());
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC; header->cbKey = keyBytes;
    BCRYPT_KEY_HANDLE other = nullptr; BCRYPT_SECRET_HANDLE secret = nullptr; ULONG size = 0; out.assign(keyBytes, 0);
    const bool ok = BCryptImportKeyPair(pair.algorithm_, nullptr, BCRYPT_ECCPUBLIC_BLOB, &other, blob.data(), ULONG(blob.size()), BCRYPT_NO_KEY_VALIDATION) >= 0 &&
                    BCryptSecretAgreement(pair.key_, other, &secret, 0) >= 0 && BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, out.data(), keyBytes, &size, 0) >= 0 && size == keyBytes;
    if (secret) BCryptDestroySecret(secret);
    if (other) BCryptDestroyKey(other);
    return ok;
  }
  static void nextSequence(Sealer& sealer, uint64_t next) { sealer.next_ = next; }
};
}

using namespace multitap::udp;
using Bytes = std::vector<uint8_t>;

namespace {
int cases = 0;
long long checks = 0;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); ++checks; }
Bytes hex(const char* text) {
  Bytes result;
  for (; text[0] && text[1]; text += 2) {
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    result.push_back(uint8_t(digit(text[0]) << 4 | digit(text[1])));
  }
  return result;
}
PublicKey key(const char* text) { const auto bytes = hex(text); PublicKey value{}; std::memcpy(value.data(), bytes.data(), value.size()); return value; }
bool same(const uint8_t* a, const Bytes& b) { return !std::memcmp(a, b.data(), b.size()); }

// Video or audio message exactly as the Host's mediaPacket builds it, with the epoch set.
Bytes media(uint8_t kind, bool key, uint16_t epoch, uint64_t pts, size_t payload, uint32_t seed = 1) {
  Bytes value(12 + payload); value[0] = kind; value[1] = key ? 1 : 0; value[2] = uint8_t(epoch); value[3] = uint8_t(epoch >> 8);
  for (unsigned n = 0; n < 8; ++n) value[4 + n] = uint8_t(pts >> (8 * n));
  uint32_t state = seed * 2654435761u + 1;
  for (size_t n = 12; n < value.size(); ++n) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; value[n] = uint8_t(state); }
  return value;
}
Bytes video(uint64_t pts, size_t payload = 2000, bool key = false, uint16_t epoch = 1, uint32_t seed = 1) { return media(1, key, epoch, pts, payload, seed); }
Bytes pcm(uint64_t pts, size_t payload = 1920, uint16_t epoch = 1) { return media(2, false, epoch, pts, payload, 7); }
std::vector<Bytes> split(uint8_t cls, uint32_t id, const Bytes& message) {
  std::vector<Bytes> result;
  for (size_t i = 0; i < fragmentCount(message.size()); ++i) { Plain p; writeFragment(p, cls, id, message, uint16_t(i)); result.emplace_back(p.bytes.data(), p.bytes.data() + p.size); }
  return result;
}
constexpr uint64_t testChallenge = 0x1122334455667788ull;
uint64_t fixedRandom() { return testChallenge; }
Bytes tailFor(uint32_t id, uint64_t hostUs, uint64_t challenge = testChallenge, uint32_t rtt = 0) { Bytes p(tailBytes); p[0] = kindTail; put32(&p[2], id); put64(&p[6], hostUs); put64(&p[14], challenge); put32(&p[22], rtt); return p; }
Bytes feedbackFor(uint32_t received, uint32_t highest = 0, uint32_t lost = 0, uint64_t echo = 0, uint32_t hold = 0, uint64_t challenge = testChallenge) {
  Bytes p(feedbackBytes); p[0] = kindFeedback; put32(&p[2], highest); put32(&p[6], received); put32(&p[10], lost); put64(&p[14], echo); put32(&p[22], hold); put64(&p[26], challenge); return p;
}
Bytes nackFor(uint32_t id, uint16_t first, uint16_t run) { Bytes p(10); p[0] = kindNack; p[1] = 1; put32(&p[2], id); put16(&p[6], first); put16(&p[8], run); return p; }
// A Sender past the address proof, by the real path: its TAIL, echoed.
Sender provenSender(int target = 20000000, uint64_t now = 0) {
  Sender sender(target, now, fixedRandom); Plain p;
  require(sender.next(now, p) && p.bytes[0] == kindTail && p.size == tailBytes, "No challenge went out");
  const auto f = feedbackFor(0, 0, 0, 0, 0, get64(&p.bytes[14])); sender.control(f.data(), f.size(), now);
  require(sender.proven(), "A valid echo did not prove the address");
  return sender;
}

// A Receiver with its output kept, and a clock.
struct Client {
  Receiver receiver;
  Receiver::Output out;
  uint64_t now = 1000000;
  // A given RTT arrives the only way it can: in a TAIL.
  explicit Client(uint64_t rtt = 0, uint16_t epoch = 1) : receiver(epoch, 1000000) { if (rtt) feed(tailFor(0, 1, testChallenge, uint32_t(rtt))); }
  void feed(const Bytes& plain) { receiver.datagram(plain.data(), plain.size(), now, out); }
  void feed(const std::vector<Bytes>& plains) { for (const auto& p : plains) feed(p); }
  void tick() { receiver.tick(now, out); }
  void at(uint64_t us) { now = us; tick(); }
  size_t controls(uint8_t kind) const { size_t n = 0; for (const auto& c : out.control) n += c[0] == kind; return n; }
  std::vector<Bytes> taken(uint8_t kind) { std::vector<Bytes> result; for (const auto& c : out.control) if (c[0] == kind) result.push_back(c); return result; }
  size_t videos() const { size_t n = 0; for (const auto& m : out.messages) n += m.video; return n; }
  void clear() { out.messages.clear(); out.control.clear(); }
};

void rfc7748() {
  const auto a = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"), aPublic = hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  const auto b = hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"), bPublic = hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  const auto shared = hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  {  // Section 6.1, both sides, through a fixed private key.
    X25519 alice, bob;
    require(alice.valid() && bob.valid(), "CNG did not make a curve25519 pair");
    require(TestAccess::importPrivate(alice, a, aPublic) && TestAccess::importPrivate(bob, b, bPublic), "CNG refused a fixed RFC 7748 private key");
    Secret<keyBytes> one, two;
    require(alice.agree(key("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"), one), "Agreement failed");
    require(bob.agree(key("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"), two), "Agreement failed");
    require(same(one.data(), shared) && same(two.data(), shared), "The shared secret is not RFC 7748 section 6.1");
  }
  {  // Section 5.2. The public keys came from OpenSSL (node): the RFC lists none for these
     // scalars. CNG's secret is reversed from the RFC's bytes, and agree() reverses it back.
    struct Vector { const char* scalar; const char* publicKey; const char* u; const char* output; };
    const Vector vectors[] = {
      {"a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", "1c9fd88f45606d932a80c71824ae151d15d73e77de38e8e000852e614fae7019",
       "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"},
      {"4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", "ff63fe57bfbf43fa3f563628b149af704d3db625369c49983650347a6a71e00e",
       "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"},
    };
    for (const auto& v : vectors) {
      X25519 pair; Bytes raw; Secret<keyBytes> out; auto expected = hex(v.output);
      require(TestAccess::importPrivate(pair, hex(v.scalar), hex(v.publicKey)), "CNG refused a section 5.2 scalar");
      require(TestAccess::rawSecret(pair, hex(v.u), raw), "Section 5.2 agreement failed");
      std::reverse(expected.begin(), expected.end());
      require(raw == expected, "CNG's raw secret is not the reversed RFC 7748 output");
      require(!pair.agree(key(v.u), out), "A point on the twist was accepted");
    }
  }
  {  // RFC 7748 masks bit 255 of a received u-coordinate; CNG would refuse it.
    X25519 pair; Secret<keyBytes> out; require(TestAccess::importPrivate(pair, a, aPublic), "CNG refused a private key");
    PublicKey peer = key("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"); peer[31] |= 0x80;
    require(pair.agree(peer, out) && same(out.data(), shared), "Bit 255 of a public key was not masked");
  }
  {  // Fresh pairs meet each other.
    X25519 client, host; Secret<keyBytes> one, two;
    require(client.publicKey() != host.publicKey(), "Two fresh pairs matched");
    require(client.agree(host.publicKey(), one) && host.agree(client.publicKey(), two), "Fresh agreement failed");
    require(!std::memcmp(one.data(), two.data(), keyBytes), "Fresh agreement differs");
    require(!(client.publicKey()[31] & 0x80), "A public key has bit 255 set");
  }
  // Every small-order u-coordinate must fail, whichever layer refuses it.
  for (const char* point : {"0000000000000000000000000000000000000000000000000000000000000000", "0100000000000000000000000000000000000000000000000000000000000000",
                            "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800", "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
                            "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                            "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"}) {
    X25519 pair; Secret<keyBytes> out;
    require(!pair.agree(key(point), out), "A small-order public key gave a shared secret");
    uint8_t any = 0; for (size_t n = 0; n < keyBytes; ++n) any |= out.data()[n];
    require(!any, "A refused agreement left a secret behind");
  }
  ++cases;
}

void rfc5869() {
  struct Vector { const char* ikm; const char* salt; const char* info; const char* okm; };
  const Vector vectors[] = {
    {"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9",
     "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"},
    {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f",
     "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
     "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
     "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"},
    // Test case 3 has no salt: by RFC 5869 that is HashLen zero bytes.
    {"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "0000000000000000000000000000000000000000000000000000000000000000", "",
     "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"},
  };
  for (const auto& v : vectors) {
    const auto ikm = hex(v.ikm), salt = hex(v.salt), info = hex(v.info), okm = hex(v.okm);
    Bytes out(okm.size());
    require(hkdfSha256(salt.data(), salt.size(), ikm.data(), ikm.size(), info.data(), info.size(), out.data(), out.size()), "HKDF failed");
    require(out == okm, "HKDF output is not RFC 5869");
  }
  ++cases;
}

void nistGcm() {
  struct Vector { const char* key; const char* iv; const char* aad; const char* plain; const char* cipher; const char* tag; };
  // AES-256 cases 13, 14, 15 and 16 of the NIST GCM specification.
  const Vector vectors[] = {
    {"0000000000000000000000000000000000000000000000000000000000000000", "000000000000000000000000", "", "", "", "530f8afbc74536b9a963b4f1c4cb738b"},
    {"0000000000000000000000000000000000000000000000000000000000000000", "000000000000000000000000", "", "00000000000000000000000000000000", "cea7403d4d606b6e074ec5d3baf39d18", "d0d1c8a799996bf0265b98b5d48ab919"},
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", "",
     "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
     "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad", "b094dac5d93471bdec1a502270e3cc6c"},
    {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", "feedfacedeadbeeffeedfacedeadbeefabaddad2",
     "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
     "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662", "76fc6ece0f4e1768cddf8853bb2d551b"},
  };
  for (const auto& v : vectors) {
    const auto keyBytesIn = hex(v.key), iv = hex(v.iv), aad = hex(v.aad), plain = hex(v.plain), cipher = hex(v.cipher), tag = hex(v.tag);
    Aes256Gcm aes(keyBytesIn.data());
    require(aes.valid(), "CNG did not take the AES key");
    Bytes outCipher(plain.size() + 1), outTag(tagBytes), outPlain(plain.size() + 1);
    require(aes.seal(iv.data(), aad.data(), aad.size(), plain.data(), plain.size(), outCipher.data(), outTag.data()), "GCM seal failed");
    require(same(outCipher.data(), cipher) && outTag == tag, "GCM output is not the NIST vector");
    require(aes.open(iv.data(), aad.data(), aad.size(), cipher.data(), cipher.size(), tag.data(), outPlain.data()) && same(outPlain.data(), plain), "GCM open failed");
    auto badTag = tag; badTag[0] ^= 1;
    require(!aes.open(iv.data(), aad.data(), aad.size(), cipher.data(), cipher.size(), badTag.data(), outPlain.data()), "A bad tag opened");
  }
  ++cases;
}

constexpr std::array<uint8_t, legIdBytes> hostLeg{1, 2, 3, 4, 5, 6, 7, 8}, clientLeg{9, 10, 11, 12, 13, 14, 15, 16};
const char* const expectedOkm = "9cc448a5129c9f7cd41a4a8e69f17c4ae855055a968a2bfcf75b7ff6ecb1234534e07e518d53706d911384c7a0af96dad9f4d5074bb6796cd903de755e689dbacb08f0937879a79c";
void derive(Role role, SessionKeys& keys) {
  Secret<keyBytes> shared; const auto bytes = hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"); std::memcpy(shared.data(), bytes.data(), keyBytes);
  const std::string session = "session-1234";
  require(deriveKeys(shared, key("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"), key("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"),
                     reinterpret_cast<const uint8_t*>(session.data()), session.size(), role, keys), "Key derivation failed");
}
struct Pair {
  SessionKeys host, client;
  Sealer hostSeal, clientSeal;
  Opener hostOpen, clientOpen;
  Pair() : hostSeal((derive(Role::host, host), host.send), hostLeg), clientSeal((derive(Role::client, client), client.send), clientLeg), hostOpen(host.receive), clientOpen(client.receive) {}
};
void keySchedule() {
  // Known answer from OpenSSL (node crypto.hkdfSync), independent of CNG.
  const auto okm = hex(expectedOkm);
  SessionKeys client, host; derive(Role::client, client); derive(Role::host, host);
  require(same(client.send.key.data(), Bytes(okm.begin(), okm.begin() + 32)) && same(host.receive.key.data(), Bytes(okm.begin(), okm.begin() + 32)), "Client-to-Host key is not the first 32 bytes");
  require(same(host.send.key.data(), Bytes(okm.begin() + 32, okm.begin() + 64)) && same(client.receive.key.data(), Bytes(okm.begin() + 32, okm.begin() + 64)), "Host-to-client key is not the next 32 bytes");
  require(same(client.send.salt.data(), Bytes(okm.begin() + 64, okm.begin() + 68)) && same(host.receive.salt.data(), Bytes(okm.begin() + 64, okm.begin() + 68)), "Client-to-Host salt is wrong");
  require(same(host.send.salt.data(), Bytes(okm.begin() + 68, okm.end())) && same(client.receive.salt.data(), Bytes(okm.begin() + 68, okm.end())), "Host-to-client salt is wrong");
  require(std::memcmp(client.send.salt.data(), client.receive.salt.data(), saltBytes) != 0, "The directions share a nonce salt");
  {  // Consumed keys are gone and cannot start a second sealer.
    Sealer sealer(host.send, hostLeg); uint8_t out[maxDatagram]; const uint8_t plain[1] = {1};
    uint8_t any = 0; for (size_t n = 0; n < keyBytes; ++n) any |= host.send.key.data()[n];
    require(!any && !host.send.ready, "A consumed key was not wiped");
    Sealer again(host.send, hostLeg); require(!again.seal(plain, 1, out), "A consumed key sealed");
    Opener opener(host.send); require(!opener.open(out, sizeof(out), out), "A consumed key opened");
  }
  {  // Wiped on destruction: read the object's own storage after its destructor.
    alignas(Secret<32>) unsigned char storage[sizeof(Secret<32>)];
    auto* secret = new (storage) Secret<32>; std::memset(secret->data(), 0xa5, 32); secret->~Secret<32>();
    bool wiped = true; for (size_t n = 0; n < sizeof(storage); ++n) wiped = wiped && !storage[n];
    require(wiped, "Key material survived its destructor");
  }
  ++cases;
}

void framingLayout() {
  Pair p; const auto okm = hex(expectedOkm);
  for (size_t size : {size_t(1), size_t(16), size_t(17), size_t(1166), maxPlaintext}) {
    Bytes plain(size); for (size_t n = 0; n < size; ++n) plain[n] = uint8_t(n * 3);
    uint8_t out[maxDatagram], back[maxPlaintext]; const size_t sealed = p.hostSeal.seal(plain.data(), size, out);
    require(sealed == size + headerBytes + tagBytes && sealed <= maxDatagram, "Wrong datagram size");
    require(out[0] == 3 && !std::memcmp(out + 1, hostLeg.data(), legIdBytes), "Wrong relay header");
    // The exact contract: AAD bytes 0..16, nonce the direction's salt then the sequence, h2c key and salt.
    uint8_t nonce[nonceBytes]; std::memcpy(nonce, okm.data() + 68, 4); std::memcpy(nonce + 4, out + 9, 8);
    Aes256Gcm direct(okm.data() + 32);
    require(direct.open(nonce, out, 17, out + 17, size, out + 17 + size, back) && !std::memcmp(back, plain.data(), size), "The datagram is not the contract's AEAD layout");
    require(p.clientOpen.open(out, sealed, back) == size && !std::memcmp(back, plain.data(), size), "Round trip failed");
  }
  uint8_t out[maxDatagram + 16], plain[maxPlaintext + 1] = {};
  require(!p.hostSeal.seal(plain, maxPlaintext + 1, out), "Oversize plaintext sealed");
  require(!p.hostSeal.seal(plain, 0, out), "Empty plaintext sealed");
  // A failed seal must not burn or reuse a sequence: the sequence still counts up from 1.
  Pair q; std::vector<uint64_t> sequences;
  for (int n = 0; n < 2000; ++n) { require(q.clientSeal.seal(plain, 8, out) != 0, "Seal failed"); sequences.push_back(get64(out + 9)); }
  for (size_t n = 0; n < sequences.size(); ++n) require(sequences[n] == n + 1, "The sequence does not count up from 1");
  ++cases;
}

void tamper() {
  Pair p; uint8_t plain[32], datagram[maxDatagram], back[maxPlaintext];
  for (size_t n = 0; n < sizeof(plain); ++n) plain[n] = uint8_t(n);
  const size_t sealed = p.hostSeal.seal(plain, sizeof(plain), datagram);
  for (size_t byte = 0; byte < sealed; ++byte) for (unsigned bit = 0; bit < 8; ++bit) {
    uint8_t copy[maxDatagram]; std::memcpy(copy, datagram, sealed); copy[byte] ^= uint8_t(1 << bit);
    Pair fresh;
    require(!fresh.clientOpen.open(copy, sealed, back), "A tampered datagram opened");
    uint8_t any = 0; for (size_t n = 0; n < sizeof(plain); ++n) any |= back[n] & 0;
    require(fresh.clientOpen.open(datagram, sealed, back) == sizeof(plain), "A tampered datagram disturbed the window");
    (void)any;
  }
  // Truncated, extended, and the 25-byte probe.
  for (size_t size : {size_t(0), size_t(1), size_t(16), size_t(25), size_t(33), sealed - 1, sealed + 1}) require(!p.clientOpen.open(datagram, size, back), "A wrong size opened");
  require(!p.clientOpen.open(datagram, maxDatagram + 1, back), "An oversize datagram opened");
  // The other direction and another grant.
  Pair other; uint8_t reflected[maxDatagram]; const size_t size = p.clientSeal.seal(plain, sizeof(plain), reflected);
  require(!p.clientOpen.open(reflected, size, back), "A reflected datagram opened on its sender");
  require(!p.hostOpen.open(datagram, sealed, back), "A Host datagram opened on the Host");
  require(p.clientOpen.open(datagram, sealed, back) == sizeof(plain), "A real datagram did not open");
  {
    SessionKeys a, b; X25519 x, y; Secret<keyBytes> sa, sb;
    require(x.agree(y.publicKey(), sa) && y.agree(x.publicKey(), sb), "Agreement failed");
    const uint8_t session[] = "other";
    require(deriveKeys(sa, x.publicKey(), y.publicKey(), session, 5, Role::client, a) && deriveKeys(sb, x.publicKey(), y.publicKey(), session, 5, Role::host, b), "Derivation failed");
    Opener open(a.receive); require(!open.open(datagram, sealed, back), "Another grant's key opened it");
    Sealer seal(b.send, hostLeg); uint8_t own[maxDatagram]; const size_t n = seal.seal(plain, sizeof(plain), own); require(open.open(own, n, back) == sizeof(plain), "Fresh keys did not work");
  }
  ++cases;
}

void replay() {
  {
    ReplayWindow w;
    const auto take = [&](uint64_t s) { const bool f = w.fresh(s); if (f) w.accept(s); return f; };
    require(take(1) && !take(1), "A duplicate passed");
    require(take(3) && take(2) && !take(2) && !take(3), "Reordering is not handled");
    require(take(3000), "A jump forward failed");
    require(!take(3000 - 2048) && take(3000 - 2047) && !take(3000 - 2047), "The window is not exactly 2,048 sequences");
    require(take(3000 + 2048) && take(3001) && !take(3000), "A jump of exactly the window kept old bits or lost the edge");
    for (uint64_t s = 10000; s < 20000; ++s) require(take(s), "A rising sequence failed");
    for (uint64_t s = 19999; s > 19999 - 2048; --s) require(!take(s), "A sequence replayed inside the window");
    require(!take(19999 - 2048), "A sequence below the window passed");
    // The ring's slots repeat: far below the window, a free slot must not let one through.
    ReplayWindow lone; lone.accept(5000);
    for (uint64_t back = 2048; back < 5000; ++back) require(!lone.fresh(5000 - back), "A sequence below the window passed on a free slot");
    // Every bit of the ring is really cleared when the window moves over it.
    ReplayWindow ring; for (uint64_t s = 1; s < 10000; s += 2) { require(ring.fresh(s), "A cleared slot looked seen"); ring.accept(s); }
    require(ring.fresh(9998), "A skipped sequence inside the window was refused");
  }
  Pair p; uint8_t plain[8] = {1}, one[maxDatagram], two[maxDatagram], three[maxDatagram], back[maxPlaintext];
  const size_t a = p.hostSeal.seal(plain, 8, one), b = p.hostSeal.seal(plain, 8, two), c = p.hostSeal.seal(plain, 8, three);
  require(p.clientOpen.open(two, b, back) == 8 && !p.clientOpen.open(two, b, back), "A replayed datagram opened");
  require(p.clientOpen.open(one, a, back) == 8 && !p.clientOpen.open(one, a, back), "A reordered datagram failed, or replayed");
  // A forgery with a huge sequence must not move the window.
  uint8_t forged[maxDatagram]; std::memcpy(forged, three, c); put64(forged + 9, 1000000);
  require(!p.clientOpen.open(forged, c, back) && p.clientOpen.open(three, c, back) == 8, "A forgery moved the replay window");
  // Limits. Sequence 0 and sequences at 2^48 never open; the last valid one does.
  Pair q; uint8_t out[maxDatagram];
  TestAccess::nextSequence(q.hostSeal, sequenceLimit - 2);
  require(!q.hostSeal.exhausted(), "Exhausted early");
  const size_t last = q.hostSeal.seal(plain, 8, out);
  require(last && get64(out + 9) == sequenceLimit - 2, "Below the limit did not seal");
  require(q.clientOpen.open(out, last, back) == 8, "Near the limit did not open");
  const size_t top = q.hostSeal.seal(plain, 8, out);
  require(top && get64(out + 9) == sequenceLimit - 1 && q.clientOpen.open(out, top, back) == 8, "The last sequence failed");
  require(q.hostSeal.exhausted() && !q.hostSeal.seal(plain, 8, out), "A sequence reached 2^48");
  uint8_t bad[maxDatagram]; std::memcpy(bad, one, a);
  put64(bad + 9, 0); require(!p.clientOpen.open(bad, a, back), "Sequence 0 opened");
  Pair r; TestAccess::nextSequence(r.hostSeal, sequenceLimit); require(!r.hostSeal.seal(plain, 8, out), "Sealed at the limit");
  put64(bad + 9, sequenceLimit); require(!r.clientOpen.open(bad, a, back), "Sequence 2^48 opened");
  ++cases;
}

// Only the sealer picks a sequence: the API takes none, and the Sender only ever holds plaintext.
static_assert(std::is_invocable_r_v<size_t, decltype(&Sealer::seal), Sealer&, const uint8_t*, size_t, uint8_t*>);
static_assert(!std::is_copy_constructible_v<Sealer> && !std::is_copy_assignable_v<Sealer>);
void retransmissionTakesNewSequence() {
  Pair p; auto sender = provenSender(); Plain plain; uint64_t now = 1000;
  sender.video(video(1, 3000, true), true);
  std::vector<Bytes> sent; std::vector<uint64_t> sequences;
  const auto pump = [&] { while (sender.next(now, plain)) { uint8_t out[maxDatagram]; const size_t n = p.hostSeal.seal(plain.bytes.data(), plain.size, out); require(n != 0, "Seal failed"); sent.emplace_back(plain.bytes.data(), plain.bytes.data() + plain.size); sequences.push_back(get64(out + 9)); } };
  for (; sent.size() < 3; now += 1000) pump();
  const auto n = nackFor(1, 1, 1); sender.control(n.data(), n.size(), now); now += 1000; pump();
  require(sent.size() >= 4 && sent[3] == sent[1], "A retransmission is not the same plaintext");
  for (size_t i = 1; i < sequences.size(); ++i) require(sequences[i] > sequences[i - 1], "A sequence was reused");
  ++cases;
}

void fragments() {
  std::mt19937_64 random(11);
  for (size_t size : {size_t(13), size_t(100), fragmentPayload - 1, fragmentPayload, fragmentPayload + 1, 2 * fragmentPayload, 2 * fragmentPayload + 1, size_t(300000), maxMessageBytes - 1, maxMessageBytes}) {
    const Bytes message = video(1, size - 12, true);
    require(fragmentCount(size) == (size + fragmentPayload - 1) / fragmentPayload, "Wrong fragment count");
    auto parts = split(classVideo, 1, message);
    for (const auto& part : parts) require(part.size() <= maxPlaintext, "A fragment is too large");
    for (int order = 0; order < 3; ++order) {
      if (order == 1) std::reverse(parts.begin(), parts.end());
      if (order == 2) std::shuffle(parts.begin(), parts.end(), random);
      Client c; c.feed(parts); c.feed(parts);  // every fragment twice
      require(c.out.messages.size() == 1 && c.out.messages[0].bytes == message, "Reassembled bytes differ from the input");
      require(!c.receiver.heldBytes() && c.receiver.gaps() <= parts.size(), "Reassembly held memory");
    }
    for (size_t audioSize : {size_t(13), size_t(1932), size_t(4096)}) {
      const Bytes sound = pcm(1, audioSize - 12); Client c; auto bits = split(classAudio, 1, sound); std::reverse(bits.begin(), bits.end()); c.feed(bits);
      require(c.out.messages.size() == 1 && c.out.messages[0].bytes == sound && !c.out.messages[0].video, "Audio bytes differ from the input");
    }
  }
  require(!fragmentCount(0) && !fragmentCount(maxMessageBytes + 1), "A message outside the limits fragmented");
  require(maxPlaintext == 1167 && fragmentPayload == 1157 && maxMessageBytes == 2369536, "The contract's limits moved");
  auto sender = provenSender(); Bytes oversize(maxMessageBytes + 1, 1);
  require(sender.video(oversize, true).tooLarge && sender.video(oversize, false).requestKey, "An oversize message was not left to WSS");
  require(sender.audio(Bytes(maxAudioFragments * fragmentPayload + 1, 2)).tooLarge, "Oversize audio was accepted");
  ++cases;
}

void inOrderDelivery() {
  Client c; const Bytes one = video(1), two = video(2), three = video(3);
  c.feed(split(classVideo, 1, one)); require(c.out.messages.size() == 1, "A whole next message was not delivered");
  c.feed(split(classVideo, 3, three)); require(c.out.messages.size() == 1, "A later message overtook a gap");
  c.feed(split(classVideo, 2, two));
  require(c.out.messages.size() == 3 && c.out.messages[1].bytes == two && c.out.messages[2].bytes == three, "Video was not delivered in order");
  c.clear(); c.feed(split(classVideo, 2, two)); require(c.out.messages.empty(), "An old message was delivered again");
  ++cases;
}

void nackTiming() {
  {  // A long RTT: RTT/4, then 1.25 RTT.
    Client c(120000); const auto parts = split(classVideo, 1, video(1, 5000));
    c.feed(parts[0]); c.feed(parts[1]); c.feed(parts[3]); const uint64_t gap = c.now;
    c.at(gap + 29999); require(!c.controls(kindNack), "A NACK came before RTT/4");
    c.at(gap + 30000); require(c.controls(kindNack) == 1, "No NACK at RTT/4");
    const auto n = c.taken(kindNack)[0];
    require(n.size() == 10 && n[1] == 1 && get32(&n[2]) == 1 && get16(&n[6]) == 2 && get16(&n[8]) == 1, "Wrong NACK entry");
    c.at(gap + 179999); require(c.controls(kindNack) == 1, "The repeat came before 1.25 RTT");
    c.at(gap + 180000); require(c.controls(kindNack) == 2, "No repeat at 1.25 RTT");
  }
  {  // A short RTT: the floors, 10 ms and then 20 ms.
    Client c(4000); const auto parts = split(classVideo, 1, video(1, 5000));
    c.feed(parts[0]); c.feed(parts[2]); const uint64_t gap = c.now;
    c.at(gap + 9999); require(!c.controls(kindNack), "The 10 ms floor was not kept");
    c.at(gap + 10000); require(c.controls(kindNack) == 1, "No NACK at 10 ms");
    c.at(gap + 29999); require(c.controls(kindNack) == 1, "The 20 ms floor was not kept");
    c.at(gap + 30000); require(c.controls(kindNack) == 2, "No repeat at 20 ms");
  }
  {  // Three repeats and no more, while the fragment stays missing.
    Client c(20000); const auto parts = split(classVideo, 1, video(1, 5000));
    c.feed(parts[0]); c.feed(parts[2]); const uint64_t gap = c.now;
    for (const uint64_t at : {10000, 35000, 60000, 85000}) {
      const size_t before = c.controls(kindNack);
      c.at(gap + at - 1); require(c.controls(kindNack) == before, "A NACK came early");
      c.at(gap + at); require(c.controls(kindNack) == before + 1, "A NACK did not come on time");
    }
    c.at(gap + 89999); require(c.controls(kindNack) == 4 && !c.receiver.awaitingKey(), "A fragment was NACKed a fifth time");
  }
  {  // The limit holds even if the RTT rises in mid-repair and leaves time for a fifth.
    Client c(4000); const auto parts = split(classVideo, 1, video(1, 5000));
    c.feed(parts[0]); c.feed(parts[2]); const uint64_t gap = c.now;
    for (const uint64_t at : {10000, 30000, 50000}) c.at(gap + at);
    c.feed(tailFor(0, 1, testChallenge, 80000));
    c.at(gap + 70000); require(c.controls(kindNack) == 4, "The fourth NACK did not come");
    for (uint64_t at = 75000; at < 300000; at += 5000) c.at(gap + at);
    require(c.controls(kindNack) == 4, "A fragment was NACKed a fifth time");
  }
  {  // A fragment that arrived is not asked for again, even behind an older gap still open.
    Client c(20000); const auto parts = split(classVideo, 1, video(1, 5000));
    c.feed(parts[0]); c.feed(parts[2]); c.feed(parts[4]); const uint64_t gap = c.now; c.at(gap + 10000); c.feed(parts[3]); c.clear(); c.at(gap + 35000);
    const auto n = c.taken(kindNack);
    require(n.size() == 1 && n[0][1] == 1 && get16(&n[0][6]) == 1 && get16(&n[0][8]) == 1, "A repaired fragment was NACKed again");
  }
  {  // A wholly lost message repeats too, until any of it arrives.
    Client c(20000); const auto parts = split(classVideo, 1, video(1, 5000)); c.feed(split(classVideo, 2, video(2))); const uint64_t gap = c.now;
    c.at(gap + 10000); require(c.controls(kindNack) == 1, "No NACK for a whole message"); c.feed(parts[1]); c.clear();
    c.at(gap + 35000); const auto n = c.taken(kindNack); require(!n.empty() && get16(&n[0][8]) != maxFragments, "A message of known size was asked for whole");
  }
  {  // A repair in time cancels the NACK; a reorder inside the delay costs nothing.
    Client c(20000); const auto parts = split(classVideo, 1, video(1, 5000));
    c.feed(parts[0]); c.feed(parts[2]); c.at(c.now + 5000); c.feed(parts[1]);
    c.at(c.now + 100000); require(!c.controls(kindNack), "A reordered fragment was NACKed");
  }
  {  // Runs and a wholly lost message.
    Client c(4000); const auto parts = split(classVideo, 1, video(1, 8000)); const auto later = split(classVideo, 3, video(3));
    c.feed(parts[0]); c.feed(parts[4]); c.feed(later);
    c.at(c.now + 10000); const auto n = c.taken(kindNack); require(n.size() == 1 && n[0][1] == 3, "Wrong NACK entries");
    require(get32(&n[0][2]) == 1 && get16(&n[0][6]) == 1 && get16(&n[0][8]) == 3, "A run was not joined");
    require(get32(&n[0][10]) == 1 && get16(&n[0][14]) == 5 && get16(&n[0][16]) == parts.size() - 5, "The tail of a closed message was not NACKed");
    require(get32(&n[0][18]) == 2 && get16(&n[0][22]) == 0 && get16(&n[0][24]) == maxFragments, "A wholly lost message was not NACKed");
  }
  {  // More gaps than one NACK holds: every NACK stays within 64 entries.
    Client c(4000); const auto parts = split(classVideo, 1, video(1, 2000 * fragmentPayload));
    for (size_t i = 0; i < parts.size(); i += 2) c.feed(parts[i]);
    c.at(c.now + 10000); const auto all = c.taken(kindNack);
    require(all.size() == maxNacksPerTick, "NACKs per tick are not bounded");
    for (const auto& n : all) require(n[1] == maxNackEntries && n.size() == 2 + maxNackEntries * nackEntryBytes, "A NACK is not 64 entries");
  }
  ++cases;
}

void rttFromTail() {
  {  // The client's RTT is the Host's: 50 ms until one arrives, and 0 means unknown.
    Client c; require(c.receiver.rttUs() == 50000, "The client does not assume 50 ms");
    c.feed(tailFor(0, 1)); require(c.receiver.rttUs() == 50000, "An unknown RTT replaced the assumption");
    c.feed(tailFor(0, 1, testChallenge, 33000)); require(c.receiver.rttUs() == 33000, "TAIL's RTT was not taken");
    c.feed(tailFor(0, 1)); require(c.receiver.rttUs() == 33000, "An unknown RTT replaced a known one");
    // The assumed 50 ms really drives the timers.
    Client d; const auto parts = split(classVideo, 1, video(1, 5000)); d.feed(parts[0]); d.feed(parts[2]); const uint64_t gap = d.now;
    d.at(gap + 12499); require(!d.controls(kindNack), "The assumed RTT was not used"); d.at(gap + 12500); require(d.controls(kindNack) == 1, "The assumed RTT was not used");
    d.at(gap + 194999); require(!d.receiver.awaitingKey(), "The assumed RTT was not used"); d.at(gap + 195000); require(d.receiver.awaitingKey(), "The assumed RTT was not used");
  }
  {  // The Host: unknown until a sample, the first sample as it is, then smoothed by 1/8.
    auto sender = provenSender(); Plain p; uint64_t now = 1000;
    const auto tailRtt = [&] { now += proofTailUs; while (sender.next(now, p)) if (p.bytes[0] == kindTail) return get32(&p.bytes[22]); throw std::runtime_error("No TAIL"); };
    require(!sender.rttUs() && !tailRtt(), "An RTT was invented");
    const auto first = feedbackFor(0, 0, 0, now - 40000, 0); sender.control(first.data(), first.size(), now); require(sender.rttUs() == 40000 && tailRtt() == 40000, "The first sample was not taken as it is");
    const auto second = feedbackFor(0, 0, 0, now - 90000, 10000); sender.control(second.data(), second.size(), now); require(sender.rttUs() == 45000 && tailRtt() == 45000, "RTT is not smoothed by 1/8, or hold was not taken off");
    for (int n = 0; n < 200; ++n) { const auto f = feedbackFor(0, 0, 0, now - 20000, 0); sender.control(f.data(), f.size(), now); }
    require(sender.rttUs() >= 20000 && sender.rttUs() < 21000, "RTT does not follow its samples");
    // A FEEDBACK that does not count gives no sample.
    const uint64_t rtt = sender.rttUs(); const auto wrong = feedbackFor(0, 0, 0, now - 90000, 0, testChallenge + 1); sender.control(wrong.data(), wrong.size(), now); require(sender.rttUs() == rtt, "An uncounted FEEDBACK moved the RTT");
  }
  {  // Round trip through the real pieces: Host time out in TAIL, back in FEEDBACK, RTT out in the next TAIL.
    auto sender = provenSender(); Client c; Plain p; uint64_t host = 200000; const uint64_t oneWay = 15000;
    for (int round = 0; round < 8; ++round) {
      while (sender.next(host, p)) { c.now = 1000000 + host + oneWay; c.feed(Bytes(p.bytes.data(), p.bytes.data() + p.size)); }
      host += 100000; c.at(1000000 + host);
      for (const auto& control : c.out.control) sender.control(control.data(), control.size(), host + oneWay);
      c.clear(); host += 100000;
    }
    require(sender.rttUs() == 30000 && c.receiver.rttUs() == 30000, "The RTT did not reach the client through TAIL");
  }
  ++cases;
}

void giveUpDeadline() {
  struct Case { uint64_t rtt, deadline; };
  for (const auto& v : {Case{5000, 60000}, Case{11000, 60000}, Case{20000, 90000}, Case{33000, 135500}, Case{60000, 230000}, Case{80000, 300000}, Case{150000, 545000}, Case{166000, 600000}, Case{300000, 600000}}) {
    Client c(v.rtt); const auto parts = split(classVideo, 1, video(1, 5000, true));
    c.feed(split(classVideo, 1, video(1, 10, true)));  // nothing yet at stake
    c.clear();
    const auto second = split(classVideo, 2, video(2, 5000));
    c.feed(second[0]); c.feed(second[2]);
    const uint64_t gap = c.now;
    c.at(gap + v.deadline - 1); require(!c.receiver.awaitingKey(), "Given up before the deadline");
    c.at(gap + v.deadline); require(c.receiver.awaitingKey(), "Not given up at the deadline");
    require(c.controls(kindKeyframe) == 1, "No KEYFRAME at the give-up");
  }
  {  // A paced IDR longer than the deadline is not lost: only a gap starts the clock.
    Client c(5000); const auto parts = split(classVideo, 1, video(1, 300000, true));
    for (const auto& part : parts) { c.now += 370; c.feed(part); c.tick(); }
    require(c.videos() == 1 && !c.receiver.awaitingKey() && c.out.control.size() < 5, "A paced IDR was given up");
  }
  {  // A gap repaired in time resets nothing later: the message delivers.
    Client c(20000); const auto parts = split(classVideo, 1, video(1, 5000, true));
    c.feed(parts[0]); c.feed(parts[2]); c.feed(parts[3]); c.feed(parts[4]); c.at(c.now + 59000); c.feed(parts[1]);
    c.at(c.now + 200000); require(c.videos() == 1 && !c.receiver.awaitingKey(), "A repaired message was given up");
  }
  {  // A whole P-frame waiting behind the lost one dies with it: its reference is gone.
    Client c(20000); c.feed(split(classVideo, 1, video(1, 100, true))); c.clear();
    const auto lost = split(classVideo, 2, video(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.feed(split(classVideo, 3, video(3))); c.feed(split(classVideo, 4, video(4)));
    require(c.out.messages.empty(), "A P-frame overtook a gap");
    c.at(c.now + 90000); require(c.out.messages.empty() && c.receiver.awaitingKey() && !c.receiver.heldBytes(), "A P-frame waiting behind a given-up frame was delivered or kept");
  }
  {  // While waiting, P-frames behind a key frame still under repair are kept: they follow it.
    Client c(20000); c.feed(split(classVideo, 1, video(1, 100, true))); c.clear();
    const auto lost = split(classVideo, 2, video(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); require(c.receiver.awaitingKey(), "Not given up"); c.clear();
    const auto key = split(classVideo, 3, video(3, 5000, true)); c.feed(key[0]); c.feed(key[2]); c.feed(key[3]); c.feed(key[4]);
    c.feed(split(classVideo, 4, video(4))); c.feed(split(classVideo, 5, video(5)));
    c.feed(key[1]); require(c.videos() == 3 && c.out.messages[0].key && !c.receiver.awaitingKey(), "P-frames behind a key frame under repair were dropped");
  }
  {  // The same when the key frame has not named itself yet: fragment 0 was the one lost.
    Client c(20000); c.feed(split(classVideo, 1, video(1, 100, true))); c.clear();
    const auto lost = split(classVideo, 2, video(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); c.clear();
    const auto key = split(classVideo, 3, video(3, 5000, true)); c.feed(key[1]); c.feed(key[2]); c.feed(key[3]); c.feed(key[4]);
    c.feed(split(classVideo, 4, video(4))); c.feed(key[0]);
    require(c.videos() == 2 && c.out.messages[0].key && c.out.messages[1].pts == 4, "A P-frame behind an unnamed key frame was dropped");
  }
  {  // A repair after the give-up is dropped and delivers nothing.
    Client c(20000); const auto first = split(classVideo, 1, video(1, 5000)); const auto next = split(classVideo, 2, video(2));
    c.feed(first[0]); c.feed(first[2]); c.at(c.now + 90000); c.feed(first[1]); c.feed(next); c.tick();
    require(!c.videos() && c.receiver.awaitingKey(), "A given-up message or a later P-frame was delivered");
  }
  ++cases;
}

void dropUntilKey() {
  Client c(20000); const uint16_t epoch = 1; const auto first = split(classVideo, 1, video(1, 5000, true));
  c.feed(first); c.clear();
  const auto lost = split(classVideo, 2, video(2, 5000)); c.feed(lost[0]); c.feed(lost[2]);
  c.at(c.now + 90000); require(c.receiver.awaitingKey(), "Not given up");
  auto k = c.taken(kindKeyframe); require(k.size() == 1 && k[0].size() == keyframeBytes && !k[0][1] && get16(&k[0][2]) == epoch, "Wrong KEYFRAME");
  c.clear();
  c.feed(split(classVideo, 3, video(3))); c.feed(split(classVideo, 4, video(4)));
  require(c.out.messages.empty() && !c.receiver.heldBytes(), "A P-frame passed the key wait, or still holds memory");
  c.at(c.now + 499999); require(!c.controls(kindKeyframe), "KEYFRAME repeated early");
  c.at(c.now + 1); require(c.controls(kindKeyframe) == 1, "KEYFRAME did not repeat");
  c.clear();
  // No gap, no NACK for frames it will drop.
  c.at(c.now + 100000); require(!c.controls(kindNack), "A dropped P-frame was NACKed");
  // A key frame lost at the start of its message: it still counts when it is whole.
  const Bytes key = video(9, 5000, true); auto parts = split(classVideo, 5, key);
  c.feed(parts[1]); c.feed(parts[2]); require(c.receiver.awaitingKey(), "A part of a key frame ended the wait");
  c.feed(parts[0]); c.feed(parts[3]); c.feed(parts[4]);
  require(c.videos() == 1 && c.out.messages[0].bytes == key && c.out.messages[0].key && !c.receiver.awaitingKey(), "A key frame did not end the wait");
  c.clear(); c.feed(split(classVideo, 6, video(10)));
  require(c.videos() == 1, "P-frames after the key frame were dropped");
  c.at(c.now + 2000000); require(!c.controls(kindKeyframe), "KEYFRAME went on after a key frame");
  ++cases;
}

void keyFrameRules() {
  {  // The repeat waits while a key frame is visibly arriving, however long that takes.
    Client c(20000); c.feed(split(classVideo, 1, video(1, 100, true))); c.clear();
    const auto lost = split(classVideo, 2, video(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); require(c.controls(kindKeyframe) == 1, "No KEYFRAME at the give-up"); c.clear();
    const auto key = split(classVideo, 3, video(3, 5000, true)); c.feed(key[0]); c.feed(key[1]);
    c.at(c.now + 500000); c.at(c.now + 1000000); require(!c.controls(kindKeyframe), "KEYFRAME repeated while a key frame was arriving");
    // It stalls: TAIL closes it, and the held repeat goes at the give-up, not 500 ms later.
    c.feed(tailFor(3, 1)); const uint64_t closed = c.now;
    c.at(closed + 89999); require(!c.controls(kindKeyframe), "The held repeat went before the give-up");
    c.at(closed + 90000); require(c.controls(kindKeyframe) == 1 && c.receiver.awaitingKey(), "The held repeat did not go at the give-up");
    c.clear(); c.at(closed + 90000 + 499999); require(!c.controls(kindKeyframe), "The repeat lost its 500 ms after the hold"); c.at(closed + 90000 + 500000); require(c.controls(kindKeyframe) == 1, "The repeat stopped after the hold");
  }
  {  // It finishes: no repeat at all.
    Client c(20000); c.feed(split(classVideo, 1, video(1, 100, true))); c.clear();
    const auto lost = split(classVideo, 2, video(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); c.clear();
    const auto key = split(classVideo, 3, video(3, 5000, true)); c.feed(key[0]); c.at(c.now + 700000); c.feed(key[1]); c.feed(key[2]); c.feed(key[3]); c.feed(key[4]); c.at(c.now + 2000000);
    require(c.videos() == 1 && !c.controls(kindKeyframe), "A key frame that arrived was asked for again");
  }
  {  // Only a usable key frame holds it, not a P-frame or an old epoch's key frame, even behind a gap.
    for (const Bytes& waiting : {video(4), video(4, 100, true, 0)}) {
      Client c(20000); c.feed(split(classVideo, 1, video(1, 100, true))); c.clear();
      const auto lost = split(classVideo, 2, video(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); const uint64_t asked = c.now; c.clear();
      c.now = asked + 495000; c.feed(split(classVideo, 4, waiting)); require(c.receiver.windowMessages() == 2, "Fixture");
      c.at(asked + 500000); require(c.controls(kindKeyframe) == 1, "Something other than a usable key frame held the repeat");
    }
  }
  {  // A newer key frame leaves an older one under repair alone: killing it is how IDRs cascade.
    Client c(20000); const auto first = split(classVideo, 1, video(1, 5000, true)), second = split(classVideo, 3, video(3, 5000, true));
    c.feed(first[0]); c.feed(first[2]); c.feed(first[3]); c.feed(first[4]); c.feed(split(classVideo, 2, video(2))); c.feed(second[0]); c.feed(second[1]);
    c.feed(first[1]); require(c.videos() == 2 && c.out.messages[0].key && c.out.messages[0].pts == 1 && c.out.messages[1].pts == 2, "A newer key frame killed one under repair");
    c.feed(second[2]); c.feed(second[3]); c.feed(second[4]); require(c.videos() == 3 && c.out.messages[2].key, "The newer key frame was lost");
  }
  {  // If the older one cannot be repaired, the newer one still ends the wait.
    Client c(20000); const auto first = split(classVideo, 1, video(1, 5000, true)), second = split(classVideo, 2, video(2, 5000, true));
    c.feed(first[0]); c.feed(first[2]); c.feed(second[0]); c.feed(second[1]); c.at(c.now + 90000); require(c.receiver.awaitingKey() && !c.videos(), "The older key frame was not given up");
    c.feed(second[2]); c.feed(second[3]); c.feed(second[4]); require(c.videos() == 1 && c.out.messages[0].pts == 2 && !c.receiver.awaitingKey(), "The newer key frame did not end the wait");
  }
  {  // Only the nearest older key frame stays, so a run of them cannot pile up.
    Client c(20000); const auto a = split(classVideo, 1, video(1, 5000, true)), b = split(classVideo, 2, video(2, 5000, true)), d = split(classVideo, 3, video(3, 5000, true));
    c.feed(a[0]); c.feed(b[0]); c.feed(d[0]); require(c.receiver.windowMessages() == 2, "Key frames piled up"); c.feed(a[1]); require(c.receiver.windowMessages() == 2, "A key frame came back");
  }
  {  // A key frame in progress makes older unfinished frames pointless, without any IDR request.
    Client c(20000); const auto old = split(classVideo, 1, video(1, 5000)); const auto key = split(classVideo, 2, video(2, 5000, true));
    c.feed(old[0]); c.feed(old[2]); c.feed(key[0]); c.feed(key[1]);
    c.at(c.now + 200000);
    require(!c.receiver.awaitingKey() && !c.controls(kindKeyframe), "An older gap asked for an IDR beside a key frame");
    c.feed(key[2]); c.feed(key[3]); c.feed(key[4]);
    require(c.videos() == 1 && c.out.messages[0].key, "The key frame was not delivered past older data");
  }
  {  // A whole key frame on the other path stops the repeat and clears the gaps.
    Client c(20000); const auto lost = split(classVideo, 1, video(1, 5000)); c.feed(lost[0]); c.feed(lost[2]);
    c.at(c.now + 90000); require(c.receiver.awaitingKey(), "Not given up"); c.clear();
    c.receiver.keyFrameOtherPath(); c.at(c.now + 2000000);
    require(!c.receiver.awaitingKey() && !c.controls(kindKeyframe) && !c.controls(kindNack), "A key frame on WSS did not end the wait");
  }
  {  // WSS ended it, and a UDP key frame already under way still finishes.
    Client c(20000); const auto key = split(classVideo, 1, video(1, 5000, true));
    c.feed(key[0]); c.feed(key[1]); c.receiver.keyFrameOtherPath(); c.feed(key[2]); c.feed(key[3]); c.feed(key[4]);
    c.at(c.now + 50000); require(!c.videos() && c.controls(kindNack) == 1, "The earlier fragments were not asked for again");
    c.feed(key[0]); c.feed(key[1]); require(c.videos() == 1, "A key frame under way was lost");
  }
  {  // A full decoder queue skips to the next key frame.
    Client c(20000); c.feed(split(classVideo, 1, video(1, 100, true))); c.clear();
    c.receiver.queueFull(c.now); c.tick(); require(c.controls(kindKeyframe) == 1, "A full queue did not request an IDR");
    c.clear(); c.feed(split(classVideo, 2, video(2))); require(c.out.messages.empty(), "A P-frame passed a full queue");
    c.feed(split(classVideo, 3, video(3, 100, true))); require(c.videos() == 1, "The key frame did not restart delivery");
  }
  {  // A decoder that keeps refusing key frames gets one request per key interval, not one per arrival.
    Client c(20000); c.feed(split(classVideo, 1, video(1, 100, true))); c.clear();
    c.receiver.queueFull(c.now); c.tick(); require(c.controls(kindKeyframe) == 1, "A full queue did not request an IDR");
    c.clear(); c.at(c.now + 30000); c.clear(); c.feed(split(classVideo, 2, video(2, 100, true))); require(c.videos() == 1, "The requested key frame was not delivered");
    c.clear(); c.receiver.queueFull(c.now); c.tick(); require(!c.controls(kindKeyframe), "A refused key frame asked for another at once");
    c.at(c.now + keyframeIntervalUs); require(c.controls(kindKeyframe) == 1, "A refused key frame was never asked for again");
  }
  ++cases;
}

void epochs() {
  Client c(20000, 5);
  c.feed(split(classVideo, 1, video(1, 100, true, 5))); require(c.videos() == 1, "The current epoch failed"); c.clear();
  c.feed(split(classVideo, 2, video(2, 100, false, 4))); c.feed(split(classAudio, 1, pcm(2, 1920, 4)));
  require(c.out.messages.empty() && !c.receiver.awaitingKey(), "A stale epoch was delivered or broke the chain");
  c.feed(split(classVideo, 3, video(3, 100, false, 6))); require(c.out.messages.empty() && c.receiver.awaitingKey(), "A future epoch was delivered");
  c.tick(); require(get16(&c.taken(kindKeyframe)[0][2]) == 5, "KEYFRAME does not carry the client's epoch");
  c.clear(); c.receiver.setEpoch(6);
  c.feed(split(classVideo, 4, video(4, 100, true, 6))); require(c.videos() == 1 && !c.receiver.awaitingKey(), "The announced epoch failed");
  // A message whole and waiting when the epoch moves on is not delivered.
  c.clear(); const auto a = split(classVideo, 5, video(5, 5000, false, 6)); const auto b = split(classVideo, 6, video(6, 100, false, 6));
  c.feed(a[0]); c.feed(a[2]); c.feed(b); c.receiver.setEpoch(7); c.feed(a[1]); c.feed(a[3]); c.feed(a[4]);
  require(c.out.messages.empty(), "A message of an old epoch was delivered");
  ++cases;
}

void tailHandling() {
  {  // The last fragments of an idle screen's frame.
    Client c(4000); const auto parts = split(classVideo, 1, video(1, 5000)); c.feed(parts[0]); c.feed(parts[1]);
    c.at(c.now + 100000); require(!c.controls(kindNack), "An unfinished message NACKed without a gap");
    c.clear(); c.feed(tailFor(1, 123456)); c.at(c.now + 10000);
    auto n = c.taken(kindNack); require(n.size() == 1 && get16(&n[0][6]) == 2 && get16(&n[0][8]) == parts.size() - 2, "TAIL did not reveal the lost end");
  }
  {  // A whole message lost on an idle screen.
    Client c(4000); c.feed(split(classVideo, 1, video(1))); c.clear(); c.feed(tailFor(2, 5)); c.at(c.now + 10000);
    auto n = c.taken(kindNack); require(n.size() == 1 && get32(&n[0][2]) == 2 && get16(&n[0][8]) == maxFragments, "TAIL did not reveal a lost message");
  }
  {  // Nothing lost: TAIL changes nothing, repeats change nothing.
    Client c(4000); c.feed(split(classVideo, 1, video(1))); c.feed(tailFor(1, 5)); c.feed(tailFor(1, 6)); c.at(c.now + 200000);
    require(!c.controls(kindNack) && !c.receiver.awaitingKey(), "A clean TAIL raised a gap");
  }
  {  // Echo and hold, once.
    Client c(4000); const uint64_t start = c.now; c.feed(tailFor(0, 777777, 0xabcdef)); c.at(start + 37000); require(c.controls(kindFeedback) == 0, "FEEDBACK came early");
    c.at(start + 100000); auto f = c.taken(kindFeedback); require(f.size() == 1 && get64(&f[0][14]) == 777777 && get32(&f[0][22]) == 100000, "Wrong echo or hold");
    require(get64(&f[0][26]) == 0xabcdef, "The challenge was not echoed");
    c.clear(); c.at(start + 200000); f = c.taken(kindFeedback); require(f.size() == 1 && !get64(&f[0][14]) && !get32(&f[0][22]), "An echo was repeated");
    require(get64(&f[0][26]) == 0xabcdef, "A later FEEDBACK lost the challenge");
    c.feed(tailFor(0, 5, 0x1234)); c.clear(); c.at(start + 300000); require(get64(&c.taken(kindFeedback)[0][26]) == 0x1234, "FEEDBACK did not echo the latest challenge");
  }
  {  // Nothing reached it: FEEDBACK echoes nothing.
    Client c; c.at(c.now + 100000); require(!get64(&c.taken(kindFeedback)[0][26]), "A challenge was invented");
  }
  ++cases;
}

void feedbackTiming() {
  Client c(20000); const uint64_t start = c.now; size_t sent = 0;
  for (uint64_t t = start; t < start + 1000000; t += 1000) c.at(t);
  sent = c.controls(kindFeedback); require(sent == 9, "FEEDBACK does not run every 100 ms on an idle stream");
  c.clear(); c.at(start + 1100000); auto f = c.taken(kindFeedback);
  require(f.size() == 1 && f[0].size() == feedbackBytes && !f[0][1], "Wrong FEEDBACK size");
  const auto first = split(classVideo, 1, video(1, 100, true)), second = split(classVideo, 2, video(2, 5000)), third = split(classVideo, 3, video(3, 100));
  c.at(c.now + 50000);
  c.feed(first); c.feed(second[0]); c.feed(second[2]); c.feed(third); c.feed(split(classAudio, 1, pcm(1))); c.feed(tailFor(3, 9));
  c.clear(); c.at(c.now + 50000); f = c.taken(kindFeedback);
  // Message 3 is whole but waits behind message 2: only what is handed on is progress.
  require(f.size() == 1 && get32(&f[0][2]) == 1 && get32(&f[0][6]) == 1 + first.size() + 2 + third.size() + 2 + 1, "Wrong highest whole or datagrams received");  // 1: the TAIL that set the RTT
  require(!get32(&f[0][10]), "Datagrams counted lost before the give-up");
  c.clear(); c.at(c.now + 300000); f = c.taken(kindFeedback); require(!f.empty() && get32(&f.back()[10]) == second.size() - 2, "Given-up fragments were not reported");
  ++cases;
}

void audioRules() {
  {  // Never waits for video, and arrives out of order as it comes.
    Client c(20000); const auto lost = split(classVideo, 1, video(1, 5000)); c.feed(lost[0]); c.feed(lost[2]);
    const Bytes one = pcm(1), two = pcm(2); const auto a = split(classAudio, 1, one), b = split(classAudio, 2, two);
    require(a.size() == 2, "10 ms of PCM is not two fragments");
    c.feed(b); require(c.out.messages.size() == 1 && c.out.messages[0].bytes == two, "Audio waited");
    c.feed(a); require(c.out.messages.size() == 2 && c.out.messages[1].bytes == one, "Audio waited for its order");
  }
  {  // A lost audio fragment causes nothing at all: no NACK, no give-up.
    Client c(4000); c.feed(split(classAudio, 1, pcm(1))[0]); c.feed(split(classAudio, 3, pcm(3)));
    c.feed(split(classAudio, 4, pcm(4))[1]); c.feed(tailFor(0, 1));
    for (int n = 0; n < 300; ++n) c.at(c.now + 1000);
    require(!c.controls(kindNack) && !c.controls(kindKeyframe) && !c.receiver.awaitingKey(), "Audio loss was NACKed");
  }
  {  // Unfinished audio is bounded and counts as lost when it goes.
    Client c(20000);
    for (uint32_t id = 1; id <= 100; ++id) { c.feed(split(classAudio, id, pcm(id))[0]); require(c.receiver.heldBytes() <= audioPartials * 2 * fragmentPayload, "Unfinished audio grew"); }
    const auto parts = split(classAudio, 200, pcm(200, 5000)); require(parts.size() > 2, "Fixture"); c.feed(parts); require(c.out.messages.size() == 1, "Newest audio did not get a slot");
  }
  {  // The Sender keeps no audio: a NACK of the same number finds nothing.
    auto sender = provenSender(); Plain p; uint64_t now = 1000; sender.audio(pcm(1));
    while (sender.next(now, p)) {}
    const auto n = nackFor(1, 0, 1); sender.control(n.data(), n.size(), now);
    require(!sender.next(now + 1000, p) && !sender.stats().repairs, "Audio was retransmitted");
    // The Sender's audio does not wait behind paced video.
    sender.video(video(1, 400000), false); for (int i = 0; i < 40; ++i) sender.next(now, p);
    sender.audio(pcm(2)); require(sender.next(now, p) && p.bytes[1] == classAudio, "Audio waited behind the pacer");
  }
  ++cases;
}

void pacer() {
  constexpr int target = 20000000; auto sender = provenSender(target); Plain p; uint64_t now = 0, wire = 0, peak = 0, window = 0, windowStart = 0;
  sender.video(video(1, maxMessageBytes - 12), true);
  const uint64_t duration = 250000;
  for (; now < duration; now += 100) {
    if (now - windowStart >= 10000) { peak = std::max(peak, window); window = 0; windowStart = now; }
    while (sender.next(now, p)) { wire += p.size + headerBytes + tagBytes; window += p.size + headerBytes + tagBytes; }
  }
  const double rate = double(wire) * 8 / (double(duration) / 1e6), expected = target * 3.0;  // the contract's figure, not the constant
  require(rate > expected * 0.95 && rate < expected * 1.05, "The pacer is not 3 times the target");
  // A 10 ms window at 3 times the target, plus the burst of 6 ms at that rate (about one
  // average frame at 60 fps, so a P-frame leaves at once as on WSS).
  require(double(peak) < expected / 8 * 0.016 + 3000, "The pacer burst");
  {  // An average 20 Mbps frame at 60 fps (41.7 KB) leaves in one pass; a key frame does not.
    auto quick = provenSender(target); Plain q; size_t sent = 0;
    quick.video(video(1, 41000, true), true); while (quick.next(1, q)) sent += q.size;
    require(sent >= 41000, "An average frame waited for the pacer");
    auto paced = provenSender(target); size_t first = 0;
    paced.video(video(1, 200000, true), true); while (paced.next(1, q)) first += q.size;
    require(first < 60000, "A key frame was not paced");
  }
  ++cases;
}

void resendBuffer() {
  auto sender = provenSender(); Plain p; uint64_t now = 1000;
  const Bytes message = video(1, 5000, true);
  sender.video(message, true); while (sender.next(now, p)) {}
  const auto parts = split(classVideo, 1, message); std::vector<Bytes> again;
  const auto ask = [&](uint16_t first, uint16_t run) { const auto n = nackFor(1, first, run); sender.control(n.data(), n.size(), now); now += 3000; while (sender.next(now, p)) if (p.bytes[0] == kindFragment) again.emplace_back(p.bytes.data(), p.bytes.data() + p.size); };
  ask(1, 2); require(again.size() == 2 && again[0] == parts[1] && again[1] == parts[2], "A repair is not the original fragment");
  again.clear(); ask(4, maxFragments); require(again.size() == parts.size() - 4 && again.back() == parts.back(), "A run was not clamped to the message");
  again.clear(); ask(uint16_t(parts.size()), 3); require(again.empty(), "A fragment outside the message was repaired");
  // Exactly 600 ms after it went out: the contract's figure, not the constant.
  auto second = provenSender(); uint64_t t = 1000; second.video(message, true); while (second.next(t, p)) {}
  const uint64_t done = t;
  const auto repairAt = [&](uint64_t at) { const auto n = nackFor(1, 0, 1); const auto before = second.stats().repairs; second.control(n.data(), n.size(), at); while (second.next(at, p)) {} return second.stats().repairs > before; };
  require(repairAt(done + 600000), "A fragment was gone before 600 ms");
  require(!repairAt(done + 600000 + 1), "A fragment was kept past 600 ms");
  require(!second.heldBytes(), "The resend buffer held memory");
  // Unknown IDs and malformed NACKs repair nothing and do not crash.
  auto third = provenSender(); third.video(message, true); while (third.next(1000, p)) {}
  for (Bytes bad : {nackFor(2, 0, 1), nackFor(0, 0, 1), nackFor(1, 0, 0), Bytes{kindNack}, Bytes{kindNack, 0}, Bytes{kindNack, 65}, Bytes{kindNack, 2, 0, 0, 0, 1, 0, 0, 0, 1}}) {
    third.control(bad.data(), bad.size(), 2000); require(!third.next(3000, p), "A malformed NACK repaired something");
  }
  require(!third.stats().repairs, "A malformed NACK counted");
  ++cases;
}

void repairBudget() {
  constexpr int target = 20000000; auto sender = provenSender(target); Plain p; uint64_t now = 1000;
  // Paced P-frames while the client NACKs everything it can.
  uint64_t repairBytes = 0, peakPerWindow = 0, inWindow = 0, windowStart = now; uint32_t id = 0; bool key = true;
  for (; now < 10000000; now += 500) {
    // Starved of pacer room, the queue overflows: the key frame keeps the stream, and so the NACKs, going.
    if (now % 16500 == 1000 % 16500) key = sender.video(video(now, 30000, key), key).requestKey;
    if (now % 2000 == 0 && id) { const auto n = nackFor(id, 0, maxFragments); sender.control(n.data(), n.size(), now); const auto m = nackFor(id > 1 ? id - 1 : 1, 0, maxFragments); sender.control(m.data(), m.size(), now); }
    while (sender.next(now, p)) if (p.bytes[0] == kindFragment) id = std::max(id, get32(&p.bytes[2]));
    const auto repairs = sender.stats().repairBytes; inWindow += repairs - repairBytes; repairBytes = repairs;
    if (now - windowStart >= 1000000) { peakPerWindow = std::max(peakPerWindow, inWindow); inWindow = 0; windowStart = now; }
  }
  const double oneSecond = target * 0.25 / 8, limit = oneSecond * 1.11;  // the contract's 25%, and 100 ms of bucket on top of 1 s of budget
  require(sender.stats().repairs > 1000, "The fixture did not exercise repairs");
  require(double(peakPerWindow) <= limit, "Retransmissions exceeded 25% of the target");
  require(double(sender.stats().repairBytes) / 9.999 <= limit && double(sender.stats().repairBytes) / 9.999 >= oneSecond * 0.9, "The budget is not 25% over the run");
  require(sender.stats().refused > 0, "The budget never refused anything");
  ++cases;
}

void tailTiming() {
  auto sender = provenSender(); Plain p; uint64_t now = 1000;
  const auto tails = [&] { size_t n = 0; while (sender.next(now, p)) if (p.bytes[0] == kindTail) { ++n; require(get32(&p.bytes[2]) == 1 && get64(&p.bytes[6]) == now && p.size == tailBytes, "Wrong TAIL"); } return n; };
  sender.video(video(1), false); while (sender.next(now, p)) {}
  const uint64_t done = now;
  now = done + 4999; require(!tails(), "TAIL came before 5 ms");
  now = done + 5000; require(tails() == 1, "No TAIL at 5 ms");
  now = done + 24999; require(!tails(), "The second TAIL came early");
  now = done + 25000; require(tails() == 1, "No second TAIL 20 ms later");
  now = done + 100000; require(!tails(), "A third TAIL");
  {  // A newer message within 5 ms: no TAIL.
    auto s = provenSender(); uint64_t t = 1000; s.video(video(1), false); while (s.next(t, p)) {}
    t += 3000; s.video(video(2), false); s.next(t, p); require(p.bytes[0] == kindFragment, "Fixture"); while (s.next(t, p)) require(p.bytes[0] == kindFragment, "Fixture");
    // That message arms its own TAIL, naming it.
    t += 5000; require(s.next(t, p) && p.bytes[0] == kindTail && get32(&p.bytes[2]) == 2, "TAIL did not name the latest message");
  }
  {  // A newer message between the two TAILs cancels the second one.
    auto s = provenSender(); uint64_t t = 1000; s.video(video(1), false); while (s.next(t, p)) {}
    t += 5000; require(s.next(t, p), "No TAIL"); t += 1000;
    s.video(video(2, 400000), false); uint64_t seen = 0; for (; t < 50000; t += 500) while (s.next(t, p)) seen += p.bytes[0] == kindTail;
    require(!seen, "A TAIL came while a newer message was sending");
  }
  ++cases;
}

void silence() {
  {  // Silence from the start and after the last FEEDBACK.
    Sender sender(20000000, 5000, fixedRandom);
    require(!sender.silent(5000 + feedbackSilenceUs - 1) && sender.silent(5000 + feedbackSilenceUs), "Not exactly 1 s from the start");
    const auto f = feedbackFor(0); sender.control(f.data(), f.size(), 900000);
    require(!sender.silent(900000 + feedbackSilenceUs - 1) && sender.silent(900000 + feedbackSilenceUs), "Not exactly 1 s after FEEDBACK");
  }
  {  // A reordered report is old news: its counters would run backwards.
    auto sender = provenSender(); const uint64_t now = 100000;
    const auto send = [&](const Bytes& f, uint64_t at) { return sender.control(f.data(), f.size(), at); };
    const auto newest = send(feedbackFor(50, 3, 7), now); require(newest.feedback && newest.received == 50 && newest.lost == 7, "A new FEEDBACK did not count");
    require(!send(feedbackFor(40, 2, 7), now + 1).feedback, "An older received count was taken");
    require(!send(feedbackFor(50, 3, 5), now + 2).feedback, "An older lost count was taken");
    require(sender.silent(now + feedbackSilenceUs), "An old report counted as life");
    const auto same = send(feedbackFor(50, 3, 7), now + 3); require(same.feedback, "A repeated count is the idle screen's report");
    const auto next = send(feedbackFor(60, 4, 8), now + 4); require(next.feedback && next.received == 60 && next.lost == 8, "The stream did not go on");
    auto long_ = provenSender();
    for (const uint32_t value : {0x70000000u, 0xe0000000u, 0xfffffff0u, 5u}) { const auto f = feedbackFor(value, 0, value); require(long_.control(f.data(), f.size(), now).feedback, "The counters did not carry past 2^32"); }
  }
  {  // Malformed or unknown plaintext is not life.
    auto sender = provenSender(); auto bad = feedbackFor(0); bad[1] = 1; sender.control(bad.data(), bad.size(), 500000);
    Bytes shortOne = feedbackFor(0); shortOne.pop_back(); sender.control(shortOne.data(), shortOne.size(), 500000);
    const Bytes other{9, 0, 0, 0}; sender.control(other.data(), other.size(), 500000);
    require(sender.silent(feedbackSilenceUs), "A malformed FEEDBACK counted");
  }
  {  // An idle screen with FEEDBACK running stays on UDP.
    auto sender = provenSender(); uint64_t now = 0; Plain p; uint32_t received = 0;
    for (; now < 5000000; now += 100000) { const auto f = feedbackFor(received); sender.control(f.data(), f.size(), now); while (sender.next(now, p)) ++received; require(!sender.silent(now), "An idle screen went silent"); }
  }
  {  // Host-to-client outage: FEEDBACK keeps coming, nothing it reports moves.
    auto sender = provenSender(); uint64_t now = 1000; Plain p; uint64_t stoppedAt = 0;
    sender.video(video(1, 5000, true), true); while (sender.next(now, p)) {}
    const auto good = feedbackFor(5); sender.control(good.data(), good.size(), now);
    const uint64_t outage = now;
    for (; now < 3000000 && !stoppedAt; now += 1000) {
      if (now % 16000 == 1000) sender.video(video(now, 5000), false);
      while (sender.next(now, p)) {}
      if (now % 100000 == 1000) sender.control(good.data(), good.size(), now);
      if (sender.silent(now)) stoppedAt = now;
    }
    require(stoppedAt && stoppedAt - outage <= 1250000, "A one-way outage never returned to WSS");
  }
  {  // Nor is a second or so of lost TAILs on an idle screen: nothing the client missed matters.
    auto sender = provenSender(); uint64_t now = 1000; Plain p;
    const auto f = feedbackFor(1);
    for (; now < 1500000; now += 100000) { sender.control(f.data(), f.size(), now); while (sender.next(now, p)) {} require(!sender.silent(now), "A blip of lost TAILs on an idle screen read as silence"); }
  }
  {  // A lost TAIL on an idle screen is not an outage: the frame itself arrived.
    auto sender = provenSender(); uint64_t now = 1000; Plain p; uint32_t sent = 0;
    sender.video(video(1, 5000, true), true); while (sender.next(now, p)) ++sent;
    for (; now < 3000000; now += 100000) { const auto f = feedbackFor(sent > 2 ? sent - 2 : 0, 1); sender.control(f.data(), f.size(), now); while (sender.next(now, p)) ++sent; require(!sender.silent(now), "A few unconfirmed datagrams counted as an outage"); }
  }
  {  // FEEDBACK runs, datagrams arrive, but no video is ever made whole: a freeze, so back to WSS.
    auto sender = provenSender(); uint64_t now = 1000; Plain p; uint32_t received = 0; uint64_t stoppedAt = 0;
    for (; now < 3000000 && !stoppedAt; now += 1000) {
      if (now % 16000 == 1000) sender.video(video(now, 5000), false);
      while (sender.next(now, p)) ++received;
      if (now % 100000 == 1000) { const auto f = feedbackFor(received); sender.control(f.data(), f.size(), now); }
      if (sender.silent(now)) stoppedAt = now;
    }
    require(stoppedAt && stoppedAt <= 1250000, "Video that never completed stayed on UDP");
    require(sender.silenceCause(stoppedAt) == 3, "Video never made whole was not cause 3");
  }
  {  // The other two causes: no FEEDBACK at all (1), FEEDBACK that shows nothing arriving (2).
    auto quiet = provenSender(); require(quiet.silent(feedbackSilenceUs) && quiet.silenceCause(feedbackSilenceUs) == 1, "Silence with no FEEDBACK was not cause 1");
    auto sender = provenSender(); uint64_t now = 1000; Plain p; uint64_t stoppedAt = 0;
    for (; now < 3000000 && !stoppedAt; now += 1000) {
      if (now % 16000 == 1000) sender.video(video(now, 5000), false);
      while (sender.next(now, p)) {}
      if (now % 100000 == 1000) { const auto f = feedbackFor(0); sender.control(f.data(), f.size(), now); }
      if (sender.silent(now)) stoppedAt = now;
    }
    require(stoppedAt && sender.silenceCause(stoppedAt) == 2, "Nothing arriving was not cause 2");
  }
  {  // A slow frame rate is progress however rarely it moves.
    auto sender = provenSender(); uint64_t now = 1000; Plain p; uint32_t received = 0, whole = 0;
    for (; now < 6000000; now += 1000) {
      if (now % 1500000 == 1000) { sender.video(video(now, 2000), false); }
      while (sender.next(now, p)) { ++received; if (p.bytes[0] == kindFragment) whole = get32(&p.bytes[2]); }
      if (now % 100000 == 1000) { const auto f = feedbackFor(received, whole); sender.control(f.data(), f.size(), now); }
      require(!sender.silent(now), "A slow screen went silent");
    }
  }
  {  // RTT from the echo, and FEEDBACK content.
    auto sender = provenSender(); const auto f = feedbackFor(7, 3, 2, 400000, 1500); const auto c = sender.control(f.data(), f.size(), 450000);
    require(c.feedback && c.highestWhole == 3 && c.received == 7 && c.lost == 2 && c.rttUs == 48500, "FEEDBACK was misread");
    const auto future = feedbackFor(8, 3, 2, 900000, 0); require(!sender.control(future.data(), future.size(), 450000).rttUs, "A future echo gave an RTT");
    const auto hold = feedbackFor(8, 3, 2, 400000, 60000); require(!sender.control(hold.data(), hold.size(), 450000).rttUs, "An absurd hold gave an RTT");
    const auto none = feedbackFor(8); require(!sender.control(none.data(), none.size(), 450000).rttUs, "No echo gave an RTT");
  }
  {  // KEYFRAME.
    auto sender = provenSender(); Bytes k(keyframeBytes); k[0] = kindKeyframe; put16(&k[2], 513);
    const auto c = sender.control(k.data(), k.size(), 1); require(c.keyFrame && c.epoch == 513, "KEYFRAME was misread");
    k[1] = 1; require(!sender.control(k.data(), k.size(), 1).keyFrame, "A malformed KEYFRAME counted");
    k[1] = 0; k.push_back(0); require(!sender.control(k.data(), k.size(), 1).keyFrame, "A long KEYFRAME counted");
  }
  ++cases;
}

void addressProof() {
  {  // Before an echo: TAIL on its timer and nothing else, whatever the Host or the client asks.
    Sender sender(20000000, 1000, fixedRandom); Plain p; uint64_t now = 1000; std::vector<uint64_t> tails;
    require(!sender.proven() && !sender.video(video(1, 5000, true), true).queued && !sender.audio(pcm(1)).queued, "Media was accepted before the proof");
    Bytes k(keyframeBytes); k[0] = kindKeyframe;
    for (; now < 2000000; now += 1000) {
      const auto n = nackFor(1, 0, maxFragments); sender.control(n.data(), n.size(), now);
      if (now % 100000 == 1000) require(!sender.control(k.data(), k.size(), now).keyFrame, "A KEYFRAME counted before the proof");
      while (sender.next(now, p)) { require(p.bytes[0] == kindTail, "Something other than TAIL left before the proof"); tails.push_back(now); }
    }
    require(tails.size() == 10, "TAIL is not on a 200 ms timer before the switch");
    for (size_t n = 1; n < tails.size(); ++n) require(tails[n] - tails[n - 1] == proofTailUs, "TAIL is not on a 200 ms timer before the switch");
    require(!sender.heldBytes() && !sender.waitingRepairs() && !sender.stats().repairs, "Something was held before the proof");
  }
  {  // Every wrong echo fails, and it neither proves nor keeps the path alive.
    Sender sender(20000000, 0, fixedRandom);
    for (uint64_t wrong : {uint64_t(0), testChallenge + 1, testChallenge ^ (1ull << 63), ~testChallenge}) {
      const auto f = feedbackFor(1, 0, 0, 0, 0, wrong); require(!sender.control(f.data(), f.size(), 900000).feedback, "A wrong echo counted");
    }
    require(!sender.proven() && sender.silent(feedbackSilenceUs), "A wrong echo proved the address or held off silence");
    Bytes shortOne = feedbackFor(1); shortOne.pop_back(); sender.control(shortOne.data(), shortOne.size(), 1); require(!sender.proven(), "A short FEEDBACK proved the address");
    const auto right = feedbackFor(1); require(sender.control(right.data(), right.size(), 1).feedback && sender.proven(), "The right echo did not count");
  }
  {  // A failed random source proves nothing, even if the client echoes zero.
    Sender sender(20000000, 0, [] { return uint64_t(0); }); Plain p; require(sender.next(0, p) && !get64(&p.bytes[14]), "Fixture");
    const auto f = feedbackFor(1, 0, 0, 0, 0, 0); sender.control(f.data(), f.size(), 1); require(!sender.proven(), "A zero challenge proved the address");
  }
  {  // New at least every second; the last two count, the one before does not.
    uint64_t counter = 100; Sender sender(20000000, 0, [&] { return ++counter; }); Plain p; std::vector<std::pair<uint64_t, uint64_t>> seen; uint64_t at = 0;
    for (uint64_t now = 0; now < 6000000; now += 1000) while (sender.next(now, p)) { const uint64_t c = get64(&p.bytes[14]); if (seen.empty() || seen.back().second != c) seen.push_back({now, c}); }
    require(seen.size() >= 6, "The challenge did not rotate");
    for (size_t n = 1; n < seen.size(); ++n) require(seen[n].first - seen[n - 1].first <= 1000000, "A challenge lived past a second");
    at = seen.back().first;
    const auto current = feedbackFor(1, 0, 0, 0, 0, seen.back().second), previous = feedbackFor(2, 0, 0, 0, 0, seen[seen.size() - 2].second), older = feedbackFor(3, 0, 0, 0, 0, seen[seen.size() - 3].second);
    require(sender.control(current.data(), current.size(), at).feedback, "The current challenge did not count");
    require(sender.control(previous.data(), previous.size(), at).feedback, "The previous challenge did not count");
    require(!sender.control(older.data(), older.size(), at).feedback, "A challenge older than two counted");
    require(!sender.silent(at + feedbackSilenceUs - 1) && sender.silent(at + feedbackSilenceUs), "A stale echo held off silence");
  }
  {  // Real Sender and Receiver: the client echoes what reached it, in every FEEDBACK, and a stale one lapses.
    uint64_t counter = 7000; Sender sender(20000000, 0, [&] { return counter += 13; }); Client c; Plain p; uint64_t now = 0, delivered = 0;
    for (; now < 400000; now += 1000) {
      c.now = 1000000 + now; while (sender.next(now, p)) c.feed(Bytes(p.bytes.data(), p.bytes.data() + p.size));
      c.tick(); for (const auto& control : c.out.control) sender.control(control.data(), control.size(), now); c.clear();
    }
    require(sender.proven(), "A real client did not prove its address");
    // From here its TAILs stop arriving: FEEDBACK keeps running on the old echo.
    uint64_t stoppedAt = 0;
    for (; now < 5000000 && !stoppedAt; now += 1000) {
      while (sender.next(now, p)) ++delivered;
      c.now = 1000000 + now; c.tick(); for (const auto& control : c.out.control) sender.control(control.data(), control.size(), now); c.clear();
      if (sender.silent(now)) stoppedAt = now;
    }
    require(delivered && stoppedAt && stoppedAt < 3400000, "An echo that went stale kept UDP alive");
  }
  {  // A busy stream with no end-of-frame gap still gets challenges, and the timer's TAIL names only finished messages.
    auto sender = provenSender(); Plain p; std::vector<uint32_t> named; uint64_t tails = 0;
    sender.video(video(1, maxMessageBytes - 12, true), true);
    for (uint64_t now = 1000; now < 200000 + 100000; now += 500) while (sender.next(now, p)) if (p.bytes[0] == kindTail) { ++tails; named.push_back(get32(&p.bytes[2])); }
    require(tails == 1 && named[0] == 0, "The timer's TAIL did not name the last finished message");
  }
  require(randomChallenge() != randomChallenge() && randomChallenge(), "The random source is not working");
  ++cases;
}

void resendMemory() {
  // Offered twice what the pacer takes, so the buffer is as full as the rate allows.
  const auto peakAt = [](int target) {
    auto sender = provenSender(target); Plain p; uint64_t peak = 0, nextFrame = 1000; bool key = true;
    const size_t frame = size_t(target / 8 / 60 * 6); const Bytes plainFrame = video(1, frame), keyFrame = video(1, frame, true);
    for (uint64_t now = 1000; now < 3000000; now += 200) {
      if (now >= nextFrame) { nextFrame += 16667; key = sender.video(key ? keyFrame : plainFrame, key).requestKey; }
      while (sender.next(now, p)) {}
      peak = std::max<uint64_t>(peak, sender.heldBytes());
    }
    return peak;
  };
  const auto expected = [](int target, double seconds) { return double(target) / 8 * 3 * seconds; };
  uint64_t peaks[2]; size_t at = 0;
  for (const int target : {20000000, 50000000}) {
    const uint64_t peak = peakAt(target); peaks[at++] = peak;
    const double frames = 3.0 * target / 8 / 60 * 6;  // whole messages on top of the rate: one kept, one sending, one waiting
    require(double(peak) >= expected(target, 0.6) * 0.9, "The buffer did not keep 600 ms at this rate");
    require(double(peak) <= expected(target, 0.6 + 0.25) + frames, "The buffer outgrew 600 ms of pacing plus the send queue");
    require(peak <= maxKeptBytes, "The buffer outgrew its 16 MiB bound");
  }
  // Past any real target the hard bound must take over from the 600 ms.
  const uint64_t beyond = peakAt(150000000);
  require(double(beyond) <= double(maxKeptBytes) + expected(150000000, 0.25) + 3.0 * 150000000 / 8 / 60 * 6, "The hard bound on kept bytes failed");
  std::printf("Resend buffer, offered twice the pacer rate: peak held %.1f MiB at 20 Mbps, %.1f at 50; bound 16 MiB of kept bytes.\n", double(peaks[0]) / 1048576, double(peaks[1]) / 1048576);
  ++cases;
}

void keyFrameMerging() {
  Bytes request(keyframeBytes); request[0] = kindKeyframe; put16(&request[2], 1);
  const auto until = [](Sender& sender, uint64_t& now) {
    Plain p; Fragment f;
    for (;; now += 500) while (sender.next(now, p)) if (parseFragment(p.bytes.data(), p.size, f) && f.index + 1u == f.count) return;
  };
  {  // Unknown RTT: the assumed 50 ms.
    auto sender = provenSender(); uint64_t now = 1000; const auto asked = [&] { return sender.control(request.data(), request.size(), now).keyFrame; };
    // A request alone is not in flight: the Host may rightly ignore it, and the next must not be lost.
    require(asked() && asked(), "With no IDR in flight a request was merged");
    require(sender.video(video(1, 3000, true), true).queued && !asked(), "A request was not merged while the IDR waited");
    Plain p; require(sender.next(now, p) && p.bytes[0] == kindFragment && !asked(), "A request was not merged while the IDR was sending");
    until(sender, now); const uint64_t sent = now;
    require(!asked(), "A request was not merged as the IDR finished");
    now = sent + 49999; require(!asked(), "A request was not merged before a round trip passed");
    now = sent + 50000; require(asked(), "A request after the round trip was merged");
    require(sender.stats().mergedKeys == 4, "Merged requests were not counted");
  }
  {  // A known RTT, and the epoch still reads on a merged request.
    auto sender = provenSender(); uint64_t now = 1000; const auto f = feedbackFor(0, 0, 0, now - 1000 + 1000, 0); (void)f;
    const auto sample = feedbackFor(0, 0, 0, 1, 0); sender.control(sample.data(), sample.size(), 30001); require(sender.rttUs() == 30000, "Fixture");
    now = 30001; sender.video(video(1, 3000, true), true); until(sender, now); const uint64_t sent = now;
    now = sent + 29999; const auto merged = sender.control(request.data(), request.size(), now); require(!merged.keyFrame && merged.epoch == 1, "A merged request lost its epoch, or was not merged");
    now = sent + 30000; require(sender.control(request.data(), request.size(), now).keyFrame, "A request after the known RTT was merged");
  }
  {  // P-frames in flight merge nothing, and a second IDR's own life counts, not the first's.
    auto sender = provenSender(); uint64_t now = 1000;
    sender.video(video(1, 3000), false); require(sender.control(request.data(), request.size(), now).keyFrame, "A P-frame merged a request");
    until(sender, now); sender.video(video(2, 3000, true), true); until(sender, now); now += 50000; require(sender.control(request.data(), request.size(), now).keyFrame, "Fixture");
    sender.video(video(3, 3000, true), true); require(!sender.control(request.data(), request.size(), now).keyFrame, "A second IDR did not merge");
  }
  ++cases;
}

// LOSS and recovery frames (classRecovery): the client says which frames its decoder lacks,
// the Host answers with a frame coded from older ones instead of an IDR.
void lossRecovery() {
  Bytes request(keyframeBytes); request[0] = kindKeyframe; put16(&request[2], 1);
  const auto givenUp = [](Client& c) {  // key 1, P 2, then P 3 is lost
    c.feed(split(classVideo, 1, video(10, 100, true))); c.feed(split(classVideo, 2, video(20)));
    const auto lost = split(classVideo, 3, video(30, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.clear();
    c.at(c.now + 90000);
  };
  {  // The give-up sends LOSS then KEYFRAME: released Hosts ignore the first and answer the second.
    Client c(20000); givenUp(c);
    require(c.receiver.awaitingRecovery() && c.out.control.size() == 2 && c.out.control[0][0] == kindLoss && c.out.control[1][0] == kindKeyframe, "No LOSS before the KEYFRAME");
    const Bytes& loss = c.out.control[0];
    require(loss.size() == lossBytes && !loss[1] && get16(&loss[2]) == 1 && get32(&loss[4]) == 3 && get64(&loss[8]) == 20, "LOSS does not name the epoch, the message and the decoder's last frame");
    c.clear(); c.feed(split(classVideo, 4, video(40)));
    require(!c.videos(), "A P-frame ended a loss wait");
    c.feed(split(classRecovery, 5, video(50, 3000)));
    require(c.videos() == 1 && !c.out.messages[0].key && c.out.messages[0].pts == 50 && !c.receiver.awaitingKey(), "A recovery frame did not end the wait");
    c.clear(); c.feed(split(classVideo, 6, video(60))); require(c.videos() == 1, "P-frames after the recovery frame were dropped");
    c.at(c.now + 2000000); require(!c.controls(kindKeyframe) && !c.controls(kindLoss), "Requests went on after the recovery frame");
  }
  {  // The repeat waits while a recovery frame visibly arrives, and repeats LOSS with KEYFRAME.
    Client c(20000); givenUp(c); const uint64_t asked = c.now; c.clear();
    const auto answer = split(classRecovery, 4, video(40, 5000)); c.feed(answer[0]); c.feed(answer[1]);
    c.at(asked + keyframeIntervalUs); require(!c.controls(kindKeyframe) && !c.controls(kindLoss), "The repeat went while a recovery frame was arriving");
    Client d(20000); givenUp(d); d.clear(); d.at(d.now + keyframeIntervalUs);
    require(d.controls(kindLoss) == 1 && d.controls(kindKeyframe) == 1 && d.out.control[0][0] == kindLoss, "The repeat dropped LOSS");
  }
  {  // Nothing delivered on this path yet: no last frame to name, so only a key frame ends it.
    Client c(20000); const auto lost = split(classVideo, 1, video(10, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000);
    require(c.receiver.awaitingKey() && !c.receiver.awaitingRecovery() && !c.controls(kindLoss) && c.controls(kindKeyframe) == 1, "A LOSS named no frame");
    c.clear(); c.feed(split(classRecovery, 2, video(20))); require(!c.videos() && c.receiver.awaitingKey(), "A recovery frame ended a key wait");
  }
  {  // A refused delivery: the decoder may lack what was delivered, so only a key frame ends it.
    Client c(20000); c.feed(split(classVideo, 1, video(10, 100, true))); c.clear();
    c.receiver.queueFull(c.now); c.tick(); require(!c.controls(kindLoss) && !c.receiver.awaitingRecovery(), "A full queue sent LOSS");
    c.clear(); c.feed(split(classRecovery, 2, video(20))); require(!c.videos(), "A recovery frame passed a full queue");
    Client d(20000); givenUp(d); d.receiver.queueFull(d.now); d.clear(); d.feed(split(classRecovery, 4, video(40)));
    require(!d.videos() && !d.receiver.awaitingRecovery(), "A full queue during a loss wait still took a recovery frame");
  }
  {  // Tiles are no encoder frame: the last frame named is the video before them.
    Client c(20000); c.feed(split(classVideo, 1, video(10, 100, true)));
    Bytes tile = video(19, 100); tile[12] = 'T'; tile[13] = 'S'; tile[14] = 'L'; tile[15] = '1'; c.feed(split(classVideo, 2, tile));
    const auto lost = split(classVideo, 3, video(20, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.clear(); c.at(c.now + 90000);
    const auto loss = c.taken(kindLoss); require(loss.size() == 1 && get64(&loss[0][8]) == 10, "A tile was named as the decoder's last frame");
  }
  {  // WSS fed the decoder, or a new epoch began: no frame of this path is the decoder's last.
    for (int k = 0; k < 2; ++k) {
      Client c(20000); c.feed(split(classVideo, 1, video(10, 100, true)));
      if (k) { c.receiver.setEpoch(2); c.receiver.setEpoch(1); } else c.receiver.keyFrameOtherPath();
      const auto lost = split(classVideo, 2, video(20, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.clear(); c.at(c.now + 90000);
      require(c.receiver.awaitingKey() && !c.controls(kindLoss), "A frame from before WSS or an epoch change was named");
    }
  }
  {  // A recovery frame outside any wait is an ordinary P-frame.
    Client c(20000); c.feed(split(classVideo, 1, video(10, 100, true))); c.feed(split(classRecovery, 2, video(20)));
    require(c.videos() == 2 && !c.receiver.awaitingKey(), "A recovery frame outside a wait was dropped");
  }
  {  // A recovery frame under repair makes the older unfinished frames pointless, like a key frame.
    Client c(20000); givenUp(c); c.clear();
    const auto old = split(classVideo, 4, video(40, 5000)); const auto answer = split(classRecovery, 5, video(50, 5000));
    c.feed(old[1]); c.feed(answer[0]); c.feed(answer[1]); c.feed(answer[2]); c.feed(answer[3]); c.feed(answer[4]);
    require(c.videos() == 1 && c.out.messages[0].pts == 50, "An older unfinished frame held up the recovery frame");
  }
  {  // Host: LOSS reads only once proven, names its fields, and merges the KEYFRAME that follows it.
    Sender unproven(20000000, 0, fixedRandom); Bytes loss(lossBytes); loss[0] = kindLoss; put16(&loss[2], 1); put32(&loss[4], 3); put64(&loss[8], 20);
    require(!unproven.control(loss.data(), loss.size(), 0).loss, "An unproven address read a LOSS");
    auto sender = provenSender(); uint64_t now = 1000;
    auto c = sender.control(loss.data(), loss.size(), now);
    require(c.loss && c.epoch == 1 && c.lostId == 3 && c.lastGood == 20, "LOSS was not read");
    Bytes bad = loss; bad[1] = 1; require(!sender.control(bad.data(), bad.size(), now).loss, "A malformed LOSS was read");
    bad = loss; bad.pop_back(); require(!sender.control(bad.data(), bad.size(), now).loss, "A short LOSS was read");
    require(sender.control(request.data(), request.size(), now).keyFrame, "A LOSS the caller did not act on merged a KEYFRAME");
    sender.recovering(now);
    require(!sender.control(request.data(), request.size(), now).keyFrame && !sender.control(loss.data(), loss.size(), now).loss, "The KEYFRAME or a repeat LOSS after an answered LOSS was not merged");
    require(sender.control(request.data(), request.size(), now + keyframeIntervalUs / 2).keyFrame, "A LOSS left unanswered merged KEYFRAMEs for good");
    // The answer: fragments carry classRecovery, and so do their repairs.
    sender.recovering(now); sender.video(video(1, 100), false);
    require(sender.video(video(2, 3000), false, true).queued && sender.heldBytes() == video(2, 3000).size(), "The recovery frame did not replace the P-frames waiting");
    Plain p; Fragment f; std::vector<uint8_t> classes; for (; sender.next(now, p); ) if (parseFragment(p.bytes.data(), p.size, f)) classes.push_back(f.cls);
    require(classes.size() == 3 && classes[0] == classRecovery && classes[2] == classRecovery, "Recovery fragments did not carry their class");
    const auto nack = nackFor(1, 1, 1); sender.control(nack.data(), nack.size(), now + 10000);
    bool repaired = false; for (uint64_t t = now + 10000; t < now + 20000; t += 500) while (sender.next(t, p)) if (parseFragment(p.bytes.data(), p.size, f) && f.id == 1) repaired = f.cls == classRecovery && f.index == 1;
    require(repaired, "A repaired recovery fragment lost its class");
    require(!sender.control(request.data(), request.size(), now + 20000).keyFrame, "A KEYFRAME right after the recovery frame went out was not merged");
    require(sender.stats().recoveries == 2, "Recoveries were not counted");
  }
  {  // An IDR already in flight answers a LOSS: the client takes a key frame in any wait.
    auto sender = provenSender(); Bytes loss(lossBytes); loss[0] = kindLoss; put16(&loss[2], 1); put64(&loss[8], 20);
    sender.video(video(1, 3000, true), true); require(!sender.control(loss.data(), loss.size(), 1000).loss, "A LOSS beside an IDR in flight was acted on");
  }
  {  // A recovery frame behind a refused P-frame is refused too: that backlog needs a key frame.
    auto sender = provenSender(); Sender::Admission a; size_t frames = 0;
    do { a = sender.video(video(frames, 30000), false); ++frames; } while (a.queued);
    require(sender.video(video(9999, 3000), false, true).requestKey, "A recovery frame passed a backlog that needs a key frame");
  }
  ++cases;
}

void backoff() {
  ProbeBackoff b; uint64_t now = 1000; require(b.mayProbe(now), "A new session could not probe");
  for (uint64_t delay : {30, 60, 120, 240, 300, 300, 300}) {
    b.failed(now); const uint64_t until = now + delay * 1000000;
    require(!b.mayProbe(until - 1) && b.mayProbe(until), "Wrong backoff schedule"); now = until;
  }
  b.reset(); b.failed(now); require(b.mayProbe(now + 30000000), "reset did not restart the schedule");
  ++cases;
}

void sendQueue() {
  auto sender = provenSender(); Plain p; const size_t limit = size_t(20000000 / 8 * 1.25 * 0.25);
  size_t frames = 0; Sender::Admission a;
  do { a = sender.video(video(frames, 30000), false); ++frames; require(frames < 1000, "The queue has no bound"); } while (a.queued);
  require(a.requestKey && sender.heldBytes() <= limit + 40000, "A P-frame queue past its bound did not ask for a key frame");
  require(sender.video(video(9999, 30000), false).requestKey, "A P-frame was accepted while a key frame is due");
  // The key frame replaces what waits and is the next to start, leaving no gap.
  const Bytes key = video(10000, 30000, true); require(sender.video(key, true).queued && sender.heldBytes() == key.size(), "The key frame did not replace the queue");
  require(sender.next(0, p), "Nothing sent"); Fragment f; require(parseFragment(p.bytes.data(), p.size, f) && f.id == 1, "A dropped message took an ID");
  // A key frame larger than the bound is no backlog: asking for another would never end.
  auto big = provenSender(); require(big.video(video(1, 1200000, true), true).queued, "A large key frame was refused");
  for (uint64_t n = 2; n < 20; ++n) require(big.video(video(n, 30000), false).queued, "A large key frame blocked the P-frames behind it");
  // A message under way is finished, not cut.
  auto s = provenSender(); s.video(video(1, 30000), false); require(s.next(0, p), "Nothing sent"); s.video(video(2, 30000), false); s.video(video(3, 30000, true), true);
  std::vector<uint32_t> ids; for (uint64_t t = 0; t < 200000; t += 500) while (s.next(t, p)) if (parseFragment(p.bytes.data(), p.size, f) && (ids.empty() || ids.back() != f.id)) ids.push_back(f.id);
  require(ids == std::vector<uint32_t>({1, 2}), "IDs are not consecutive across a replaced queue");
  sender.stop(); require(!sender.next(1000000, p) && !sender.heldBytes(), "A stopped Sender kept sending or holding");
  ++cases;
}

void pathRule() {
  PathState s;
  require(!acceptVideo(s, Path::wss, 1, false, 10), "A P-frame passed before any key frame");
  require(!acceptVideo(s, Path::udp, 1, false, 10), "A P-frame passed before any key frame");
  require(acceptVideo(s, Path::wss, 1, true, 10) && s.active == Path::wss, "A key frame on WSS failed");
  require(acceptVideo(s, Path::wss, 1, false, 20), "A P-frame on the active path failed");
  require(!acceptVideo(s, Path::udp, 1, false, 30), "A P-frame on the other path passed");
  require(!acceptVideo(s, Path::wss, 1, false, 20) && !acceptVideo(s, Path::wss, 1, false, 19), "A P-frame not newer passed");
  require(!acceptVideo(s, Path::udp, 1, true, 20) && s.active == Path::wss, "A key frame not newer switched the path");
  require(acceptVideo(s, Path::udp, 1, true, 40) && s.active == Path::udp, "A key frame on UDP did not switch");
  require(!acceptVideo(s, Path::wss, 1, false, 50), "A straggler on the old path passed");
  require(acceptVideo(s, Path::udp, 1, false, 50), "A P-frame on the new path failed");
  require(!acceptVideo(s, Path::wss, 1, true, 45), "A stale key frame on the old path passed");
  require(acceptVideo(s, Path::wss, 1, true, 60) && s.active == Path::wss, "Falling back on a key frame failed");
  // A new epoch restarts PTS, and needs a key frame.
  require(!acceptVideo(s, Path::wss, 2, false, 70), "A new epoch started on a P-frame");
  require(acceptVideo(s, Path::wss, 2, true, 1) && s.lastPts == 1 && s.epoch == 2, "A new epoch's lower PTS failed");
  require(!acceptVideo(s, Path::wss, 1, true, 1000), "An old epoch's key frame passed");
  ++cases;
}

void maliciousPlaintext() {
  // Each one is whole and authenticated by assumption, and must deliver nothing.
  std::vector<Bytes> malformed;
  auto wrongKind = video(1); wrongKind[0] = 2; malformed.push_back(wrongKind);
  auto cursorKind = video(1); cursorKind[0] = 3; malformed.push_back(cursorKind);
  auto zeroKind = video(1); zeroKind[0] = 0; malformed.push_back(zeroKind);
  auto flags = video(1); flags[1] = 2; malformed.push_back(flags);
  malformed.push_back(Bytes(12, 1));  // the envelope alone
  for (const auto& message : malformed) {
    Client c; c.feed(split(classVideo, 1, message));
    c.feed(tailFor(1, 1)); c.at(c.now + 1000000);
    require(!c.videos() && c.receiver.awaitingKey(), "A malformed message was delivered");
  }
  auto audioFlag = pcm(1); audioFlag[1] = 1;
  auto audioKind = pcm(1); audioKind[0] = 1;
  for (const auto& message : {audioFlag, audioKind, Bytes(12, 2)}) { Client c; c.feed(split(classAudio, 1, message)); require(c.out.messages.empty(), "Malformed audio was delivered"); }
  {
    const Bytes message = video(1, 5000); auto parts = split(classVideo, 1, message);
    const auto expect = [&](Bytes bad, const char* reason) { Client c; c.feed(bad); require(!c.receiver.windowMessages() && !c.receiver.heldBytes(), reason); };
    auto a = parts[0]; a[1] = classRecovery + 1; expect(a, "A bad class was accepted");
    a = parts[0]; put32(&a[2], 0); expect(a, "Message ID 0 was accepted");
    a = parts[0]; put16(&a[8], 0); expect(a, "Count 0 was accepted");
    a = parts[0]; put16(&a[8], 2049); expect(a, "Count 2049 was accepted");
    a = parts[0]; put16(&a[6], get16(&a[8])); expect(a, "An index past the count was accepted");
    a = parts[0]; a.pop_back(); expect(a, "A short middle fragment was accepted");
    a = parts[0]; a.push_back(0); expect(a, "A long fragment was accepted");
    a.assign(parts[0].begin(), parts[0].begin() + fragmentHeaderBytes); expect(a, "An empty fragment was accepted");
    expect(Bytes{}, "An empty plaintext was accepted");
    // A count that changes inside a message is ignored; the message still finishes.
    Client c; auto changed = parts[1]; put16(&changed[8], uint16_t(parts.size() + 1)); c.feed(parts[0]); c.feed(changed); c.feed(parts[1]);
    for (size_t i = 2; i < parts.size(); ++i) c.feed(parts[i]);
    require(c.out.messages.size() == 1 && c.out.messages[0].bytes == message, "A changed count disturbed a message");
  }
  {  // TAIL and sizes.
    Client c; auto t = tailFor(1, 1); t[1] = 1; c.feed(t); t = tailFor(1, 1); t.pop_back(); c.feed(t); t = tailFor(1, 1); t.push_back(0); c.feed(t);
    c.at(c.now + 200000); require(!get32(&c.taken(kindFeedback)[0][6]), "A malformed TAIL counted");
    // Huge IDs cannot make it spin or grow.
    c.feed(tailFor(0xffffffffu, 1)); c.feed(tailFor(0xfffffff0u, 2)); require(c.receiver.windowMessages() <= videoWindow, "A huge TAIL grew the window");
    c.at(c.now + 2000000);
    auto part = split(classVideo, 0xffffffffu, video(1, 5000, true)); c.feed(part[0]); require(c.receiver.windowMessages() <= videoWindow, "A huge ID grew the window");
  }
  ++cases;
}

void windowBounds() {
  Client c(20000); c.feed(split(classVideo, 1, video(1, 5000, true))[0]);
  for (uint32_t id = 3; id < 1000; id += 7) {
    const auto parts = split(classVideo, id, video(id, 5000));
    c.feed(parts[1]);
    require(c.receiver.windowMessages() <= videoWindow && c.receiver.heldBytes() <= maxVideoBytes && c.receiver.gaps() <= maxGaps, "The receive state grew");
  }
  // One enormous message then many more: the byte bound holds, and delivery recovers.
  Client d(20000); uint32_t id = 1;
  for (; id <= 40; ++id) { d.feed(split(classVideo, id, video(id, maxMessageBytes - 12, true))[1]); require(d.receiver.heldBytes() <= maxVideoBytes, "The byte bound failed"); }
  d.at(d.now + 2000000); d.clear();
  const Bytes key = video(9000, 2000, true); d.feed(split(classVideo, id + 200, key)); require(d.videos() == 1, "Delivery did not recover");
  // Gaps are capped too.
  Client g(20000); const auto many = split(classVideo, 1, video(1, 2000 * fragmentPayload));
  for (uint32_t m = 1; m <= 8; ++m) {
    Bytes last = many.back(); put32(&last[2], m); put16(&last[8], get16(&many.back()[8]));
    g.feed(last); require(g.receiver.gaps() <= maxGaps, "Gaps grew");
    g.feed(tailFor(m, 1)); require(g.receiver.gaps() <= maxGaps, "Gaps grew");
  }
  ++cases;
}

void fuzz() {
  std::mt19937_64 random(0x75647031);
  const auto byte = [&] { return uint8_t(random() & 0xff); };
  {  // Random datagrams: nothing opens.
    Pair p; uint8_t datagram[maxDatagram], back[maxPlaintext]; long long opened = 0;
    for (int n = 0; n < 300000; ++n) {
      const size_t size = size_t(random() % (maxDatagram + 1));
      for (size_t i = 0; i < size; ++i) datagram[i] = byte();
      if (size) datagram[0] = (n & 1) ? 3 : datagram[0];
      opened += p.clientOpen.open(datagram, size, back) != 0;
    }
    require(!opened, "A random datagram opened");
  }
  {  // Random plaintext straight into both state machines: bounds hold, nothing delivered.
    Client c; auto sender = provenSender(); Plain p; Bytes plain(maxPlaintext); long long delivered = 0;
    for (long long n = 0; n < 2000000; ++n) {
      const size_t size = size_t(random() % 64) + (n % 16 == 0 ? size_t(random() % maxPlaintext) : 0);
      plain.assign(size, 0); for (auto& b : plain) b = byte();
      if (size && n % 2) plain[0] = uint8_t(1 + random() % 5);
      c.feed(plain); sender.control(plain.data(), size, c.now);
      c.now += 50; if (n % 8 == 0) c.tick();
      while (sender.next(c.now, p)) {}
      delivered += c.out.messages.size(); c.clear();
      if (n % 1024 == 0) require(c.receiver.windowMessages() <= videoWindow && c.receiver.gaps() <= maxGaps && c.receiver.heldBytes() <= maxVideoBytes + audioPartials * maxAudioFragments * fragmentPayload && sender.waitingRepairs() <= maxRepairs, "State grew under random plaintext");
    }
    require(!delivered, "Random plaintext produced a delivered message");
  }
  {  // Real traffic with junk, duplicates, loss and reordering mixed in: what comes out is exactly what went in.
    Client c(4000); std::vector<Bytes> sent; std::deque<Bytes> wire; uint32_t nextId = 1; uint64_t pts = 1; size_t delivered = 0; uint64_t lastPts = 0;
    Bytes plain;
    for (int n = 0; n < 30000; ++n) {
      if (wire.size() < 8) {
        const bool key = random() % 50 == 0; const Bytes m = video(pts, size_t(random() % 6000) + 10, key, 1, uint32_t(pts)); ++pts;
        sent.push_back(m); for (auto& f : split(classVideo, nextId, m)) wire.push_back(f);
        ++nextId;
      }
      size_t at = size_t(random() % std::min<size_t>(wire.size(), 6)); const Bytes f = wire[at];
      if (random() % 20) wire.erase(wire.begin() + std::ptrdiff_t(at));
      if (random() % 10) c.feed(f);
      plain.assign(size_t(random() % 40), 0); for (auto& b : plain) b = byte();
      if (!plain.empty()) { if (plain[0] == kindFragment || plain[0] == kindTail) plain[0] = 0; c.feed(plain); }
      c.now += 700; c.tick();
      for (const auto& m : c.out.messages) {
        require(m.video && m.pts > lastPts && m.pts <= sent.size() && m.bytes == sent[size_t(m.pts - 1)], "A delivered message is not an input message, or is out of order");
        lastPts = m.pts; ++delivered;
      }
      c.clear();
    }
    require(delivered > 100, "The fixture delivered too little");
  }
  ++cases;
}

// A simulated link. Delay, jitter, a rate, and the listed faults.
struct Scenario {
  std::string name; double lossPct = 0; bool bursts = false; double reorderPct = 0, duplicatePct = 0;
  uint64_t outageFrom = 0, outageTo = 0; bool oneWay = false; uint64_t rttUs = 30000; size_t idrBytes = 300000; uint64_t runUs = 30000000;
  bool spoofed = false;  // the client holds the keys but its address is someone else's: it receives nothing
  uint64_t idleFromUs = 0, idleToUs = 0;  // an unchanged screen with no sound: no media at all
};
struct Result {
  int generated = 0, delivered = 0, idrs = 0, keysDelivered = 0, keyRequests = 0, giveUps = 0, recoveries = 0;
  uint64_t stallUs = 0, longestKeyWaitUs = 0, gaveUpAfterUs = 0, repairs = 0, refused = 0, nacks = 0, mediaDatagrams = 0, bytesToAddress = 0, videoBytes = 0, waitUs = 0;
  double overheadPct = 0; bool gaveUp = false, proven = false;
};
class Link {
 public:
  Link(uint64_t seed, const Scenario& s, double rateBps, bool forward) : random_(seed), s_(s), bytesPerUs_(rateBps / 8e6), forward_(forward) {}
  void send(const uint8_t* data, size_t size, uint64_t now) {
    if ((forward_ && s_.spoofed) || down(now) || uniform() * 100 < s_.lossPct) return;
    free_ = std::max(free_, now) + uint64_t(double(size + 28) / bytesPerUs_);
    uint64_t at = free_ + s_.rttUs / 2 + uint64_t(uniform() * 1000);
    if (uniform() * 100 < s_.reorderPct) at += 2000 + uint64_t(uniform() * 6000);
    queue_.push({at, order_++, Bytes(data, data + size)});
    if (uniform() * 100 < s_.duplicatePct) queue_.push({at + 500 + uint64_t(uniform() * 3000), order_++, Bytes(data, data + size)});
  }
  template <class F> void arrive(uint64_t now, F handle) {
    while (!queue_.empty() && queue_.top().at <= now) { handle(queue_.top().bytes); queue_.pop(); }
  }
 private:
  struct Packet { uint64_t at, order; Bytes bytes; bool operator>(const Packet& o) const { return at != o.at ? at > o.at : order > o.order; } };
  double uniform() { return double(random_() >> 11) / double(1ull << 53); }
  bool down(uint64_t now) {
    if (now >= s_.outageFrom && now < s_.outageTo && (forward_ || !s_.oneWay)) return true;
    if (!s_.bursts) return false;
    // Bursts are shared by both directions: one 20-120 ms blackout in each 2 s slot.
    const uint64_t slot = now / 2000000;
    std::mt19937_64 slotRandom(slot * 977 + 5);
    const uint64_t start = slot * 2000000 + slotRandom() % 1800000, length = 20000 + slotRandom() % 100000;
    return now >= start && now < start + length;
  }
  std::mt19937_64 random_; Scenario s_; double bytesPerUs_; bool forward_; uint64_t free_ = 0, order_ = 0;
  std::priority_queue<Packet, std::vector<Packet>, std::greater<Packet>> queue_;
};

// `recovery`: the Host answers LOSS with a frame `recoveryScale` times a P-frame's size. x264
// measured 1.3x after one lost frame and 5.5-6.1x after 4-15 lost on scrolling text, 1.3-1.6x on
// noise (x264-reference-recovery in x264_encoder_test.cpp), so 6 is the pessimistic end.
Result simulate(const Scenario& s, uint64_t seed, bool recovery = false, double recoveryScale = 6) {
  Result result; Pair keys; constexpr int target = 20000000; constexpr uint64_t frameUs = 16667, audioUs = 10000;
  Link forward(seed, s, 100e6, true), reverse(seed + 1, s, 100e6, false); std::mt19937_64 sizes(seed + 2);
  std::mt19937_64 challenges(seed + 3); Sender sender(target, 1000, [&] { return challenges() | 1; });
  Receiver receiver(1, 1000); Receiver::Output out; PathState path; Plain plain;
  std::vector<Bytes> sentFrames; std::vector<bool> answers; uint64_t nextFrame = 1000, nextAudio = 1000, lastDelivered = 0, audioPts = 1, waitingSince = 0; bool forceIdr = true, answerNext = false, widened = false; int64_t lastFrame = -1; uint8_t sealed[maxDatagram], opened[maxPlaintext];
  const auto byFrame = [&](uint64_t pts) { return int64_t(pts / frameUs) - 1; };
  uint64_t now = 1000;
  for (; now < s.runUs; now += 100) {
    // No media is no stall: the clock restarts when the screen does.
    if (now >= s.idleFromUs && now < s.idleToUs) { nextFrame = now + frameUs; nextAudio = now + audioUs; lastDelivered = 0; }
    if (now >= nextFrame && now + 500000 < s.runUs) {  // the last half second drains the link
      nextFrame += frameUs;
      // Offered whether or not the Host would: the Sender itself must refuse before the proof.
      const bool key = forceIdr, answer = !key && answerNext; const int frame = int(sentFrames.size());
      const size_t pSize = 24000 + size_t(sizes() % 12000), size = key ? s.idrBytes : answer ? size_t(double(pSize) * recoveryScale) : pSize;
      sentFrames.push_back(video(uint64_t(frame + 1) * frameUs + 5, size, key, 1, uint32_t(frame))); answers.push_back(answer);
      const auto admitted = sender.video(sentFrames.back(), key, answer);
      if (sender.proven()) {
        ++result.generated;
        if (key) { forceIdr = false; ++result.idrs; }
        answerNext = false;
        if (admitted.requestKey) forceIdr = true;
      }
    }
    if (now >= nextAudio) { nextAudio += audioUs; sender.audio(pcm(audioPts++)); }
    while (sender.next(now, plain)) {
      if (plain.bytes[0] == kindFragment) ++result.mediaDatagrams;
      const size_t n = keys.hostSeal.seal(plain.bytes.data(), plain.size, sealed); require(n != 0, "Seal failed in the simulation");
      result.bytesToAddress += n; forward.send(sealed, n, now);
    }
    forward.arrive(now, [&](const Bytes& d) { const size_t n = keys.clientOpen.open(d.data(), d.size(), opened); if (n) receiver.datagram(opened, n, now, out); });
    if (now % 1000 == 0) {
      receiver.tick(now, out);
      if (receiver.awaitingKey() && !waitingSince) { waitingSince = now; ++result.giveUps; }
      if (!receiver.awaitingKey() && waitingSince) { result.longestKeyWaitUs = std::max(result.longestKeyWaitUs, now - waitingSince); result.waitUs += now - waitingSince; waitingSince = 0; }
      for (const auto& c : out.control) { result.nacks += c[0] == kindNack; const size_t n = keys.clientSeal.seal(c.data(), c.size(), sealed); require(n != 0, "Seal failed in the simulation"); reverse.send(sealed, n, now); }
      out.control.clear();
      if (s.spoofed && now % 50000 == 0) {
        // It holds the keys, so it can seal anything: a guessed echo, and repair and IDR requests.
        Bytes forged[] = {feedbackFor(5, 1, 0, 0, 0, challenges()), nackFor(1, 0, maxFragments), Bytes{kindKeyframe, 0, 0, 1}};
        for (const auto& c : forged) { const size_t n = keys.clientSeal.seal(c.data(), c.size(), sealed); require(n != 0, "Seal failed in the simulation"); reverse.send(sealed, n, now); }
      }
    }
    for (auto& m : out.messages) {
      if (!m.video) continue;
      const int64_t frame = byFrame(m.pts);
      require(frame >= 0 && frame < int64_t(sentFrames.size()) && m.bytes == sentFrames[size_t(frame)], "The simulation delivered bytes it never sent");
      require(frame > lastFrame && (m.key || frame == lastFrame + 1 || answers[size_t(frame)]), "The simulation delivered a broken P-frame chain");
      require(acceptVideo(path, Path::udp, m.epoch, m.key, m.pts), "The path rule refused a UDP frame");
      if (lastDelivered) result.stallUs = std::max(result.stallUs, now - lastDelivered);
      lastDelivered = now; lastFrame = frame; ++result.delivered; result.keysDelivered += m.key;
    }
    out.messages.clear();
    reverse.arrive(now, [&](const Bytes& d) {
      const size_t n = keys.hostOpen.open(d.data(), d.size(), opened); if (!n) return;
      const auto c = sender.control(opened, n, now);
      if (c.keyFrame) { ++result.keyRequests; forceIdr = true; }
      // As UdpMediaPath: the encoder's next frame answers it. The first LOSS of a session is an
      // IDR that widens the DPB (host::Encoder::recover); later ones are recovery frames.
      if (recovery && c.loss && c.epoch == 1 && c.lastGood) { sender.recovering(now); if (widened) { answerNext = true; ++result.recoveries; } else { widened = true; forceIdr = true; } }
    });
    if (sender.silent(now)) { result.gaveUp = true; result.gaveUpAfterUs = s.outageTo ? now - s.outageFrom : now; sender.stop(); break; }
  }
  // A run that returned to WSS ends in a stall that lasted until then.
  if (result.gaveUp && !(now >= s.idleFromUs && now < s.idleToUs)) result.stallUs = std::max(result.stallUs, now - std::max<uint64_t>(lastDelivered, 1000));
  result.proven = sender.proven();
  result.overheadPct = 100.0 * double(sender.stats().repairBytes) / double(std::max<uint64_t>(sender.stats().videoBytes, 1));
  result.repairs = sender.stats().repairs; result.refused = sender.stats().refused; result.videoBytes = sender.stats().videoBytes + sender.stats().repairBytes;
  return result;
}

// The same links with and without recovery frames. A released Host ignores LOSS, so its
// column is today's behaviour with the new client.
void recoverySimulation() {
  std::vector<Scenario> scenarios;
  for (const uint64_t rtt : {30, 60, 120}) for (const int loss : {1, 3, 5}) { Scenario s; s.name = "RTT " + std::to_string(rtt) + " ms, " + std::to_string(loss) + "%"; s.lossPct = loss; s.rttUs = rtt * 1000; scenarios.push_back(s); }
  { Scenario s; s.name = "RTT 30 ms, bursts"; s.bursts = true; scenarios.push_back(s); }
  { Scenario s; s.name = "RTT 60 ms, bursts"; s.bursts = true; s.rttUs = 60000; scenarios.push_back(s); }
  { Scenario s; s.name = "RTT 30 ms, 10%"; s.lossPct = 10; scenarios.push_back(s); }
  { Scenario s; s.name = "5%, 1.2 MB IDR"; s.lossPct = 5; s.idrBytes = 1200000; scenarios.push_back(s); }
  // Five seeds each, summed: one run has too few give-ups to rank, and heavy loss is chaotic.
  // UDP_SIM_SEEDS asks for more in a one-off measurement.
  char text[8] = {}; size_t length = 0; getenv_s(&length, text, sizeof(text), "UDP_SIM_SEEDS");
  const int seeds = length > 1 ? std::clamp(std::atoi(text), 1, 200) : 5;
  std::printf("SIMULATION, %d seeds summed. LOSS answered with a recovery frame (6x a P-frame, 0.6x the IDR; the first LOSS of a run is an IDR) versus an IDR.\n", seeds);
  std::printf("%-20s %-9s %13s %11s %6s %6s %8s %9s %10s\n", "case", "Host", "frames", "wait ms", "IDRs", "recov", "give-ups", "video MB", "fallbacks");
  for (const auto& s : scenarios) {
    Result sum[2]; int fallbacks[2] = {};
    for (int k = 0; k < 2; ++k) for (int seed = 0; seed < seeds; ++seed) {
      const Result r = simulate(s, 0x5eed + uint64_t(seed) * 7919, k == 1);
      sum[k].delivered += r.delivered; sum[k].generated += r.generated; sum[k].waitUs += r.waitUs; sum[k].idrs += r.idrs; sum[k].recoveries += r.recoveries;
      sum[k].giveUps += r.giveUps; sum[k].videoBytes += r.videoBytes; fallbacks[k] += r.gaveUp;
    }
    for (int k = 0; k < 2; ++k) {
      char frames[32]; std::snprintf(frames, sizeof(frames), "%d/%d", sum[k].delivered, sum[k].generated);
      std::printf("%-20s %-9s %13s %11.1f %6d %6d %8d %9.2f %10d\n", s.name.c_str(), k ? "recovery" : "IDR only", frames, double(sum[k].waitUs) / 1000, sum[k].idrs, sum[k].recoveries, sum[k].giveUps, double(sum[k].videoBytes) / 1e6, fallbacks[k]);
    }
    require(!sum[0].recoveries, "A Host without recovery answered a LOSS");
    // Where loss is common, recovery replaces IDRs and delivers about as large a share of the
    // frames (a fallback ends a run early, so shares, not counts).
    // (The first LOSS of each run is still an IDR, so runs with one give-up change nothing.)
    if (sum[0].giveUps >= 10) require(sum[1].idrs <= sum[0].idrs && double(sum[1].delivered) / sum[1].generated >= double(sum[0].delivered) / sum[0].generated - 0.03, "Recovery frames did not replace IDRs, or delivered a smaller share of the frames");
  }
  ++cases;
}

void simulation() {
  std::vector<Scenario> scenarios;
  for (const uint64_t rtt : {30, 60, 120}) for (const int loss : {0, 1, 3, 5}) {
    Scenario s; s.name = "RTT " + std::to_string(rtt) + " ms, " + std::to_string(loss) + "%"; s.lossPct = loss; s.rttUs = rtt * 1000; scenarios.push_back(s);
  }
  const auto named = [&](const char* name, auto&& change) { Scenario s; s.name = name; change(s); scenarios.push_back(s); };
  named("RTT 60 ms, bursts", [](Scenario& s) { s.bursts = true; s.rttUs = 60000; });
  named("RTT 30 ms, bursts", [](Scenario& s) { s.bursts = true; });
  named("RTT 30 ms, 10%", [](Scenario& s) { s.lossPct = 10; });
  named("5% reorder 2-8 ms", [](Scenario& s) { s.reorderPct = 5; });
  named("5% duplicates", [](Scenario& s) { s.duplicatePct = 5; });
  named("3% + reorder + dup", [](Scenario& s) { s.lossPct = 3; s.reorderPct = 5; s.duplicatePct = 2; });
  named("total outage 5 s", [](Scenario& s) { s.outageFrom = 10000000; s.outageTo = 15000000; });
  named("Host-to-client outage", [](Scenario& s) { s.outageFrom = 10000000; s.outageTo = 15000000; s.oneWay = true; });
  named("3%, 1.2 MB IDR (VBV)", [](Scenario& s) { s.lossPct = 3; s.idrBytes = 1200000; });
  named("5%, 1.2 MB IDR (VBV)", [](Scenario& s) { s.lossPct = 5; s.idrBytes = 1200000; });
  named("RTT 60 ms, 5%, 1.2 MB IDR", [](Scenario& s) { s.lossPct = 5; s.rttUs = 60000; s.idrBytes = 1200000; });
  // 30 s with no media at all. Only a real failure may end it, as in the last case.
  const auto idle = [](Scenario& s) { s.idleFromUs = 2000000; s.idleToUs = 32000000; s.runUs = 34000000; };
  named("idle 30 s", idle);
  named("idle 30 s, 5%", [&](Scenario& s) { idle(s); s.lossPct = 5; });
  named("idle 30 s, RTT 120 ms", [&](Scenario& s) { idle(s); s.rttUs = 120000; });
  named("idle, outage inside it", [&](Scenario& s) { idle(s); s.outageFrom = 10000000; s.outageTo = 15000000; });
  named("idle, one-way outage", [&](Scenario& s) { idle(s); s.outageFrom = 10000000; s.outageTo = 15000000; s.oneWay = true; });
  named("idle, 1.2 s one-way blip", [&](Scenario& s) { idle(s); s.outageFrom = 10000000; s.outageTo = 11200000; s.oneWay = true; });
  named("spoofed source", [](Scenario& s) { s.spoofed = true; });
  std::printf("SIMULATION, fixed seed, not a network measurement. 20 Mbps target, 60 fps, 24-36 KB P-frames, 300 KB IDR unless named, 100 Mbps link, 30 s unless named.\n");
  std::printf("%-24s %-11s %9s %5s %7s %8s %7s %8s %s\n", "case", "frames", "stall ms", "IDRs", "failed", "give-ups", "retx %", "refused", "fell back to WSS");
  for (const auto& s : scenarios) {
    const Result r = simulate(s, 0x5eed);
    char frames[32], gaveUp[48]; std::snprintf(frames, sizeof(frames), "%d/%d", r.delivered, r.generated);
    if (r.gaveUp) std::snprintf(gaveUp, sizeof(gaveUp), "yes, %.2f s after it began", double(r.gaveUpAfterUs) / 1e6); else std::snprintf(gaveUp, sizeof(gaveUp), "no");
    std::printf("%-24s %-11s %9.1f %5d %7d %8d %7.2f %8llu %s\n", s.name.c_str(), frames, double(r.stallUs) / 1000, r.idrs, r.idrs - r.keysDelivered, r.giveUps, r.overheadPct, (unsigned long long)r.refused, gaveUp);
    const std::string& name = s.name;
    // The same seed gives the same run.
    const Result again = simulate(s, 0x5eed);
    require(again.delivered == r.delivered && again.stallUs == r.stallUs && again.repairs == r.repairs, "The simulation is not deterministic");
    if (s.lossPct == 0 && !s.bursts && !s.outageTo && !s.spoofed && s.reorderPct == 0 && s.duplicatePct == 0) require(r.delivered == r.generated && r.idrs == 1 && !r.keyRequests && !r.repairs && !r.gaveUp, "A clean link was not clean");
    if (name == "total outage 5 s" || name == "Host-to-client outage") require(r.gaveUp && r.gaveUpAfterUs >= 900000 && r.gaveUpAfterUs <= 1300000, "An outage did not return to WSS in about 1 s");
    if (name.rfind("idle 30 s", 0) == 0 || name == "idle, 1.2 s one-way blip") require(!r.gaveUp, "An idle screen read as silence");
    if (name == "idle, outage inside it" || name == "idle, one-way outage") require(r.gaveUp && r.gaveUpAfterUs <= 3000000, "A real outage on an idle screen stayed on UDP");
    if (name == "5% reorder 2-8 ms" || name == "5% duplicates" || name == "RTT 30 ms, 1%" || name == "RTT 30 ms, 3%" || name == "RTT 60 ms, 1%") require(!r.gaveUp, "A workable link returned to WSS");
    if (name == "spoofed source") {
      require(!r.proven && !r.mediaDatagrams && !r.generated && !r.delivered && !r.repairs && !r.keyRequests, "A client that receives nothing got media");
      require(r.gaveUp && r.gaveUpAfterUs >= 1000000 && r.gaveUpAfterUs <= 1001200, "A spoofed client was not refused at 1 s");
      require(r.bytesToAddress <= 6 * (tailBytes + headerBytes + tagBytes), "The refused address received more than TAIL");
      std::printf("  spoofed source: %llu media datagrams, %llu bytes sent to the pinned address before refusal\n", (unsigned long long)r.mediaDatagrams, (unsigned long long)r.bytesToAddress);
    } else require(r.proven, "A real client did not prove its address");
  }
  ++cases;
}

void aesCost() {
  SessionKeys keys; derive(Role::host, keys); SessionKeys other; derive(Role::client, other);
  Sealer sealer(keys.send, hostLeg); Opener opener(other.receive);
  uint8_t plain[maxPlaintext] = {}, out[maxDatagram], back[maxPlaintext]; constexpr int rounds = 200000;
  using Clock = std::chrono::steady_clock;
  std::vector<Bytes> sealed; sealed.reserve(rounds);
  for (int n = 0; n < rounds; ++n) { const size_t s = sealer.seal(plain, maxPlaintext, out); sealed.emplace_back(out, out + s); }
  auto start = Clock::now(); size_t total = 0;
  for (int n = 0; n < rounds; ++n) total += sealer.seal(plain, maxPlaintext, out);
  const double sealUs = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / rounds;
  start = Clock::now();
  for (const auto& d : sealed) total += opener.open(d.data(), d.size(), back);
  const double openUs = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / rounds;
  require(total > 0, "Timing fixture");
  std::printf("AES-256-GCM through CNG, 1,167-byte plaintext, THIS PC ONLY: seal %.2f us, open %.2f us per datagram (%d rounds).\n", sealUs, openUs, rounds);
  ++cases;
}
}

int main() {
  try {
    rfc7748(); rfc5869(); nistGcm(); keySchedule(); framingLayout(); tamper(); replay(); retransmissionTakesNewSequence();
    fragments(); inOrderDelivery(); nackTiming(); rttFromTail(); giveUpDeadline(); dropUntilKey(); keyFrameRules(); epochs(); tailHandling();
    feedbackTiming(); audioRules(); pacer(); resendBuffer(); repairBudget(); tailTiming(); silence(); addressProof(); keyFrameMerging(); lossRecovery(); resendMemory(); backoff(); sendQueue(); pathRule();
    maliciousPlaintext(); windowBounds(); fuzz(); simulation(); recoverySimulation(); aesCost();
    std::cout << "udp-media: " << cases << " cases, " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAILED after " << cases << " cases, " << checks << " checks: " << error.what() << "\n";
    return 1;
  }
}
