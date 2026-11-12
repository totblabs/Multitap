#pragma once
#include "product_update.hpp"
#include <flutter/encodable_value.h>
#include <flutter/method_result.h>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace multitap {
// No window/engine pointer crosses the worker boundary. Completion and method
// results stay on the UI thread; closing cancels before the launch commit.
//
// Update mode with a handoff record (update-handoff-v1): the record and its
// named event exist before the installer starts. The installer signals the
// event once its own checks passed ("ready": this app closes and the
// installer relaunches it) or with a fixed failure code (this app stays open
// and shows it). The installer process ending first is a failure too. After
// a bounded wait the app closes anyway: the installer is running and owns
// the rest, including relaunching an app on every outcome.
class ProductUpdateTask {
 public:
  static constexpr UINT completed_message=WM_APP+182;
  // Posted to the window after a successful handoff; it closes without the
  // ordinary close questions (the update itself was the confirmed action).
  static constexpr UINT close_message=WM_APP+183;
  explicit ProductUpdateTask(HWND window):window_(window){}
  bool Busy() const { return result_ != nullptr || exit_committed_; }
  ~ProductUpdateTask(){if(state_){int preparing=0;state_->phase.compare_exchange_strong(preparing,2);state_->closed=true;}result_.reset();}
  void Start(ProductUpdateSpec spec,InstallerMode mode,std::optional<relaunch::Record> handoff,relaunch::Product product,
      DWORD wait_ms,std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    if(Busy()){result->Error("update_busy","An installer is already being prepared.");return;}
    auto state=std::make_shared<State>();state_=state;result_=std::move(result);
    const auto window=window_;
    try {
      std::thread([state,window,spec=std::move(spec),mode,handoff=std::move(handoff),product,wait_ms] {
        state->code=Run(*state,spec,mode,handoff,product,wait_ms);
        state->done=true;
        if(!state->closed)PostMessageW(window,completed_message,0,0);
      }).detach();
    } catch(...) {state->closed=true;state_.reset();auto pending=std::move(result_);pending->Error("installer_start_failed","The installer worker could not start.");}
  }
  void Complete() {
    if(!state_||!state_->done||!result_)return;
    const std::string code=state_->code;const bool close=state_->closeApp;state_->closed=true;state_.reset();auto result=std::move(result_);
    using V=flutter::EncodableValue;
    if(code.empty()){
      exit_committed_=close;
      result->Success(V(flutter::EncodableMap{{V("closing"),V(close)}}));
      if(close)PostMessageW(window_,close_message,0,0);
    } else result->Error(code,"The update did not start.");
  }
 private:
  // preparing -> committed OR closed is one atomic decision. Closing after the
  // commit cannot revoke a CreateProcess request already explicitly approved.
  struct State {std::atomic<bool> closed{false},done{false};std::atomic<int> phase{0};std::string code;bool closeApp=false;};
  static std::string Run(State& state,const ProductUpdateSpec& spec,InstallerMode mode,const std::optional<relaunch::Record>& handoff,
      relaunch::Product product,DWORD wait_ms) {
    const relaunch::Store store;
    HANDLE ready=nullptr;
    const bool seamless=mode==InstallerMode::Update&&handoff.has_value();
    if(seamless){
      // Before the installer exists: it reads the record and opens the event.
      if(!store.Write(product,*handoff))return "handoff_unavailable";
      ready=CreateEventW(nullptr,TRUE,FALSE,relaunch::EventName(handoff->token).c_str());
      if(!ready||GetLastError()==ERROR_ALREADY_EXISTS){if(ready)CloseHandle(ready);store.Clear(product);return "handoff_unavailable";}
    }
    HANDLE installer=nullptr;std::string failure;
    const bool launched=LaunchVerifiedProductUpdate(spec,[&state]{return state.phase.load()==2;},
        [&state]{int preparing=0;return state.phase.compare_exchange_strong(preparing,1);},mode,&installer,&failure,
        seamless?handoff->token:std::wstring());
    if(!launched){if(ready)CloseHandle(ready);if(seamless)store.Clear(product);return failure.empty()?"installer_start_failed":failure;}
    if(!seamless){
      CloseHandle(installer);
      // A legacy (record-less) client update still closes; the manual
      // installer leaves this window open and asks for itself.
      state.closeApp=mode==InstallerMode::Update&&product==relaunch::Product::Client;
      return {};
    }
    HANDLE waits[]{ready,installer};
    const DWORD signal=WaitForMultipleObjects(2,waits,FALSE,wait_ms);
    // The handle is the elevated update instance (the installed client EXE
    // started through UAC) for the unelevated client, or the installer itself
    // for Host settings. The instance's fixed exit codes name why it ended.
    DWORD exitCode=STILL_ACTIVE;
    const std::string helperFailure=signal==WAIT_OBJECT_0+1&&GetExitCodeProcess(installer,&exitCode)?StagedUpdateExitFailure(exitCode):std::string();
    CloseHandle(installer);CloseHandle(ready);
    if(!helperFailure.empty()){store.Clear(product);return helperFailure;}
    const auto answer=store.Read(product);
    const std::string outcome=answer&&answer->token==handoff->token?answer->outcome:std::string();
    if(outcome=="failed"||outcome=="blocked"){
      const std::string code=answer->code.empty()?"install_failed":answer->code;
      store.Clear(product);
      return code;
    }
    if(signal==WAIT_OBJECT_0+1&&outcome!="ready"){
      // The installer ended without accepting the update: nothing changed.
      store.Clear(product);
      return "installer_exited";
    }
    // Ready, or still running after the bounded wait: the installer owns it.
    state.closeApp=true;
    return {};
  }
  HWND window_;std::shared_ptr<State> state_;
  bool exit_committed_=false;
  std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result_;
};
}
