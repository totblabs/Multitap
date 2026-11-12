#ifndef MULTITAP_CONSOLE_INPUT_OWNERSHIP_H_
#define MULTITAP_CONSOLE_INPUT_OWNERSHIP_H_
#include <cstdint>
#include <limits>
#include <string>

namespace multitap {
struct ConsoleInputStamp {
  std::string owner;
  int64_t generation=0, lease=0;
};
// Window/UI-thread policy only. No HWND, hook, OS input, socket or side effects.
class ConsoleInputOwnership {
 public:
  static bool ValidOwner(const std::string& owner) {
    if(owner.size()!=32)return false;
    for(const auto c:owner)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    return true;
  }
  bool CanAcquire(const std::string& owner,int64_t generation,bool other_active)const {
    return !closed_&&!other_active&&ValidOwner(owner)&&generation>0&&generation<=INT32_MAX&&
      serial_<std::numeric_limits<int64_t>::max()&&
      (view_owner_.empty()||(view_owner_==owner&&generation>view_generation_))&&
      (!valid()||(current_.owner==owner&&generation>current_.generation));
  }
  ConsoleInputStamp Acquire(const std::string& owner,int64_t generation,bool other_active) {
    if(!CanAcquire(owner,generation,other_active))return {};
    view_owner_=owner;view_generation_=generation;
    current_={owner,generation,++serial_};captured_=false;return current_;
  }
  bool Current(const ConsoleInputStamp& stamp)const {
    return !closed_&&valid()&&stamp.owner==current_.owner&&
      stamp.generation==current_.generation&&stamp.lease==current_.lease;
  }
  bool CanCapture(const ConsoleInputStamp& stamp,bool enabled,bool foreground,bool other_active)const {
    return Current(stamp)&&(!enabled||(foreground&&!other_active));
  }
  bool Captured(const ConsoleInputStamp& stamp,bool captured) {
    if(!Current(stamp))return false;captured_=captured;return true;
  }
  void ResetCapture(){captured_=false;}
  bool Release(const ConsoleInputStamp& stamp){if(!Current(stamp))return false;current_={};captured_=false;return true;}
  bool BlocksOtherStart()const{return !view_owner_.empty();}
  bool CanRetireView(const std::string& owner,int64_t generation)const {
    return !closed_&&owner==view_owner_&&generation>=view_generation_;
  }
  bool RetireView(const std::string& owner,int64_t generation) {
    if(!CanRetireView(owner,generation))return false;
    current_={};captured_=false;view_owner_.clear();view_generation_=0;return true;
  }
  void Close(){current_={};captured_=false;view_owner_.clear();closed_=true;}
  bool valid()const{return current_.lease>0;}
  bool captured()const{return captured_;}
  ConsoleInputStamp stamp()const{return current_;}
 private:
  ConsoleInputStamp current_;
  std::string view_owner_;
  int64_t view_generation_=0;
  int64_t serial_=0;
  bool captured_=false,closed_=false;
};
}
#endif
