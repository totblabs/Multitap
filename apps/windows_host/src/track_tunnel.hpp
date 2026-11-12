#pragma once
#include "machine.hpp"
#include "display_operation.hpp"
#include "websocket_send.hpp"
#include "stream_observability.hpp"
#include "host_observation_log.hpp"
#include <condition_variable>
#include <deque>
#include <optional>
namespace host {
inline std::wstring trackTunnelPath(const Credentials& credentials,const std::string& sessionId,const std::string& url){
  const auto path="/api/v1/hosts/sessions/"+sessionId+"/tracks";
  if(!displayOperationUuid(sessionId)||!credentialOrigin(credentials).relayMatches(url,path))throw std::runtime_error("Invalid track relay URL");
  return wide(path);
}
// tracks-resume-v1 (Broker server/src/session-resume.mjs): a session.start that
// carries "trackResumeProtocol":"tracks-resume-v1" keeps its worker through a
// lost track tunnel and dials the tunnel again, as host-resume-v1 does for the
// broker channel. Only an established tunnel lost to the transport (a receive
// or send error, not the broker's close frame) is held. Its media is dropped
// meanwhile; failed dials retry with a growing wait until holdMs after the
// loss. The broker decides within its own window and restarts every output at
// an IDR; a broker that ended the session also stops it on the channel.
inline constexpr char trackResumeProtocol[]="tracks-resume-v1";
inline bool trackTunnelControlAllowed(const std::string& type,bool udpTracks){
  static const std::set<std::string> ordinary{"input","input.reset","ping","stream.tracks.ready","stream.tracks.resize","cursor.compose","stream.keyframe"};
  static const std::set<std::string> scoped{"input.direct","udp.tracks.grant","udp.tracks.key","udp.tracks.peer","udp.tracks.lease"};
  return ordinary.contains(type)||(udpTracks&&scoped.contains(type));
}
class TrackTunnelHold {
 public:
  // Longer than the broker's detection (two heartbeats) plus its 30 s window.
  static constexpr uint64_t holdMs=60000;
  // stopped: the service ended it; closed: the broker's close frame or a
  // protocol error; failed: a dial that got no WebSocket; lost: an
  // established connection's transport failed.
  enum class Ended{stopped,closed,failed,lost};
  // A connection (or dial) ended. True: dial again after wait().
  bool ended(bool resumable,Ended why,uint64_t nowMs){
    if(!resumable||why==Ended::stopped||why==Ended::closed)return false;
    if(!since_){if(why!=Ended::lost||!established_)return false;since_=nowMs;}
    return nowMs-*since_<holdMs;
  }
  // A dial got its WebSocket: the next loss starts a new hold.
  void established(){established_=true;since_.reset();attempt_=0;}
  bool holding()const{return since_.has_value();}
  uint32_t wait(){return 250u<<std::min(attempt_++,3u);}  // 250 ms .. 2 s
 private:
  bool established_=false;std::optional<uint64_t> since_;uint32_t attempt_=0;
};
// Session0 owns this authenticated media connection. The interactive worker
// never receives the Host credential. Source failure leaves registration live.
class TrackSourceTunnel {
 public:
  using Receive=std::function<void(const std::string&)>;
  using Closed=std::function<void()>;
  ~TrackSourceTunnel(){stop();}
  DWORD httpStatus()const{return status_.load();}
  bool waitReady(DWORD timeoutMs){std::unique_lock lock(mutex_);condition_.wait_for(lock,std::chrono::milliseconds(timeoutMs),[this]{return stopped_||socket_.load()!=nullptr;});return !stopped_&&socket_.load()!=nullptr;}
  void cancel()noexcept{close();}
  void stop(){std::lock_guard lifecycle(lifecycle_);close();if(thread_.joinable())thread_.join();receive_={};closed_={};lost_={};}
  // While a lost tunnel is held (tracks-resume-v1) media is dropped, not queued.
  bool send(bool text,std::vector<uint8_t> bytes){
    bool overflow=false;{std::lock_guard lock(mutex_);if(stopped_)return false;if(holding_)return true;
      if(bytes.empty()||bytes.size()>(text?8192:4*1024*1024)||queue_.size()>=32||queued_+bytes.size()>8*1024*1024)overflow=true;
      else{queued_+=bytes.size();queue_.push_back({text,std::move(bytes),observability_.enabled()?Clock::now():Clock::time_point{}});}
    }if(overflow){close();return false;}condition_.notify_one();return true;
  }
  // `resumable`: the broker granted tracks-resume-v1; `lost` runs once when a
  // held loss begins (the service releases the worker's input then).
  void start(const Credentials& credentials,const std::string& sessionId,const std::string& url,Receive receive,Closed closed,const Json* observationRequest=nullptr,bool resumable=false,Closed lost={}){
    const auto path=trackTunnelPath(credentials,sessionId,url);
    std::lock_guard lifecycle(lifecycle_);stop();receive_=std::move(receive);closed_=std::move(closed);lost_=std::move(lost);stopped_=false;holding_=false;status_=0;udpTracks_=observationRequest&&observationRequest->get("udpTracksProtocol").string()=="udp-tracks-v1";
    if(observationRequest)observability_.configure(*observationRequest);else observability_.reset();
    thread_=std::thread([this,credentials,sessionId,path,resumable]{
      TrackTunnelHold hold;
      for(;;){
        const auto ended=connection(credentials,sessionId,path,hold);
        const bool first=!hold.holding();
        if(!hold.ended(resumable,ended,GetTickCount64()))break;
        if(first){{std::lock_guard lock(mutex_);holding_=true;queue_.clear();queued_=0;}if(lost_)try{lost_();}catch(...){ }}
        std::unique_lock lock(mutex_);if(condition_.wait_for(lock,std::chrono::milliseconds(hold.wait()),[this]{return stopped_.load();}))break;
      }
      close();if(closed_)try{closed_();}catch(...){ }
    });
  }
 private:
  struct Packet{bool text=false;std::vector<uint8_t> bytes;Clock::time_point queuedAt{};};
  // One dial and, when it gets its WebSocket, its whole life; why it ended.
  TrackTunnelHold::Ended connection(const Credentials& credentials,const std::string& sessionId,const std::wstring& path,TrackTunnelHold& hold){
    using Ended=TrackTunnelHold::Ended;
    std::thread sender;std::atomic<bool> done{false},failed{false};Ended ended=Ended::failed;
    try{
      const auto origin=verifiedCredentialOrigin(credentials);Internet session(httpSession());
      winCheck(WinHttpSetTimeouts(session.handle,2000,3000,3000,5000),"Track connection timeout");
      Internet connection(WinHttpConnect(session.handle,origin.hostname.c_str(),origin.port,0));
      Internet request(WinHttpOpenRequest(connection.handle,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,origin.flags()));
      DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
      winCheck(WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)),"Track relay redirects");
      winCheck(WinHttpSetOption(request.handle,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0),"Track relay WebSocket");
      if(stopped_)throw std::runtime_error("Track source cancelled");
      const auto authorization=L"Authorization: Host "+wide(credentials.token)+L"\r\n";
      winCheck(WinHttpSendRequest(request.handle,authorization.c_str(),DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0),"Track relay connect");
      winCheck(WinHttpReceiveResponse(request.handle,nullptr),"Track relay response");DWORD status=0,length=sizeof(status);
      winCheck(WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&length,WINHTTP_NO_HEADER_INDEX),"Track relay status");
      status_=status;
      if(status!=101)throw std::runtime_error("Track relay unavailable");
      const auto upgraded=WinHttpWebSocketCompleteUpgrade(request.handle,0);if(!upgraded)winCheck(FALSE,"Track relay upgrade");
      // stop() joins this attempt before another start(), so an old attempt
      // can never publish its handle into a replacement session.
      {std::lock_guard lock(mutex_);if(stopped_){WinHttpCloseHandle(upgraded);throw std::runtime_error("Track source cancelled");}socket_=upgraded;holding_=false;queue_.clear();queued_=0;}
      hold.established();ended=Ended::lost;
      condition_.notify_all();
      sender=std::thread([this,&done,&failed]{
        std::optional<uint64_t> lastWriteUs;
        try{while(!stopped_&&!done){Packet packet;{std::unique_lock lock(mutex_);condition_.wait(lock,[this,&done]{return stopped_||done||!queue_.empty();});if(stopped_||done)break;packet=std::move(queue_.front());queue_.pop_front();queued_-=packet.bytes.size();}
          if(observability_.enabled()&&packet.text){
            ObservedSendQueue snapshot;{std::lock_guard lock(mutex_);snapshot.packets=queue_.size();snapshot.bytes=queued_;
              if(!queue_.empty())snapshot.oldestUs=uint64_t(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-queue_.front().queuedAt).count()));}
            snapshot.writeUs=lastWriteUs;const auto raw=observability_.augment<Json>(std::string(packet.bytes.begin(),packet.bytes.end()),true,snapshot,{},JsonParser::parse,streamObservationJson);queueHostObservationLog(raw);packet.bytes.assign(raw.begin(),raw.end());
          }
          // Fragments of at most 8 KiB (websocket_send.hpp: a relay PING
          // during a large frame broke the connection).
          const auto began=observability_.enabled()?Clock::now():Clock::time_point{};
          if(sendWebSocketMessageOn(socket_.load(),packet.text,packet.bytes.data(),packet.bytes.size())!=NO_ERROR){failed=true;break;}
          if(observability_.enabled()&&!packet.text)lastWriteUs=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-began).count());
        }}catch(...){failed=true;}
        // The receive below returns once this connection's handle is closed.
        if(failed)dropSocket();
      });
      std::array<char,8192> buffer{};std::string json;
      while(!stopped_){const auto web=socket_.load();DWORD count=0;WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        if(!web)break;
        if(WinHttpWebSocketReceive(web,buffer.data(),DWORD(buffer.size()),&count,&type)!=NO_ERROR)break;
        if(type==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE){ended=Ended::closed;break;}
        if((type!=WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE&&type!=WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)||json.size()+count>8192){ended=Ended::closed;throw std::runtime_error("Invalid track control");}
        json.append(buffer.data(),count);
        if(type==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE){const auto value=JsonParser::parse(json);
          // cursor.compose: 게임 마우스 (cursor-compose-v1 on independent outputs).
          // stream.keyframe: tracks-resume-v1, every output restarts at an IDR.
          if(value.get("sessionId").string()!=sessionId||!trackTunnelControlAllowed(value.get("type").string(),udpTracks_)){ended=Ended::closed;throw std::runtime_error("Invalid track control binding");}
          if(receive_)receive_(json);json.clear();
        }
      }
    }catch(...){if(ended==Ended::lost)ended=Ended::closed;}
    done=true;dropSocket();if(sender.joinable())sender.join();
    return stopped_?Ended::stopped:ended;
  }
  // Ends the current connection only (stop() ends the tunnel).
  void dropSocket()noexcept{if(const auto web=socket_.exchange(nullptr))WinHttpCloseHandle(web);{std::lock_guard lock(mutex_);queue_.clear();queued_=0;}condition_.notify_all();}
  StreamObservability observability_;
  std::atomic<bool> stopped_{true};std::atomic<HINTERNET> socket_{nullptr};
  std::atomic<DWORD> status_{0};
  std::thread thread_;Receive receive_;Closed closed_,lost_;std::mutex mutex_;std::recursive_mutex lifecycle_;
  std::condition_variable condition_;std::deque<Packet> queue_;size_t queued_=0;bool holding_=false,udpTracks_=false;
  void close()noexcept{stopped_=true;dropSocket();}
};
}
