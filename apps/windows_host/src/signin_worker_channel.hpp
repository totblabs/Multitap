#pragma once
#include "signin_worker_native.hpp"
#include "json_object.hpp"
#include <atomic>
#include <mutex>

namespace host::signin::channel {
// This protocol is private to the service's exact PID-authenticated child pipe.
// It is never accepted on the broker connection, settings RPC or a named socket.
struct Init {
  // Context.unknownAccountAllowed is explicit on the authenticated private
  // pipe for empty-account noUser admission. No public/broker parser uses it.
  Context context;
  Binding binding;
  native::OwnedTarget target;
  uint64_t serviceQuery = 0;
};

std::string initPacket(const Init&);
Init parseInit(const Json&);
bool sameBinding(const Binding&, const Binding&);
bool sameTarget(const native::OwnedTarget&, const native::OwnedTarget&);

// Adopt only after the pipe's actual server PID and the delegated query/wait
// handle's process identity have both been checked. Invalid numeric handles
// are never closed. Permit::current performs the full SCM/image/desktop check.
native::Handle acceptInitService(const Init&, HANDLE pipe, uint32_t servicePid);

// Service-side dispatcher. 'authority' must be backed by the existing service
// controller and the retained original baseline, not by the request's fields.
// A missing authority cannot grant a lease; there is no permissive callback.
std::string answerLease(const Json&, const Binding&,
                        std::span<const native::OwnedTarget>,
                        const native::LeaseAuthority&);

std::string answerReleaseLease(const Json&, const Binding&,
                        std::span<const native::OwnedTarget>,
                        const native::LeaseAuthority&);

class WorkerLease final : public native::LeaseAuthority {
 public:
  using Defer = std::function<bool(const Json&)>;
  WorkerLease(HANDLE pipe, const Binding&, const native::OwnedTarget&,
              const std::atomic<bool>& stopped, std::mutex& writing,
              Defer defer);
  bool current(const Binding&, std::span<const native::OwnedTarget>) const override;
  bool currentForRelease(const Binding&, std::span<const native::OwnedTarget>) const override;
 private:
  bool query(const Binding&, std::span<const native::OwnedTarget>, bool releaseOnly) const;
  HANDLE pipe_;
  Binding binding_;
  native::OwnedTarget target_;
  const std::atomic<bool>& stopped_;
  std::mutex& writing_;
  Defer defer_;
  mutable std::mutex querying_;
  mutable uint64_t serial_ = 0;
};
}
