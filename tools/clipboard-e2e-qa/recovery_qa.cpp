// Deterministic recovery QA. Every clipboard read/write/listener is substituted;
// no network, driver, service or interactive clipboard is used.
#include "e2e_common.hpp"
#include "clipboard_worker.hpp"
#include "clipboard_channel.hpp"  // compile the real WinHTTP adapter as well
#include <condition_variable>
#include <iostream>

namespace {
namespace clip=multitap::clipboard;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
class Rig {
 public:
  e2e::Board board;
  Rig(){
    host::ClipboardWorkerAccess access;
    access.read=[this](HWND w,bool t,bool f){return board.read(w,clip::ReadOptions{t,f,false,false});};
    access.readFormats=[this](HWND w,const clip::ReadOptions& o){return board.read(w,o);};
    access.putText=[this](HWND w,const std::wstring& t,std::optional<DWORD> e){
      {std::unique_lock lock(mutex);
        if(pauseNext){pauseNext=false;entered=true;condition.notify_all();require(condition.wait_for(lock,std::chrono::seconds(4),[this]{return released;}),"Native publication gate timed out");}}
      const auto result=board.putText(w,t,e);
      {std::lock_guard lock(mutex);mutations.push_back(t);}
      return result;
    };
    access.putFiles=[this](HWND w,const std::vector<std::filesystem::path>& f,std::optional<DWORD> e){return board.putFiles(w,f,e);};
    access.publishOffer=[this](const std::shared_ptr<clip::PullOffer>& o,std::optional<DWORD> e){return board.publishOffer(o,e);};
    access.offerCurrent=[this](const std::shared_ptr<clip::PullOffer>& o){return board.offerCurrent(o);};
    access.withdrawOffer=[this](const std::shared_ptr<clip::PullOffer>& o){return board.withdraw(o);};
    access.watch.add=[](HWND){return true;};access.watch.remove=[](HWND){};
    access.watch.sequence=[this]{return board.sequence();};access.watch.owner=[this]{return board.owner();};
    access.formats=[]{return uint32_t(0);};
    worker=std::make_unique<host::ClipboardWorker>("recovery-qa",[this](const std::string& packet){
      const auto value=host::JsonParser::parse(packet);
      if(value.get("type").string()!="clip.packet")return;
      std::lock_guard lock(mutex);packets.push_back(value.get("payload"));wire.push_back(value);condition.notify_all();
    },[]{return true;},std::filesystem::path{},std::move(access));
  }
  ~Rig(){worker->stop();}
  void send(const std::string& packet,uint64_t generation=0){if(generation)channelGeneration=generation;worker->message(host::JsonParser::parse(packet),channelGeneration);}
  void ready(uint64_t generation=1){send(R"({"type":"clipboard.ready","sessionId":"recovery-qa","protocol":"clipboard-v1","clipboardSync":true,"clipboardPolicy":{"textToLocal":true,"textToRemote":true,"filesToLocal":false,"filesToRemote":false}})",generation);}
  void queueTextStart(const std::string& id,char ch){
    send("{\"type\":\"clipboard.begin\",\"requestId\":"+host::jsonQuote(id)+",\"format\":\"text\",\"size\":1}");
    send("{\"type\":\"clipboard.chunk\",\"requestId\":"+host::jsonQuote(id)+",\"index\":0,\"offset\":0,\"data\":"+host::jsonQuote(host::clipboardBase64({uint8_t(ch)}))+"}");
  }
  void finishText(const std::string& id,char ch){
    wait("clipboard.chunk.ack",id);
    send("{\"type\":\"clipboard.item.end\",\"requestId\":"+host::jsonQuote(id)+",\"index\":0,\"sha256\":"+host::jsonQuote(host::clipboardDigest({uint8_t(ch)}))+"}");
    wait("clipboard.item.ack",id);
    send("{\"type\":\"clipboard.commit\",\"requestId\":"+host::jsonQuote(id)+"}");
  }
  void beginText(const std::string& id,char ch){queueTextStart(id,ch);finishText(id,ch);}
  void text(const std::string& id,char ch){
    beginText(id,ch);
    wait("clipboard.applied",id);
  }
  void blockNext(){std::lock_guard lock(mutex);pauseNext=true;entered=released=false;}
  void waitBlocked(){std::unique_lock lock(mutex);require(condition.wait_for(lock,std::chrono::seconds(4),[this]{return entered;}),"Native publication was not reached");}
  void unblock(){std::lock_guard lock(mutex);released=true;condition.notify_all();}
  uint64_t appliedGeneration(const std::string& id){std::lock_guard lock(mutex);for(const auto& packet:wire){const auto& payload=packet.get("payload");if(payload.get("type").string()=="clipboard.applied"&&payload.get("requestId").string()==id)return host::clipboardGenerationNumber(packet.get("generation").number());}return 0;}
  bool hasApplied(const std::string& id){std::lock_guard lock(mutex);for(const auto& packet:wire)if(packet.get("payload").get("type").string()=="clipboard.applied"&&packet.get("payload").get("requestId").string()==id)return true;return false;}
  void checkMutations(){std::lock_guard lock(mutex);require(mutations==std::vector<std::wstring>{L"a",L"x",L"b"},"Native mutation order crossed clipboard pairs");}
  void wait(const std::string& type,const std::string& id){
    std::unique_lock lock(mutex);
    const auto found=[&]{for(const auto& packet:packets)if(packet.get("requestId").string()==id&&(packet.get("type").string()==type||packet.get("type").string()=="clipboard.error"))return true;return false;};
    require(condition.wait_for(lock,std::chrono::seconds(4),found),"Recovery response timed out");
    for(auto it=packets.begin();it!=packets.end();++it)if(it->get("requestId").string()==id&&(it->get("type").string()==type||it->get("type").string()=="clipboard.error")){
      require(it->get("type").string()==type,"Fresh pair rejected after old sequence namespace");packets.erase(it);return;
    }
  }
 private:
  std::unique_ptr<host::ClipboardWorker> worker;
  std::mutex mutex;std::condition_variable condition;std::vector<host::Json> packets,wire;
  bool pauseNext=false,entered=false,released=false;
  std::vector<std::wstring> mutations;
  uint64_t channelGeneration=1;
};
}
int main(){
  try{
    host::ClipboardReconnect policy;std::chrono::milliseconds delay{};
    for(int expected:{1000,2000,4000}){require(policy.next(true,delay),"Retry missing");require(delay.count()==expected,"Unbounded retry delay");}
    require(!policy.next(true,delay),"Retry count exceeded");
    host::ClipboardReconnect denied;
    require(!denied.next(false,delay),"Policy rejection retried");
    for(const char* reason:{"authorization_revoked","clipboard_policy_denied","clipboard_invalid_message","clipboard_rate_limit","clipboard_commit_uncertain"})require(!host::ClipboardReconnect::transientReason(reason),"Unsafe reason retried");
    for(const char* reason:{"clipboard_transport_closed","clipboard_pair_timeout","clipboard_timeout"})require(host::ClipboardReconnect::transientReason(reason),"Transient reason lost");
    require(host::ClipboardChannel::transientNetworkError(ERROR_WINHTTP_CONNECTION_ERROR),"Established network failure was not classified");
    for(DWORD error:std::array<DWORD,4>{ERROR_WINHTTP_SECURE_FAILURE,ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED,ERROR_INVALID_DATA,ERROR_INVALID_HANDLE})require(!host::ClipboardChannel::transientNetworkError(error),"Established security/protocol I/O failure retried");
    host::ClipboardPairGeneration generation;require(generation.advance(),"Initial generation missing");
    Rig rig;rig.ready(generation.current());rig.text(std::string(32,'a')+"-0000000000000001",'a');
    const auto buffered=rig.appliedGeneration(std::string(32,'a')+"-0000000000000001");
    const auto old=std::string(32,'a')+"-0000000000000002";
    rig.blockNext();rig.beginText(old,'x');rig.waitBlocked();
    // Back-to-back close/ready/watch was previously reordered: stale closed
    // reset ready after the message thread already marked the new pair ready.
    rig.send(R"({"type":"clipboard.channel.closed"})");require(generation.advance(),"Next generation missing");rig.ready(generation.current());
    require(!generation.accepts(buffered),"Buffered old IPC packet crossed into a fresh pair");
    rig.send(R"({"type":"clipboard.watch","sessionId":"recovery-qa","enabled":true})");
    const auto fresh=std::string(32,'b')+"-0000000000000001";
    rig.queueTextStart(fresh,'b');
    require(rig.board.read(nullptr,clip::ReadOptions{true,false,false,false}).text==std::optional<std::wstring>(L"a"),"A fresh pair mutated clipboard before old native work quiesced");
    rig.unblock();rig.finishText(fresh,'b');rig.wait("clipboard.applied",fresh);rig.checkMutations();
    require(!rig.hasApplied(old),"Late native commit emitted an old applied packet into the new pair");
    require(generation.accepts(rig.appliedGeneration(std::string(32,'b')+"-0000000000000001")),"Fresh packet lost its generation");
    require(rig.board.read(nullptr,clip::ReadOptions{true,false,false,false}).text==std::optional<std::wstring>(L"b"),"Recovered copy not published");
    // A second fresh handshake may negotiate legacy request IDs independently.
    rig.send(R"({"type":"clipboard.channel.closed"})");
    require(generation.advance(),"Legacy generation missing");
    rig.send(R"({"type":"clipboard.ready","sessionId":"recovery-qa","protocol":"clipboard-v1","clipboardSync":false,"clipboardPolicy":{"textToLocal":true,"textToRemote":true,"filesToLocal":false,"filesToRemote":false}})",generation.current());
    rig.text("legacy-1",'c');
    std::cout<<"Clipboard recovery policy + fresh sequence/legacy pairs passed; realClipboard=false\n";return 0;
  }catch(const std::exception& failure){std::cerr<<failure.what()<<'\n';return 1;}
}
