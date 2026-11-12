#pragma once
#include "udp_media.hpp"
#include <windows.h>
#include <bcrypt.h>

// UDP media protection through CNG only (link bcrypt), Phase 3 contract. An active
// Broker can substitute the public keys: this is not end-to-end. Any failure here
// means the session stays on WSS. Custom framing: it needs independent review.
namespace multitap::udp {
constexpr uint64_t sequenceLimit = 1ull << 48;
constexpr size_t legIdBytes = 8, keyBytes = 32, saltBytes = 4, nonceBytes = 12, replaySpan = 2048;
constexpr size_t minimumDatagram = headerBytes + 1 + tagBytes;  // above the 25-byte probe, so the two never mix
constexpr char keyLabel[] = "multitap-udp-media-v1";
using PublicKey = std::array<uint8_t, keyBytes>;  // RFC 7748 byte order
enum class Role { client, host };

// Key material: never copied, wiped when it goes.
template <size_t N> class Secret {
 public:
  Secret() = default;
  Secret(const Secret&) = delete;
  Secret& operator=(const Secret&) = delete;
  ~Secret() { wipe(); }
  uint8_t* data() { return bytes_.data(); }
  const uint8_t* data() const { return bytes_.data(); }
  void wipe() { SecureZeroMemory(bytes_.data(), N); }
 private:
  std::array<uint8_t, N> bytes_{};
};

inline bool sha256(const uint8_t* data, size_t size, uint8_t* out) {
  return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data), ULONG(size), out, 32) >= 0;
}
inline bool hmacSha256(const uint8_t* key, size_t keySize, const uint8_t* data, size_t size, uint8_t* out) {
  return BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, const_cast<PUCHAR>(key), ULONG(keySize), const_cast<PUCHAR>(data), ULONG(size), out, 32) >= 0;
}
// RFC 5869.
inline bool hkdfSha256(const uint8_t* salt, size_t saltSize, const uint8_t* ikm, size_t ikmSize, const uint8_t* info, size_t infoSize, uint8_t* out, size_t outSize) {
  if (!outSize || outSize > 255 * 32) return false;
  Secret<32> prk, block;
  if (!hmacSha256(salt, saltSize, ikm, ikmSize, prk.data())) return false;
  std::vector<uint8_t> input(32 + infoSize + 1);
  bool ok = true; size_t done = 0;
  for (uint8_t counter = 1; ok && done < outSize; ++counter) {
    size_t at = 0;
    if (counter > 1) { std::memcpy(input.data(), block.data(), 32); at = 32; }
    if (infoSize) std::memcpy(input.data() + at, info, infoSize);
    at += infoSize; input[at++] = counter;
    ok = hmacSha256(prk.data(), 32, input.data(), at, block.data());
    const size_t take = std::min<size_t>(32, outSize - done);
    if (ok) std::memcpy(out + done, block.data(), take);
    done += take;
  }
  SecureZeroMemory(input.data(), input.size());
  if (!ok) SecureZeroMemory(out, outSize);
  return ok;
}

// A fresh key pair per grant. An all-zero shared secret fails.
class X25519 {
 public:
  X25519() {
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_ECDH_ALGORITHM, nullptr, 0) < 0 ||
        BCryptSetProperty(algorithm_, BCRYPT_ECC_CURVE_NAME, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_ECC_CURVE_25519)), sizeof(BCRYPT_ECC_CURVE_25519), 0) < 0 ||
        BCryptGenerateKeyPair(algorithm_, &key, 255, 0) < 0) return;
    key_ = key;
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 2 * keyBytes> blob{}; ULONG size = 0;
    // CNG's public blob already holds the RFC 7748 bytes (udp_media_test.cpp).
    valid_ = BCryptFinalizeKeyPair(key_, 0) >= 0 && BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), ULONG(blob.size()), &size, 0) >= 0 && size == blob.size();
    if (valid_) std::memcpy(public_.data(), blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), keyBytes);
  }
  X25519(const X25519&) = delete;
  X25519& operator=(const X25519&) = delete;
  ~X25519() { if (key_) BCryptDestroyKey(key_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
  bool valid() const { return valid_; }
  const PublicKey& publicKey() const { return public_; }

  bool agree(const PublicKey& peer, Secret<keyBytes>& shared) const {
    if (!valid_) return false;
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 2 * keyBytes> blob{};
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC; header->cbKey = keyBytes;
    std::memcpy(blob.data() + sizeof(*header), peer.data(), keyBytes);
    blob[sizeof(*header) + keyBytes - 1] &= 0x7f;  // RFC 7748 masks it; CNG refuses it
    BCRYPT_KEY_HANDLE other = nullptr; BCRYPT_SECRET_HANDLE secret = nullptr; ULONG size = 0;
    Secret<keyBytes> raw;
    bool ok = BCryptImportKeyPair(algorithm_, nullptr, BCRYPT_ECCPUBLIC_BLOB, &other, blob.data(), ULONG(blob.size()), 0) >= 0 &&
              BCryptSecretAgreement(key_, other, &secret, 0) >= 0 &&
              BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, raw.data(), keyBytes, &size, 0) >= 0 && size == keyBytes;
    if (secret) BCryptDestroySecret(secret);
    if (other) BCryptDestroyKey(other);
    if (!ok) return false;
    // CNG's raw secret is the reverse of RFC 7748's bytes, the only order that
    // would meet a peer that does not use CNG.
    uint8_t any = 0;
    for (size_t n = 0; n < keyBytes; ++n) { shared.data()[n] = raw.data()[keyBytes - 1 - n]; any |= raw.data()[n]; }
    if (!any) shared.wipe();
    return any != 0;
  }

 private:
  friend struct TestAccess;
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_KEY_HANDLE key_ = nullptr;
  PublicKey public_{};
  bool valid_ = false;
};

