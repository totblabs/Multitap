#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "signin_diagnostic_inlet.hpp"
#include <atomic>

namespace host {class NativeDisplayService;}
namespace host::signin::diagnostic {
namespace coordinator_detail {
enum class Start { started,refused,failed };
enum class Poll { waiting,finished,failed };
enum class Exit { none,alive,exited,unknown };
enum class Original { absent,owned,unknown };
// Actual native resource adapter in production; injected own-resource adapter
// only in the focused fixture. No peer chooses this adapter or its predicates.
class Operations {
 public:
  virtual ~Operations()=default;
  void bindCancellation(std::shared_ptr<std::atomic<bool>> value){cancelled_=std::move(value);}
  virtual Start begin(VerifiedOperator&&,const Request&)=0;
  virtual bool engaged()const noexcept=0;
  virtual bool forwardCurrent()const noexcept=0;
  virtual Poll preparation()=0;
  virtual bool preparationSucceeded()const noexcept=0;
  virtual Exit preparationExit()=0;
  virtual void stopPreparation()noexcept=0;
  virtual Original original()=0;
  virtual bool startRestoration()=0;
  // Existing cleanup obligation ONLY. Prove same original/private reservation
  // and every held predecessor exit before pruning exited older wait handles.
  // Never opens a new forward operation, reservation or baseline.
  virtual bool rearmRestoration()=0;
  virtual Poll restoration()=0;
  virtual Exit restorationExit()=0;
  virtual void stopRestoration()noexcept=0;
  virtual bool finishRestoration()=0;
  virtual bool releaseReservation()=0;
  virtual bool heartbeat()noexcept=0;
  virtual uint64_t now()const noexcept=0;
  virtual void pause(uint32_t milliseconds)noexcept=0;
 protected:
  bool forwardCancelled()const noexcept{return !cancelled_||cancelled_->load(std::memory_order_seq_cst);}
 private:
  std::shared_ptr<std::atomic<bool>> cancelled_;
};
}
struct CoordinatorTestAccess;
// NativeDisplayService and stopEvent outlive this object. Root joins inlet
// callbacks before destruction. No Bridge pointer or cross-thread lock guard
// is retained; Root's per-call lexical locks surround prepare/tickCleanup.
class Coordinator {
 public:
  using FinalAdmission=std::function<bool()>;
  Coordinator(NativeDisplayService&,HANDLE stopEvent);
  ~Coordinator();
  Coordinator(const Coordinator&)=delete;Coordinator& operator=(const Coordinator&)=delete;
  // Shared SC busy flag is set BEFORE this mandatory synchronous-only check.
  // finalCheck is invoked once and never retained in native operation state.
  Result prepare(VerifiedOperator&&,const Request&,const FinalAdmission&);
  Result tickCleanup();
  bool busy()const noexcept;
  std::shared_ptr<std::atomic<bool>> busyFlag()const;
  void cancelForward()noexcept;
  // Returns pending without releasing any unexited child/original/reservation.
  Result shutdown();
 private:
  friend struct CoordinatorTestAccess;
  Coordinator(std::unique_ptr<coordinator_detail::Operations>,HANDLE stopEvent);
  struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace host::signin::diagnostic
