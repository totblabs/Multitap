// Actual production epoch state, memory-only: no Windows headers or API.
#define MULTITAP_THUMBNAIL_PRIVACY_TEST
#include "../../src/thumbnail_privacy.hpp"
#include <iostream>
#include <stdexcept>

namespace host {
struct ThumbnailPrivacyEpochTestAccess {
  static void counters(ThumbnailPrivacyEpoch& value, uint64_t revision, uint64_t observation) {
    std::lock_guard lock(value.mutex_);
    value.revision_ = revision; value.observation_ = observation; value.denied_ = false;
  }
};
}

namespace {
int checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
std::string epoch(unsigned value) {
  std::string result(32, '0');
  const char* hex = "0123456789abcdef";
  for (unsigned i = 0; i < 8; ++i) { result[31 - i] = hex[value & 15]; value >>= 4; }
  return result;
}
std::string seed(unsigned value) { return epoch(value).substr(16) + "0123456789abcdef"; }
std::string wire(unsigned owner, unsigned revision) { return seed(owner).substr(0, 16) + epoch(revision).substr(16); }
void run() {
  unsigned next = 0;
  host::ThumbnailPrivacyEpoch state([&] { return seed(++next); });
  const auto original = state.begin();
  const auto first = state.snapshot(original, true);
  require(first.allowed && first.epoch == wire(1, 1), "Fresh normal-console observation was lost");
  require(!state.snapshot(original, false).allowed, "Unknown/locked observation allowed a thumbnail");
  state.rotate(); // a lock notification, even with no active stream
  const auto locked = state.snapshot(original, true);
  require(locked.epoch == wire(1, 1) && !locked.allowed, "Held pre-lock observation denied or authorized a successor epoch");
  require(!state.snapshot(state.begin(), false).allowed, "Lock epoch did not stay denied");
  state.rotate(); // unlock; same boolean endpoint, different history
  const auto unlocked = state.snapshot(state.begin(), true);
  require(unlocked.allowed && unlocked.epoch == wire(1, 3), "Unlock did not carry a fresh epoch");
  require(unlocked.epoch != first.epoch, "Lock-unlock history collapsed to the old source");
  require(next == 1, "Ordinary WTS events unnecessarily queried entropy");
  host::ThumbnailPrivacyEpoch restarted([&] { return seed(++next); });
  require(restarted.snapshot(restarted.begin(), true).epoch != unlocked.epoch, "New service owner reused its predecessor epoch");

  host::ThumbnailPrivacyEpoch recovery([] { return seed(9); });
  const auto heldTrue = recovery.begin();
  const auto heldAfterDeny = recovery.begin();
  const auto uncertainFirst = recovery.snapshot(recovery.begin(), false);
  require(!uncertainFirst.allowed && uncertainFirst.epoch == wire(9, 1), "Fresh uncertainty did not deny its own epoch");
  require(!recovery.snapshot(heldTrue, true).allowed, "True observation begun before denial revived that epoch");
  const auto recovered = recovery.snapshot(recovery.begin(), true);
  require(recovered.allowed && recovered.epoch == wire(9, 2), "Fresh normal observation failed to recover into a new epoch");
  const auto lateFalse = recovery.snapshot(heldAfterDeny, false);
  require(!lateFalse.allowed && lateFalse.epoch == wire(9, 1), "Late old false burned the recovered epoch");
  const auto stillRecovered = recovery.snapshot(recovery.begin(), true);
  require(stillRecovered.allowed && stillRecovered.epoch == recovered.epoch, "Recovered epoch churned or stayed denied after an old completion");
  recovery.snapshot(recovery.begin(), false);
  const auto recoveredAgain = recovery.snapshot(recovery.begin(), true);
  require(recoveredAgain.allowed && recoveredAgain.epoch == wire(9, 3), "Repeated observation outage required a WTS event to recover");

  host::ThumbnailPrivacyEpoch exhausted([] { return seed(10); });
  host::ThumbnailPrivacyEpochTestAccess::counters(exhausted, std::numeric_limits<uint64_t>::max() - 1, 1);
  exhausted.rotate();
  const auto last = exhausted.snapshot(exhausted.begin(), true);
  require(last.allowed && last.epoch == seed(10).substr(0,16) + "ffffffffffffffff", "Last representable epoch was not monotonic");
  exhausted.rotate();
  require(!exhausted.snapshot(exhausted.begin(), true).allowed, "Exhausted event counter wrapped into permission");
  host::ThumbnailPrivacyEpoch observationExhausted([] { return seed(11); });
  host::ThumbnailPrivacyEpochTestAccess::counters(observationExhausted, 1, std::numeric_limits<uint64_t>::max());
  require(!observationExhausted.snapshot(observationExhausted.begin(), false).allowed, "Observation overflow lost denial");
  require(!observationExhausted.snapshot(observationExhausted.begin(), true).allowed, "Observation overflow revived permission");

  bool fail = true;
  unsigned generated = 20;
  host::ThumbnailPrivacyEpoch uncertain([&]() -> std::string {
    if (fail) throw std::runtime_error("fixed synthetic entropy failure");
    return seed(++generated);
  });
  require(uncertain.snapshot(uncertain.begin(), true).epoch.empty(), "Failed startup entropy manufactured an owner");
  uncertain.rotate();
  const auto denied = uncertain.snapshot(uncertain.begin(), true);
  require(denied.epoch.empty() && !denied.allowed, "Entropy failure preserved permission");
  fail = false; uncertain.rotate();
  require(uncertain.snapshot(uncertain.begin(), true).allowed, "Fresh epoch could not recover after entropy failure");
  require(uncertain.snapshot(uncertain.begin(), true).epoch == wire(21, 3), "Entropy retry reset the event counter");
  host::ThumbnailPrivacyEpoch unavailable([] { return std::string("invalid"); });
  require(!unavailable.snapshot(unavailable.begin(), true).allowed, "Malformed initial epoch granted permission");
  require(host::thumbnailPrivacyCapabilityFields(unavailable.snapshot(unavailable.begin(), true)).empty(), "Unknown initial epoch was advertised");

  host::ThumbnailPrivacyEpoch* reentrant = nullptr;
  bool nest = false;
  bool initialize = false;
  unsigned count = 40;
  host::ThumbnailPrivacyEpoch nested([&] {
    if (!initialize) return std::string();
    const auto candidate = seed(++count);
    if (nest) { nest = false; reentrant->rotate(); }
    return candidate;
  });
  reentrant = &nested; initialize = true; nest = true; nested.rotate();
  const auto latest = nested.snapshot(nested.begin(), true);
  require(latest.allowed && latest.epoch == wire(42, 3), "Late older rotation overwrote the newer source");
  require(host::thumbnailPrivacyCapabilityFields({epoch(7), false}) ==
    ",\"thumbnailPrivacy\":{\"protocol\":\"thumbnail-privacy-v1\",\"epoch\":\"" + epoch(7) + "\",\"allowed\":false}", "Readonly wire shape changed");
  for (const auto& invalid : {std::string(), std::string(31, 'a'), std::string(33, 'a'), std::string(32, 'A'), std::string(32, 'g')}) {
    require(!host::ThumbnailPrivacyEpoch::valid(invalid), "Invalid epoch accepted");
  }
}
}
int main() {
  try { run(); std::cout << checks << " checks, 0 failures\n"; return 0; }
  catch (const std::exception& failure) { std::cerr << "FAIL: " << failure.what() << '\n'; return 1; }
}
