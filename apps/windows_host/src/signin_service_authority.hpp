#pragma once
#include "signin_worker_channel.hpp"
#include "native_display_service.hpp"
#include "display_v2.hpp"

namespace host::signin {
// Borrow authority only AFTER display preparation has produced the original
// protected V2 baseline. This is not initial sign-in eligibility, and it never
// captures a replacement baseline or creates a driver/controller. Initial
// locked-console preparation remains a separate integration step.
class PreparedServiceAuthority final : public native::LeaseAuthority {
 public:
  using Assigned = std::function<bool(const Binding&)>;
  PreparedServiceAuthority(NativeDisplayService& display, std::string stream,
      Binding binding, native::OwnedTarget target, HANDLE exactOwnedChild,
      DisplayV2Store protectedStore, Assigned assigned)
    : display_(display), stream_(std::move(stream)), binding_(std::move(binding)),
      target_(target), child_(exactOwnedChild), store_(std::move(protectedStore)),
      assigned_(std::move(assigned)) {
    if (!assigned_ || !store_.load || !child_ || child_ == INVALID_HANDLE_VALUE ||
        !valid(binding_.service) || !valid(binding_.worker) || binding_.worker==binding_.service || !valid(binding_.lease) ||
        !nonzero(binding_.stream) || !nonzero(binding_.baseline) || !nonzero(binding_.nonce) ||
        binding_.role != Role::signin || !binding_.consoleSession || binding_.consoleSession==UINT32_MAX || !binding_.generation || !binding_.mediaEpoch ||
        target_.mask != 1 || stream_ != streamText(binding_.stream)) invalid();
    baseline_ = store_.load();
    if (!baseline_ || baseline_->size() > 65536 || digestText(binding_.baseline) != nativeBaselineDigest(*baseline_)) invalid();
    // Parse once before sealing the exact bytes. A missing/corrupt topology
    // never becomes a valid baseline merely because somebody hashed it.
    (void)parseDisplayV2State(*baseline_);
    if (!current(binding_, {&target_, 1})) invalid();
  }
  bool current(const Binding& value, std::span<const native::OwnedTarget> targets) const override {
    try {
      if (!channel::sameBinding(value, binding_) || targets.size() != 1 || targets.front() != target_ ||
          !assigned_ || !assigned_(binding_) || !baseline_ || store_.load() != baseline_ ||
          native::processIdentity(GetCurrentProcess()) != binding_.service ||
          native::processIdentity(child_) != binding_.worker || WaitForSingleObject(child_, 0) != WAIT_TIMEOUT) return false;
      const auto lease = display_.proveOwnedStreamLease(stream_, binding_.lease.instance, binding_.lease.generation);
      if (!lease || lease->targets.size() != 1 || lease->service.pid != binding_.service.pid || lease->service.created != binding_.service.created) return false;
      const auto owned = std::find_if(lease->targets.begin(), lease->targets.end(), [](const auto& target) { return target.id == "0"; });
      if (owned == lease->targets.end() || std::count_if(lease->targets.begin(), lease->targets.end(), [](const auto& target) { return target.id == "0"; }) != 1 ||
          owned->adapter.LowPart != target_.adapterLow || owned->adapter.HighPart != target_.adapterHigh ||
          owned->target != target_.targetId || owned->width != target_.width || owned->height != target_.height) return false;
      // Recheck the assignment and original bytes after the actual SDK query.
      // CCD source/placement and the active desktop are independently checked
      // by CaptureDxgi/Permit in the exact authenticated child.
      return assigned_(binding_) && store_.load() == baseline_ &&
        native::processIdentity(child_) == binding_.worker && WaitForSingleObject(child_, 0) == WAIT_TIMEOUT;
    } catch (...) { return false; }
  }
  static std::string streamText(const StreamId& stream) {
    constexpr char hex[] = "0123456789abcdef"; std::string out; out.reserve(36);
    for (size_t i = 0; i < stream.size(); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
      out += hex[stream[i] >> 4]; out += hex[stream[i] & 15];
    }
    return out;
  }
 private:
  [[noreturn]] static void invalid() { throw std::runtime_error("Prepared sign-in service authority is unavailable"); }
  static std::string digestText(const Nonce& digest) {
    constexpr char hex[] = "0123456789abcdef"; std::string out; out.reserve(64);
    for (const auto byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
  }
  NativeDisplayService& display_;
  const std::string stream_;
  const Binding binding_;
  const native::OwnedTarget target_;
  HANDLE child_;
  DisplayV2Store store_;
  Assigned assigned_;
  std::optional<std::string> baseline_;
};
}