// One direction's key and nonce salt. A sealer or opener consumes it.
struct DirectionKey { Secret<keyBytes> key; Secret<saltBytes> salt; bool ready = false; };
struct SessionKeys { DirectionKey send, receive; };

// The contract's HKDF. Both sides pass the public keys in the same order.
inline bool deriveKeys(const Secret<keyBytes>& shared, const PublicKey& client, const PublicKey& host, const uint8_t* sessionId, size_t sessionIdSize, Role role, SessionKeys& out) {
  constexpr size_t labelBytes = sizeof(keyLabel) - 1;
  std::array<uint8_t, labelBytes + 2 * keyBytes> info;
  std::memcpy(info.data(), keyLabel, labelBytes);
  std::memcpy(info.data() + labelBytes, client.data(), keyBytes);
  std::memcpy(info.data() + labelBytes + keyBytes, host.data(), keyBytes);
  uint8_t salt[32]; Secret<2 * keyBytes + 2 * saltBytes> okm;
  if (!sha256(sessionId, sessionIdSize, salt) || !hkdfSha256(salt, sizeof(salt), shared.data(), keyBytes, info.data(), info.size(), okm.data(), 2 * keyBytes + 2 * saltBytes)) return false;
  DirectionKey& clientToHost = role == Role::client ? out.send : out.receive;
  DirectionKey& hostToClient = role == Role::client ? out.receive : out.send;
  std::memcpy(clientToHost.key.data(), okm.data(), keyBytes);
  std::memcpy(hostToClient.key.data(), okm.data() + keyBytes, keyBytes);
  std::memcpy(clientToHost.salt.data(), okm.data() + 2 * keyBytes, saltBytes);
  std::memcpy(hostToClient.salt.data(), okm.data() + 2 * keyBytes + saltBytes, saltBytes);
  clientToHost.ready = hostToClient.ready = true;
  return true;
}

class Aes256Gcm {
 public:
  explicit Aes256Gcm(const uint8_t* key) {
    if (BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &key_, nullptr, 0, const_cast<PUCHAR>(key), keyBytes, 0) < 0) key_ = nullptr;
  }
  Aes256Gcm(const Aes256Gcm&) = delete;
  Aes256Gcm& operator=(const Aes256Gcm&) = delete;
  ~Aes256Gcm() { if (key_) BCryptDestroyKey(key_); }  // CNG wipes its key object
  bool valid() const { return key_ != nullptr; }
  bool seal(const uint8_t* nonce, const uint8_t* aad, size_t aadSize, const uint8_t* plain, size_t size, uint8_t* cipher, uint8_t* tag) const {
    auto mode = info(nonce, aad, aadSize, tag); ULONG done = 0;
    return key_ && BCryptEncrypt(key_, const_cast<PUCHAR>(plain), ULONG(size), &mode, nullptr, 0, cipher, ULONG(size), &done, 0) >= 0 && done == size;
  }
  bool open(const uint8_t* nonce, const uint8_t* aad, size_t aadSize, const uint8_t* cipher, size_t size, const uint8_t* tag, uint8_t* plain) const {
    auto mode = info(nonce, aad, aadSize, const_cast<uint8_t*>(tag)); ULONG done = 0;
    return key_ && BCryptDecrypt(key_, const_cast<PUCHAR>(cipher), ULONG(size), &mode, nullptr, 0, plain, ULONG(size), &done, 0) >= 0 && done == size;
  }
 private:
  static BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info(const uint8_t* nonce, const uint8_t* aad, size_t aadSize, uint8_t* tag) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO mode; BCRYPT_INIT_AUTH_MODE_INFO(mode);
    mode.pbNonce = const_cast<PUCHAR>(nonce); mode.cbNonce = nonceBytes;
    mode.pbAuthData = const_cast<PUCHAR>(aad); mode.cbAuthData = ULONG(aadSize);
    mode.pbTag = tag; mode.cbTag = tagBytes;
    return mode;
  }
  BCRYPT_KEY_HANDLE key_ = nullptr;
};

