#pragma once
#include "machine.hpp"
#include "websocket_send.hpp"
#include "clipboard_reconnect.hpp"
#include <condition_variable>
#include <deque>
#include <functional>

namespace host {
class ClipboardChannel {
 public:
  using Receive=std::function<void(const std::string&)>;
  uint64_t generation()const{return generation_.current();}
  static bool transientNetworkError(DWORD error){
    return error==ERROR_WINHTTP_TIMEOUT||error==ERROR_WINHTTP_CANNOT_CONNECT||
      error==ERROR_WINHTTP_CONNECTION_ERROR||error==ERROR_WINHTTP_NAME_NOT_RESOLVED;
  }
  ~ClipboardChannel(){stop();}
  void stop(){
    std::lock_guard lifecycle(lifecycle_);quitting_=true;close();
    if(thread_.joinable())thread_.join();receive_={};
  }
  void send(const std::string& json,uint64_t generation){
    bool overflow=false;
    {std::lock_guard lock(mutex_);if(stopped_||!generation_.accepts(generation))return;
      // A pull queue's 4 MiB window is ~91 chunks of ~61 KB in flight, besides the control messages.
      if(json.size()>65000||queue_.size()>=192||bytes_+json.size()>12*1024*1024)overflow=true;
      else{queue_.push_back(json);bytes_+=json.size();}}
    if(overflow){retryDenied_=true;retryAllowed_=false;close();}else condition_.notify_one();
  }
  void start(const Credentials& credentials,const std::string& sessionId,const std::string& url,Receive receive){
    std::lock_guard lifecycle(lifecycle_);stop();const auto origin=credentialOrigin(credentials);
    if(!origin.relayMatches(url,"/api/v1/hosts/sessions/"+sessionId+"/clipboard"))throw std::runtime_error("Invalid clipboard relay URL");
    receive_=std::move(receive);quitting_=false;
    {std::lock_guard lock(mutex_);reconnect_=ClipboardReconnect{};resumed_=false;}
    thread_=std::thread([this,credentials,sessionId]{
      do {
        {std::lock_guard lock(mutex_);if(quitting_||!generation_.advance())break;stopped_=false;retryAllowed_=false;retryDenied_=false;}
        connectOnce(credentials,sessionId);
        close();
        if(receive_)try{receive_("{\"type\":\"clipboard.channel.closed\"}");}catch(...){}
        std::chrono::milliseconds delay{};
        std::unique_lock lock(mutex_);
        if(quitting_)break;
        auto step=reconnect_.next(retryAllowed_&&!retryDenied_,delay);
        // host-resume-v1: a channel lost with the held stream (or out of
        // attempts) waits for the stream to continue; stop() also ends the wait
        // (close() takes this mutex before it notifies).
        while(step==ClipboardReconnect::Step::wait){
          condition_.wait(lock,[this]{return quitting_.load()||resumed_;});
          if(quitting_)break;
          resumed_=false;step=reconnect_.next(true,delay);
        }
        if(quitting_||step!=ClipboardReconnect::Step::retry)break;
        if(condition_.wait_for(lock,delay,[this]{return quitting_.load();}))break;
      }while(!quitting_);
    });
  }
  // host-resume-v1 (ClipboardReconnect): the stream is held through a lost
  // broker channel, and it continues. Any thread; neither blocks.
  void away(){std::lock_guard lock(mutex_);reconnect_.away();}
  void resumed(){
    {std::lock_guard lock(mutex_);if(!reconnect_.resumed())return;resumed_=true;}
    condition_.notify_all();
  }
 private:
  ClipboardReconnect reconnect_;bool resumed_=false;
  std::atomic<bool> stopped_{true},quitting_{true},retryAllowed_{false},retryDenied_{false};
  std::atomic<HINTERNET> socket_{nullptr};std::thread thread_;Receive receive_;
  ClipboardPairGeneration generation_;
  std::mutex mutex_;std::recursive_mutex lifecycle_;std::condition_variable condition_;
  std::deque<std::string> queue_;size_t bytes_=0;
  void checked(BOOL ok,const char* message){
    if(ok)return;
    const auto error=GetLastError();
    // Certificate and authentication failures never become network outages.
    // Do not inspect or report arbitrary exception text.
    retryAllowed_=transientNetworkError(error);
    throw std::runtime_error(message);
  }
  void connectOnce(const Credentials& credentials,const std::string& sessionId){
    std::thread sender;
    try{
      const auto origin=verifiedCredentialOrigin(credentials);
      Internet session(httpSession()),connection(WinHttpConnect(session.handle,origin.hostname.c_str(),origin.port,0));
      const auto path=L"/api/v1/hosts/sessions/"+wide(sessionId)+L"/clipboard";
      Internet request(WinHttpOpenRequest(connection.handle,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,origin.flags()));
      DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
      checked(WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)),"Clipboard relay redirects");
      checked(WinHttpSetOption(request.handle,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0),"Clipboard relay WebSocket");
      // A fresh attempt repeats authenticated capability negotiation. No old
      // message or transfer survives it; released relays ignore unknown headers.
      const auto authorization=L"Authorization: Host "+wide(credentials.token)+L"\r\nX-Multitap-Clipboard-Limits: 2\r\nX-Multitap-Clipboard-Formats: 2\r\nX-Multitap-Clipboard-Pull: 1\r\nX-Multitap-Clipboard-Scale: 1\r\nX-Multitap-Clipboard-OnPaste: 1\r\nX-Multitap-Clipboard-Settled: 1\r\nX-Multitap-Clipboard-Intent: 1\r\nX-Multitap-Clipboard-PullQueue: 1\r\nX-Multitap-Clipboard-Deferred: 1\r\n";
      checked(WinHttpSendRequest(request.handle,authorization.c_str(),DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0),"Clipboard relay connect");
      checked(WinHttpReceiveResponse(request.handle,nullptr),"Clipboard relay response");
      DWORD status=0,length=sizeof(status);
      checked(WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&length,WINHTTP_NO_HEADER_INDEX),"Clipboard relay status");
      if(status!=101){retryAllowed_=status==408||status==502||status==503||status==504;throw std::runtime_error("Clipboard relay unavailable");}
      const auto web=WinHttpWebSocketCompleteUpgrade(request.handle,0);
      if(!web)checked(FALSE,"Clipboard relay upgrade");
      {std::lock_guard lock(mutex_);if(quitting_){WinHttpCloseHandle(web);return;}socket_=web;retryAllowed_=true;}
      sender=std::thread([this]{
        // How many messages of this connection reached the socket, told to the worker (clipboard.flow, never sent to the relay) every
        // 16 messages and whenever the queue is empty: a list's pages have no window of their own, and the worker sends the next page
        // only while few are still queued here (the bounds below would end the channel).
        uint64_t written=0;
        try{while(!stopped_){std::string json;bool drained=false;
          {std::unique_lock lock(mutex_);condition_.wait(lock,[this]{return stopped_||!queue_.empty();});
            if(stopped_)break;json=std::move(queue_.front());queue_.pop_front();bytes_-=json.size();drained=queue_.empty();}
          const auto sent=sendWebSocketMessageOn(socket_.load(),true,json.data(),json.size());
          if(sent!=NO_ERROR){if(!stopped_)retryAllowed_=transientNetworkError(sent);break;}
          if((++written%16==0||drained)&&receive_)try{receive_("{\"type\":\"clipboard.flow\",\"sent\":"+std::to_string(written)+'}');}catch(...){}
        }}catch(...){}close();
      });
      std::array<char,16384> buffer{};std::string json;
      while(!stopped_){
        DWORD count=0;WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};const auto socket=socket_.load();
        if(!socket)break;
        const auto received=WinHttpWebSocketReceive(socket,buffer.data(),DWORD(buffer.size()),&count,&type);
        if(received!=NO_ERROR){if(!stopped_)retryAllowed_=transientNetworkError(received);break;}
        if(type==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE){
          USHORT code=0;DWORD used=0;std::array<char,128> reason{};
          const auto queried=WinHttpWebSocketQueryCloseStatus(socket,&code,reason.data(),DWORD(reason.size()),&used);
          if(queried!=NO_ERROR)retryAllowed_=false;
          else if(code!=1000&&code!=1001)retryAllowed_=ClipboardReconnect::transientReason(std::string(reason.data(),used));
          break;
        }
        // Invalid data and callbacks fail closed. Only a fully checked packet
        // restores the transient-network classification.
        const bool previous=retryAllowed_.exchange(false);
        if(type!=WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE&&type!=WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)throw std::runtime_error("Non-JSON clipboard relay payload");
        if(json.size()+count>65000)throw std::runtime_error("Clipboard relay message exceeds private IPC bound");
        json.append(buffer.data(),count);
        if(type==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE){
          const auto parsed=JsonParser::parse(json);const auto packetType=parsed.get("type").string();
          if(packetType.rfind("clipboard.",0)!=0)throw std::runtime_error("Unexpected clipboard relay message");
          const bool ended=packetType=="clipboard.error"&&parsed.get("requestId").string().empty();
          if(receive_)receive_(json);
          retryAllowed_=ended?ClipboardReconnect::transientReason(parsed.get("reason").string()):previous;
          json.clear();
        }else retryAllowed_=previous;
      }
    }catch(...){}
    close();if(sender.joinable())sender.join();
  }
  void close(){
    stopped_=true;HINTERNET socket=nullptr;
    {std::lock_guard lock(mutex_);socket=socket_.exchange(nullptr);queue_.clear();bytes_=0;}
    if(socket)WinHttpCloseHandle(socket);condition_.notify_all();
  }
};
}
