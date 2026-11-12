#pragma once
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
namespace multitap {
// Actual query lifetime is independent from any UI observer's deadline/close.
// Shared by the real asynchronous executor and its windowless lifecycle tests.
class StreamSizeActualSlot {
 public:
  struct Ticket {uint64_t serial=0;};
  std::optional<Ticket> Begin(){std::lock_guard lock(mutex_);if(active_||serial_>=uint64_t(std::numeric_limits<int64_t>::max()))return {};active_=++serial_;return Ticket{active_};}
  bool ActualReturned(Ticket ticket){std::lock_guard lock(mutex_);if(!ticket.serial||ticket.serial!=active_)return false;active_=0;return true;}
  bool Busy()const{std::lock_guard lock(mutex_);return active_!=0;}
 private:
  mutable std::mutex mutex_;uint64_t serial_=0,active_=0;
};
// Caller holds its endpoint lock when crossing UI/worker threads. No HWND,
// Flutter object or actual-slot release is hidden in observer retirement.
class StreamSizeQueryObserver {
 public:
  struct Stamp {uint64_t serial=0,epoch=0;};
  Stamp Begin(StreamSizeActualSlot::Ticket ticket){if(closed_||!ticket.serial||epoch_==UINT64_MAX)return {};serial_=ticket.serial;return {serial_,++epoch_};}
  bool Current(Stamp stamp)const{return !closed_&&stamp.serial&&stamp.serial==serial_&&stamp.epoch==epoch_;}
  void Retire(){serial_=0;if(epoch_!=UINT64_MAX)++epoch_;}
  void Close(){closed_=true;Retire();}
 private:
  uint64_t serial_=0,epoch_=0;bool closed_=false;
};
}