// For the Sender's challenge. On failure 0, which never proves an address.
inline uint64_t randomChallenge() {
  uint64_t value = 0;
  return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0 ? value : 0;
}

// One direction's sender. The sequence is private and only moves forward, and no
// sealed datagram is ever kept, so a retransmission cannot reuse one.
class Sealer {
 public:
  Sealer(DirectionKey& key, const std::array<uint8_t, legIdBytes>& ownLegId) : aes_(key.key.data()), ready_(key.ready && aes_.valid()) {
    std::memcpy(salt_.data(), key.salt.data(), saltBytes); std::memcpy(legId_.data(), ownLegId.data(), legIdBytes);
    key.key.wipe(); key.salt.wipe(); key.ready = false;
  }
  // Writes the datagram and returns its size, or 0 for nothing sent. At the
  // sequence limit it refuses for good, and the Host returns to WSS.
  size_t seal(const uint8_t* plain, size_t size, uint8_t* out) {
    if (!ready_ || !size || size > maxPlaintext || exhausted()) return 0;
    out[0] = 3; std::memcpy(out + 1, legId_.data(), legIdBytes); put64(out + 9, next_);
    uint8_t nonce[nonceBytes]; std::memcpy(nonce, salt_.data(), saltBytes); std::memcpy(nonce + saltBytes, out + 9, 8);
    if (!aes_.seal(nonce, out, headerBytes, plain, size, out + headerBytes, out + headerBytes + size)) return 0;
    ++next_;
    return headerBytes + size + tagBytes;
  }
  bool exhausted() const { return next_ >= sequenceLimit; }
 private:
  friend struct TestAccess;
  Aes256Gcm aes_;
  Secret<saltBytes> salt_;
  std::array<uint8_t, legIdBytes> legId_{};
  bool ready_;
  uint64_t next_ = 1;
};

// The last 2,048 sequences: the newest seen and those just before it.
class ReplayWindow {
 public:
  bool fresh(uint64_t sequence) const {
    if (sequence > highest_) return true;
    return highest_ - sequence < replaySpan && !(bits_[word(sequence)] & mask(sequence));
  }
  void accept(uint64_t sequence) {
    if (sequence > highest_) {
      if (sequence - highest_ >= replaySpan) bits_.fill(0);
      else for (uint64_t n = highest_ + 1; n <= sequence; ++n) bits_[word(n)] &= ~mask(n);
      highest_ = sequence;
    }
    bits_[word(sequence)] |= mask(sequence);
  }
 private:
  static size_t word(uint64_t sequence) { return size_t(sequence / 64 % (replaySpan / 64)); }
  static uint64_t mask(uint64_t sequence) { return 1ull << (sequence % 64); }
  uint64_t highest_ = 0;
  std::array<uint64_t, replaySpan / 64> bits_{};
};

// One direction's receiver. Anything that fails is dropped silently.
class Opener {
 public:
  explicit Opener(DirectionKey& key) : aes_(key.key.data()), ready_(key.ready && aes_.valid()) {
    std::memcpy(salt_.data(), key.salt.data(), saltBytes);
    key.key.wipe(); key.salt.wipe(); key.ready = false;
  }
  // Writes the plaintext (maxPlaintext bytes at most) and returns its size, or 0.
  // The replay window moves only after authentication, so a forgery cannot move it.
  size_t open(const uint8_t* datagram, size_t size, uint8_t* plain) {
    if (!ready_ || size < minimumDatagram || size > maxDatagram || datagram[0] != 3) return 0;
    const uint64_t sequence = get64(datagram + 9);
    if (!sequence || sequence >= sequenceLimit || !replay_.fresh(sequence)) return 0;
    uint8_t nonce[nonceBytes]; std::memcpy(nonce, salt_.data(), saltBytes); std::memcpy(nonce + saltBytes, datagram + 9, 8);
    const size_t plainSize = size - headerBytes - tagBytes;
    if (!aes_.open(nonce, datagram, headerBytes, datagram + headerBytes, plainSize, datagram + headerBytes + plainSize, plain)) { SecureZeroMemory(plain, plainSize); return 0; }
    replay_.accept(sequence);
    return plainSize;
  }
 private:
  Aes256Gcm aes_;
  Secret<saltBytes> salt_;
  bool ready_;
  ReplayWindow replay_;
};
}
