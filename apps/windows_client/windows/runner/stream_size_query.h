#pragma once
#include "stream_size_native.h"
#include "stream_size_query_owner.h"
#include <flutter/method_channel.h>
#include <optional>
namespace multitap {
// One process-wide actual query, queue0. Windows owns the one submitted callback;
// closing a view never waits for or cancels a vendor/decoder call. No detached
// std::thread, per-request thread growth or caller-owned worker destructor.
class StreamSizeQueryChannel {
 public:
  static constexpr UINT completed_message=WM_APP+193;
  static constexpr UINT_PTR timeout_timer=19043;
  static constexpr ULONGLONG observer_timeout_ms=5000;
  using V=flutter::EncodableValue;
  using Result=flutter::MethodResult<V>;
  struct Request {std::array<StreamSize,2> shapes{};size_t count=0;bool hardware=false;Microsoft::WRL::ComPtr<IDXGIAdapter> render;};
  explicit StreamSizeQueryChannel(HWND window):endpoint_(std::make_shared<Endpoint>()){endpoint_->window=window;}
  ~StreamSizeQueryChannel(){Close();}
  StreamSizeQueryChannel(const StreamSizeQueryChannel&)=delete;
  StreamSizeQueryChannel& operator=(const StreamSizeQueryChannel&)=delete;
  void Submit(Request request,std::unique_ptr<Result> reply){
    if(closed_||!endpoint_->window){reply->Error("session_replaced","Decoder query observer retired.");return;}
    if(!request.count||request.count>request.shapes.size()){reply->Error("invalid_arguments","One or two stream-size candidates required.");return;}
    // A newer request supersedes only this window's observation. A still-held
    // actual query remains busy and cannot be replaced by another native job.
    if(pending_)Retire();
    std::shared_ptr<StreamSizeActualSlot> actual;std::optional<StreamSizeActualSlot::Ticket> ticket;
    try{actual=ActualSlot();ticket=actual->Begin();}catch(...){reply->Error("stream_query_unknown","Decoder query ownership unavailable.");return;}
    if(!ticket){reply->Error("stream_query_busy","A decoder query is still running.");return;}
    std::unique_ptr<Job> job;
    try{
      job=std::make_unique<Job>();job->actual=actual;job->ticket=*ticket;job->request=std::move(request);job->endpoint=endpoint_;
      {std::lock_guard lock(endpoint_->mutex);stamp_=endpoint_->observer.Begin(*ticket);endpoint_->completion.reset();job->stamp=stamp_;}
      request_.shapes=job->request.shapes;request_.count=job->request.count;request_.hardware=job->request.hardware;
      pending_=std::move(reply);began_=GetTickCount64();
    }catch(...){auto answer=reply?std::move(reply):std::move(pending_);Retire();job.reset();actual->ActualReturned(*ticket);if(answer)answer->Error("stream_query_unknown","Decoder query could not be submitted.");return;}
    auto* context=job.release();
    if(!TrySubmitThreadpoolCallback(Work,context,nullptr)){
      std::unique_ptr<Job> unsubmitted(context);unsubmitted.reset();actual->ActualReturned(*ticket);
      RetireWithReply("stream_query_unknown");return; // API proved no callback was queued
    }
    if(!SetTimer(endpoint_->window,timeout_timer,UINT(observer_timeout_ms),nullptr))RetireWithReply("stream_query_unknown");
  }
  // UI observer retirement only. Actual slot remains owned until Work returns.
  // Session replacement drops the old result, so it cannot call a closed engine.
  void Retire(){
    if(endpoint_->window)KillTimer(endpoint_->window,timeout_timer);
    {std::lock_guard lock(endpoint_->mutex);endpoint_->observer.Retire();endpoint_->completion.reset();}
    pending_.reset();request_=Request{};
  }
  void Close(){
    if(closed_)return;closed_=true;Retire();
    std::lock_guard lock(endpoint_->mutex);endpoint_->observer.Close();endpoint_->window=nullptr;
  }
  void Expire(){
    bool ready=false;{std::lock_guard lock(endpoint_->mutex);ready=endpoint_->observer.Current(stamp_)&&endpoint_->completion.has_value();}
    if(ready){Complete();return;}
    if(pending_&&GetTickCount64()-began_>=observer_timeout_ms)RetireWithReply("stream_query_unknown");
  }
  void Complete(){
    std::optional<NativeStreamSizeResult> value;
    {std::lock_guard lock(endpoint_->mutex);if(endpoint_->observer.Current(stamp_)&&endpoint_->completion){value=std::move(endpoint_->completion);endpoint_->completion.reset();}}
    if(!value||!pending_)return;
    auto reply=std::move(pending_);const auto request=std::move(request_);Retire();
    // Everything owned by this observer is retired before the reply can reenter
    // Flutter/window teardown. No access to this after Success/Error.
    std::optional<V> encoded;
    try{encoded=Encode(request,*value);}catch(...){reply->Error("stream_query_unknown","Decoder query result could not be observed.");return;}
    try{reply->Success(*encoded);}catch(...){/* uncertain reply transport: never send a second terminal result */}
  }
 private:
  struct Endpoint {std::mutex mutex;StreamSizeQueryObserver observer;HWND window=nullptr;std::optional<NativeStreamSizeResult> completion;};
  struct Job {std::shared_ptr<StreamSizeActualSlot> actual;StreamSizeActualSlot::Ticket ticket;StreamSizeQueryObserver::Stamp stamp;Request request;std::weak_ptr<Endpoint> endpoint;};
  static std::shared_ptr<StreamSizeActualSlot> ActualSlot(){static const auto slot=std::make_shared<StreamSizeActualSlot>();return slot;}
  static void CALLBACK Work(PTP_CALLBACK_INSTANCE,void* context){
    std::unique_ptr<Job> job(static_cast<Job*>(context));NativeStreamSizeResult value;
    try{value=ProbeNativeStreamSizesOnWorker(job->request.shapes.data(),job->request.count,job->request.hardware,job->request.render.Get());}
    catch(...){value.reason="stream_decoder_unavailable";value.hresult=E_FAIL;}
    // Captured COM references also finish their actual release before another
    // query can claim this slot. UI timeout/close never calls ActualReturned.
    job->request.render.Reset();job->actual->ActualReturned(job->ticket);
    if(const auto endpoint=job->endpoint.lock()){
      HWND target=nullptr;{std::lock_guard lock(endpoint->mutex);if(endpoint->observer.Current(job->stamp)){endpoint->completion=value;target=endpoint->window;}}
      // An HWND reused after close only receives an empty notification; the new
      // endpoint has a different observer and no old result/pointer to consume.
      if(target)PostMessageW(target,completed_message,0,0);
    }
  }
  void RetireWithReply(const char* code){auto reply=std::move(pending_);Retire();if(reply)reply->Error(code,"Decoder query observation ended; native completion may still be pending.");}
  static V Encode(const Request& request,const NativeStreamSizeResult& value){
    flutter::EncodableList tracks;
    for(size_t i=0;i<request.count;++i){const auto& s=request.shapes[i];tracks.emplace_back(flutter::EncodableMap{{V("width"),V(int64_t(s.width))},{V("height"),V(int64_t(s.height))},{V("fps"),V(int64_t(s.fps))},{V("accepted"),V(value.accepted[i])},{V("reason"),value.accepted[i]?V():V(value.reason?value.reason:"stream_decoder_type")},{V("decoderPath"),value.accepted[i]?V(value.hardware[i]?"dxva-d3d11-type":"software-mft-type"):V()},{V("widthAlignment"),V(2)},{V("heightAlignment"),V(2)}});}
    return V(flutter::EncodableMap{{V("schema"),V(1)},{V("accepted"),V(!value.reason)},{V("reason"),value.reason?V(value.reason):V()},{V("hresult"),V(int32_t(value.hresult))},{V("tracks"),V(tracks)},{V("validation"),V("exact-candidate")},{V("decoder"),V("microsoft-h264")},{V("maxWidth"),V(int64_t(kMaximumVideoWidth))},{V("maxHeight"),V(int64_t(kMaximumVideoHeight))},{V("maxPixels"),V(int64_t(kMaximumVideoPixels))},{V("maxPixelRate"),V(0)}});
  }
  std::shared_ptr<Endpoint> endpoint_;std::unique_ptr<Result> pending_;Request request_;StreamSizeQueryObserver::Stamp stamp_{};ULONGLONG began_=0;bool closed_=false;
};
}
