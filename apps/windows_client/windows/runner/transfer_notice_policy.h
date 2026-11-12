#pragma once
// Plain C++ policy/codec extracted from the existing native notice. No HWND,
// OLE, foreground, desktop, graphics, or other native API is called here.
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <limits>
#include <utility>
#include <functional>
#include <vector>
#include <algorithm>
namespace multitap {
struct TransferNoticeContent {
  enum class Kind { running, done, failed, cancelled };
  Kind kind = Kind::running;
  std::wstring title, name, detail, message;
  // The line under the title ("다운로드 · 1,702개 중 120개"), the right side of the line under the bar (the time left),
  // and a failure's next step.
  std::wstring subtitle, trailing, hint;
  // 자세히: label and full value rows.
  std::vector<std::pair<std::wstring, std::wstring>> details;
  int percent = -1;
  bool dark = true, cancellable = true, cancelling = false;
  // This PC → VM (the icon points up); `subtitle` starts with the destination folder's name; a finished card offers 폴더 열기.
  bool sending = false, folder = false, openable = false;
  int generation = 0;
};
inline bool WantsBackgroundNotice(bool have,bool dismissed,bool owner_foreground) {
  return have && !dismissed && !owner_foreground;
}
inline int DecodeNoticePercent(std::optional<int64_t> value) {
  return value ? int(*value) : -1;
}
template<class Owner>
void ApplyNoticeUpdate(Owner& current_owner,int& generation,TransferNoticeContent& current,
    bool& have,bool& dismissed,bool& anchored,bool& moved,Owner owner,const TransferNoticeContent& content) {
  if (!have || content.generation != generation || owner != current_owner) {
    dismissed = false;
    anchored = false;
    moved = false;
  } else if (content.kind != current.kind && !moved) {
    anchored = false;
  }
  generation = content.generation;
  current_owner = owner;
  current = content;
  have = true;
}
inline constexpr const char* kNoticePresentationProtocol="presentation-lease-v1";
inline bool NoticeOpaque(std::string_view text) {
  if(text.size()!=32)return false;
  for(char ch:text)if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')))return false;
  return true;
}
struct NoticeIdentity {
  std::uintptr_t window=0;
  std::string owner,lease;
  int generation=-1;
  bool operator==(const NoticeIdentity& other) const {
    return window==other.window&&owner==other.owner&&lease==other.lease&&generation==other.generation;
  }
};
enum class NoticePhase { inlineCard, preparing, nativeCard, fallback, hiding };
inline const char* NoticePhaseName(NoticePhase phase) {
  return phase==NoticePhase::preparing?"preparing":phase==NoticePhase::nativeCard||phase==NoticePhase::hiding?"native":"inline";
}
struct NoticeStamp {
  NoticeIdentity identity;
  int64_t revision=0;
  bool visible=false;
  NoticePhase phase=NoticePhase::inlineCard;
  bool requiresFrame=true;
};
struct NoticeReply {bool accepted=false;NoticeStamp state;};
enum class NoticeAction { none, prepare, show, hide, presentation };
struct NoticeDecision {NoticeAction action=NoticeAction::none;NoticeStamp state;};

enum class NoticeSurfaceProof { hidden, visible, unknown };
struct NoticeSurfaceStamp {
  int64_t revision=1;
  bool inlineAllowed=true;
  bool blockedByNative=false;
};
// One observer per actual Flutter HWND: launcher + at most eight live session
// windows (flutter_window.cpp session_window_limit). Tokens prevent stale
// Detach from erasing a later registration after a native handle is reused.
class NoticeSurfaceObservers {
 public:
  static constexpr std::size_t maximum=9;
  using Callback=std::function<void(const NoticeSurfaceStamp&)>;
  uint64_t Observe(std::uintptr_t window,Callback callback) {
    if(!window||!callback||entries_.size()>=maximum||serial_==std::numeric_limits<uint64_t>::max())return 0;
    for(const auto& entry:entries_)if(entry.window==window)return 0;
    entries_.push_back({window,++serial_,std::move(callback)});return serial_;
  }
  bool Owns(std::uintptr_t window,uint64_t token) const {
    for(const auto& entry:entries_)if(entry.window==window&&entry.token==token)return true;
    return false;
  }
  bool Remove(std::uintptr_t window,uint64_t token) {
    const auto found=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& entry){return entry.window==window&&entry.token==token;});
    if(found==entries_.end())return false;entries_.erase(found);return true;
  }
  NoticeSurfaceStamp Stamp() const {return {revision_,proof_==NoticeSurfaceProof::hidden,proof_!=NoticeSurfaceProof::hidden};}
  bool Current(const NoticeSurfaceStamp& state) const{
    const auto current=Stamp();return state.revision==revision_&&state.inlineAllowed==current.inlineAllowed&&state.blockedByNative==current.blockedByNative;
  }
  NoticeSurfaceStamp Set(NoticeSurfaceProof proof) {
    if(proof!=proof_){
      if(revision_==std::numeric_limits<int64_t>::max())proof_=NoticeSurfaceProof::unknown;
      else {proof_=proof;++revision_;}
    }
    return Stamp();
  }
  void Notify(const NoticeSurfaceStamp& state) {
    const auto snapshot=entries_;
    for(const auto& entry:snapshot) {
      // An earlier observer may reenter and show a newer surface or remove B.
      // Never deliver an old hidden proof after that newer visibility revision.
      if(!Current(state))return;
      if(Owns(entry.window,entry.token)){try{entry.callback(state);}catch(...){ }}
    }
  }
 private:
  struct Entry {std::uintptr_t window;uint64_t token;Callback callback;};
  std::vector<Entry> entries_;
  uint64_t serial_=0;
  int64_t revision_=1;
  NoticeSurfaceProof proof_=NoticeSurfaceProof::hidden; // No popup exists initially.
};

