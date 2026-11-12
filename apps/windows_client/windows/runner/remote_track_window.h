#pragma once
#include "remote_track_frame.h"
#include "remote_track_presenter.h"
#include <functional>
#include <memory>
namespace multitap {
// UI-thread owned secondary view. Bounds are local physical pixels supplied by
// local monitor inventory, never the VM's desktop coordinates. No new decoder,
// display-mode change, foreground activation or application quit is performed.
class RemoteTrackWindow {
 public:
  struct Binding {int64_t lease=0;uint8_t track=0;uint32_t topology=0,epoch=0;RECT bounds{};bool fullscreen=false;};
  struct Event {int64_t lease=0;uint8_t track=0;uint32_t topology=0,epoch=0;const char* type="";int width=0,height=0;HRESULT error=S_OK;};
  using Callback=std::function<void(const Event&)>;
  RemoteTrackWindow()=default;
  ~RemoteTrackWindow(){Close();}
  RemoteTrackWindow(const RemoteTrackWindow&)=delete;
  RemoteTrackWindow& operator=(const RemoteTrackWindow&)=delete;
  HRESULT Open(HWND owner,const Binding& binding,Callback callback={},bool visible=true,bool forceWarp=false);
  HRESULT Render(int64_t lease,const RemoteTrackFrame& frame);
  HRESULT Close(int64_t lease);
  void Close();
  HWND window()const;
  int64_t lease()const;
  RemoteTrackPresenter::Stats stats()const;
 private:
  struct State;
  static LRESULT CALLBACK Procedure(HWND,UINT,WPARAM,LPARAM) noexcept;
  static void Emit(const std::shared_ptr<State>& state,const char* type,HRESULT error=S_OK)noexcept;
  static HRESULT Draw(const std::shared_ptr<State>& state,const RemoteTrackFrame& frame);
  std::shared_ptr<State> state_;
  int64_t highest_=0;
};
}
