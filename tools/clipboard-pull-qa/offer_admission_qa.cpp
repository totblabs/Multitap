// Source-only admission regression: fake selection metadata and owned memory
// snapshots. No OS clipboard, window, input, file source or network is used.
#include "pull.hpp"
#include <iostream>
#include <stdexcept>

namespace clip = multitap::clipboard;
namespace {
unsigned checks = 0;
void require(bool accepted, const char* reason) { if (!accepted) throw std::runtime_error(reason); ++checks; }
template<class F> void expired(F&& operation) {
  bool refused = false;
  try { operation(); } catch (const std::runtime_error& error) { refused = std::string(error.what()) == "clipboard_offer_expired"; }
  require(refused, "An obsolete selection admitted a new paste range");
}
std::shared_ptr<clip::SourceOffer> text(const std::string& id) {
  clip::Snapshot snapshot; snapshot.text = L"old selection";
  return std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::fromContent(id, snapshot, clip::Bounds::scale(), false));
}
}
int main() {
  try {
    clip::OfferRegistry offers;
    DWORD sequence = 41;
    offers.add(text("old"), sequence);
    auto admitted = offers.openCurrent("old", 0, 0, [&] { return sequence; });
    require(admitted->next(3) == std::vector<uint8_t>({'o','l','d'}), "The current selection was not readable");
    // No watcher dispatch or quiet timer has run: the old source still exists.
    sequence = 42;
    require(offers.find("old") != nullptr, "The race was not exercised before retirement");
    // A paste already reading "old" goes on after the newer copy: its next range is admitted (as RDP
    // keeps the clipboard data of a paste locked). An offer no paste has read is refused (the "race" case below).
    auto again = offers.openCurrent("old", 0, 3, [&] { return sequence; });
    require(again->next(10) == std::vector<uint8_t>({' ','s','e','l','e','c','t','i','o','n'}), "A paste under way lost its next range");
    require(admitted->next(10) == std::vector<uint8_t>({' ','s','e','l','e','c','t','i','o','n'}), "A new copy invalidated an already admitted Reader");
    require(admitted->digest().size() == 64, "The admitted range did not retain its content");

    {
      clip::OfferRegistry held; auto now = clip::OfferRegistry::Clock::now();
      held.setClockForTesting([&] { return now; });
      DWORD selected = 7;
      held.add(text("idle"), selected); held.add(text("paste"), selected);
      held.openCurrent("paste", 0, 0, [&] { return selected; });
      require(held.pasting("paste") && !held.pasting("idle"), "A served offer was not marked as pasting");
      selected = 8;  // this side copied something else
      const auto gone = held.dropIdle();
      require(gone == std::vector<std::string>{"idle"} && held.find("paste"), "dropIdle kept the idle offer or dropped the pasting one");
      expired([&] { held.openCurrent("idle", 0, 0, [&] { return selected; }); });
      require(held.openCurrent("paste", 0, 0, [&] { return selected; })->size() > 0, "The pasting offer refused its next range");
      held.add(text("n1"), selected); held.add(text("n2"), selected);
      require(held.size() == clip::maxLiveOffers && held.find("paste") && held.find("n2") && !held.find("n1"), "Trimming dropped the offer a paste is reading");
      now += clip::OfferRegistry::pasteHold - std::chrono::seconds(1);
      held.served("paste");  // a long file is still being read
      now += std::chrono::seconds(30);
      require(held.pasting("paste"), "A paste still reading lost its hold");
      now += clip::OfferRegistry::pasteHold + std::chrono::seconds(1);  // quiet past the hold
      expired([&] { held.openCurrent("paste", 0, 0, [&] { return selected; }); });
      require(held.dropIdle().size() == 2 && held.size() == 0, "A quiet paste kept its offer past the hold");
    }

    offers.add(text("race"), sequence);
    unsigned probes = 0;
    expired([&] { offers.openCurrent("race", 0, 0, [&] { if (++probes == 2) ++sequence; return sequence; }); });
    require(probes == 2, "Admission did not check again after opening its source");

    offers.clear(); offers.add(text("reentrant"), sequence);
    expired([&] { offers.openCurrent("reentrant", 0, 0, [&] { offers.clear(); return sequence; }); });
    require(offers.size() == 0, "Metadata callback could not revoke without a registry lock");

    offers.add(text("legacy"));
    auto legacy = offers.openCurrent("legacy", 0, 0, []() -> DWORD { throw std::runtime_error("Legacy admission read new metadata"); });
    require(legacy->next(3).size() == 3, "The older profile's source lifetime changed");
    offers.add(text("wrap"), DWORD(0xffffffff));
    sequence = 0;
    expired([&] { offers.openCurrent("wrap", 0, 0, [&] { return sequence; }); });
    offers.add(text("zero"), sequence);
    // The bound holds; "legacy" was read (pasting), so the unread "wrap" goes first.
    require(offers.size() == clip::maxLiveOffers && offers.find("legacy") && !offers.find("wrap"), "The existing offer count bound changed");
    auto zero = offers.openCurrent("zero", 0, 0, [&] { return sequence; });
    offers.remove("zero");
    expired([&] { offers.openCurrent("zero", 0, 0, [&] { return sequence; }); });
    require(zero->next(3).size() == 3, "Offer retirement destroyed an admitted memory range");
    offers.clear();
    require(offers.ids().empty(), "Session cleanup retained source admission");
    std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"fakeSelectionMetadata\":true,\"realClipboardReads\":0,\"realClipboardWrites\":0,\"network\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