template<class Hidden,class Owned,class Hide,class Destroy>
bool TryHideOwnedNotice(Hidden hidden,Owned owned,Hide hide,Destroy destroy) {
  if(hidden())return true;
  if(!owned())return false;
  hide();if(hidden())return true;
  if(owned())destroy();
  return hidden(); // Neither API's return is substituted for actual absence/hidden proof.
}

// Own HWND/COM calls can pump the platform loop. Keep their old-owner cleanup
// indivisible with respect to inbound presentation operations, and release the
// fence BEFORE notification. There is deliberately no old-owner work afterward.
class NoticeTransitionFence {
 public:
  bool active() const{return active_;}
  template<class Commit,class Notify> bool Finish(Commit commit,Notify notify) {
    if(active_)return false;
    bool completed=false;
    {struct Reset {bool& flag;~Reset(){flag=false;}} reset{active_};active_=true;completed=commit();}
    if(completed)notify();
    return completed;
  }
 private:
  bool active_=false;
};

// Actual HWND operations live outside this state machine. 'visible' changes only
// after the caller acknowledges the real Show/Hide result. A fresh claim is an
// explicit new-transfer operation; updates never claim or automatically reacquire.
class NoticeLeaseModel {
 public:
  static constexpr uint64_t preparationMs=1500;
  NoticeReply Acquire(std::uintptr_t window,std::string owner,int generation) {
    if(!window||!NoticeOpaque(owner)||generation<0)return {};
    if(active_&&id_.window==window&&id_.owner==owner&&id_.generation==generation)return retiring_?NoticeReply{}:Reply(true);
    if(serial_==std::numeric_limits<uint64_t>::max()||revision_==std::numeric_limits<int64_t>::max())return {};
    ++serial_;std::string lease(32,'0');auto serial=serial_;
    for(std::size_t i=0;i<16;++i){lease[31-i]="0123456789abcdef"[serial&15];serial>>=4;}
    id_={window,std::move(owner),std::move(lease),generation};active_=true;have_=dismissed_=nativeVisible_=false;
    content_={};retiring_=false;phase_=NoticePhase::inlineCard;factsKnown_=false;ready_=false;requiresFrame_=true;deadline_=0;Advance();return Reply(true);
  }
  bool Owns(const NoticeIdentity& identity) const{return active_&&identity==id_;}
  bool Current(const NoticeStamp& stamp) const{return Owns(stamp.identity)&&stamp.revision==revision_;}
  NoticeReply Reply(bool accepted) const{return {accepted,Stamp()};}
  NoticeStamp Stamp() const{return {id_,revision_,nativeVisible_,phase_,requiresFrame_};}
  const TransferNoticeContent& content() const{return content_;}
  bool have() const{return have_;}
  bool active() const{return active_;}
  bool retiring() const{return retiring_;}
  bool Update(const NoticeIdentity& identity,const TransferNoticeContent& content) {
    if(retiring_||!Owns(identity)||content.generation!=id_.generation)return false;
    content_=content;have_=true;return true;
  }
  NoticeDecision Hide(const NoticeIdentity& identity) {
    if(!Owns(identity))return {};
    have_=false;dismissed_=false;ready_=false;deadline_=0;Advance();
    phase_=nativeVisible_?NoticePhase::hiding:NoticePhase::inlineCard;
    return {nativeVisible_?NoticeAction::hide:NoticeAction::presentation,Stamp()};
  }
  NoticeDecision Dismiss(const NoticeIdentity& identity) {
    if(!Owns(identity)||content_.kind==TransferNoticeContent::Kind::running)return {};
    dismissed_=true;ready_=false;deadline_=0;Advance();phase_=nativeVisible_?NoticePhase::hiding:NoticePhase::inlineCard;
    return {nativeVisible_?NoticeAction::hide:NoticeAction::presentation,Stamp()};
  }
  // Close old visibility/pending preparation BEFORE replacing a global claim.
  NoticeDecision ResetPresentation() {
    if(!active_)return {};
    deadline_=0;ready_=false;Advance();phase_=nativeVisible_?NoticePhase::hiding:NoticePhase::inlineCard;
    return {nativeVisible_?NoticeAction::hide:NoticeAction::presentation,Stamp()};
  }
  void Retire() {
    if(!active_)return;
    retiring_=true;have_=false;ready_=false;deadline_=0;Advance();
    phase_=nativeVisible_?NoticePhase::hiding:NoticePhase::inlineCard;
  }
  void Detach(std::uintptr_t window) {
    if(!active_||id_.window!=window)return;
    Advance();active_=have_=nativeVisible_=ready_=retiring_=false;deadline_=0;phase_=NoticePhase::inlineCard;
  }
  NoticeDecision Evaluate(bool foreground,bool nonpainted,uint64_t now) {
    if(!active_)return {};
    if(retiring_)return {NoticeAction::hide,Stamp()};
    const bool changed=factsKnown_&&(foreground!=lastForeground_||nonpainted!=lastNonpainted_);
    lastForeground_=foreground;lastNonpainted_=nonpainted;factsKnown_=true;
    if(phase_==NoticePhase::fallback&&changed)phase_=NoticePhase::inlineCard;
    if(!WantsBackgroundNotice(have_,dismissed_,foreground)) {
      if(phase_==NoticePhase::hiding)return {NoticeAction::hide,Stamp()};
      if(nativeVisible_||phase_==NoticePhase::preparing||ready_) {
        deadline_=0;ready_=false;Advance();phase_=nativeVisible_?NoticePhase::hiding:NoticePhase::inlineCard;
        return {nativeVisible_?NoticeAction::hide:NoticeAction::presentation,Stamp()};
      }
      return {};
    }
    if(phase_==NoticePhase::hiding)return {NoticeAction::hide,Stamp()};
    if(phase_==NoticePhase::nativeCard||phase_==NoticePhase::fallback)return {};
    if(phase_==NoticePhase::preparing) {
      if(now>=deadline_)return FailPreparation();
      if(!ready_)return {};
      if(!requiresFrame_&&!nonpainted)return BeginPreparation(true,now);
      return {NoticeAction::show,Stamp()};
    }
    return BeginPreparation(!nonpainted,now);
  }
  NoticeDecision Prepared(const NoticeStamp& stamp,bool prepared,bool foreground,bool nonpainted,uint64_t now) {
    if(retiring_||!Current(stamp)||phase_!=NoticePhase::preparing)return {};
    if(!prepared||now>=deadline_)return FailPreparation();
    ready_=true;return Evaluate(foreground,nonpainted,now);
  }
  NoticeDecision Shown(const NoticeStamp& stamp,bool visible) {
    if(retiring_||!Current(stamp)||phase_!=NoticePhase::preparing||!ready_)return {};
    if(!visible)return FailPreparation();
    ready_=false;deadline_=0;nativeVisible_=true;phase_=NoticePhase::nativeCard;
    return {NoticeAction::presentation,Stamp()};
  }
  NoticeDecision Hidden(const NoticeStamp& stamp,bool hidden) {
    if(!Current(stamp))return {};
    if(!hidden){phase_=NoticePhase::hiding;nativeVisible_=true;return {};}
    nativeVisible_=false;ready_=false;deadline_=0;phase_=NoticePhase::inlineCard;
    return {NoticeAction::presentation,Stamp()};
  }
  NoticeDecision Unavailable(const NoticeStamp& stamp) {return Current(stamp)?FailPreparation():NoticeDecision{};}
 private:
  NoticeIdentity id_;
  TransferNoticeContent content_;
  uint64_t serial_=0,deadline_=0;
  int64_t revision_=0;
  bool active_=false,have_=false,dismissed_=false,nativeVisible_=false,ready_=false,requiresFrame_=true,retiring_=false;
  bool factsKnown_=false,lastForeground_=true,lastNonpainted_=false;
  NoticePhase phase_=NoticePhase::inlineCard;
  void Advance(){if(revision_<std::numeric_limits<int64_t>::max())++revision_;}
  NoticeDecision BeginPreparation(bool frame,uint64_t now) {
    Advance();ready_=false;requiresFrame_=frame;phase_=NoticePhase::preparing;
    deadline_=now>std::numeric_limits<uint64_t>::max()-preparationMs?std::numeric_limits<uint64_t>::max():now+preparationMs;
    return {NoticeAction::prepare,Stamp()};
  }
  NoticeDecision FailPreparation() {
    Advance();ready_=false;deadline_=0;nativeVisible_=false;phase_=NoticePhase::fallback;
    return {NoticeAction::presentation,Stamp()};
  }
};
} // namespace multitap
