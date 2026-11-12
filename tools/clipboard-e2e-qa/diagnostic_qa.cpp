// Diagnostic attribution regression with the real worker/protocol. Every
// clipboard accessor, publisher and listener is substituted. No device, file,
// network, live clipboard or service is used; only a private message window.
#include "clipboard_worker.hpp"
#include <condition_variable>
#include <iostream>

namespace {
namespace clip=multitap::clipboard;
int checks=0,failures=0;
void check(bool value,const char* message){++checks;if(!value){++failures;std::cerr<<"FAIL "<<message<<'\n';}}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
class Rig {
 public:
  explicit Rig(bool scale):scale_(scale){
    host::ClipboardWorkerAccess access;
    access.read=[](HWND,bool,bool)->clip::Snapshot{throw std::runtime_error("Unexpected legacy read");};
    access.readFormats=[this](HWND,const clip::ReadOptions& options){
      require(options.virtualFiles.has_value(),"Negotiated tree bounds missing");
      observedEntries_=options.virtualFiles->bounds.entries;
      if(failRead_)throw std::runtime_error("clipboard_entry_count_limit");
      clip::Snapshot snapshot;snapshot.sequence=1;
      snapshot.image=clip::Image{std::vector<uint8_t>(86*1024,0x61),1,1};
      return snapshot;
    };
    access.putText=[](HWND,const std::wstring&,std::optional<DWORD>)->DWORD{throw std::runtime_error("Unexpected publication");};
    access.putFiles=[](HWND,const std::vector<std::filesystem::path>&,std::optional<DWORD>)->DWORD{throw std::runtime_error("Unexpected publication");};
    access.publishOffer=[](const std::shared_ptr<clip::PullOffer>&,std::optional<DWORD>)->DWORD{throw std::runtime_error("Unexpected offer publication");};
    access.offerCurrent=[](const std::shared_ptr<clip::PullOffer>&){return false;};
    access.withdrawOffer=[](const std::shared_ptr<clip::PullOffer>&)->DWORD{return 0;};
    access.watch.add=[](HWND){return true;};access.watch.remove=[](HWND){};
    access.watch.sequence=[]{return DWORD(1);};access.watch.owner=[]{return HWND(nullptr);};
    access.formats=[]{return uint32_t(0);};
    access.publishers.rich=[](HWND,const std::wstring&,const std::vector<uint8_t>*,const std::vector<uint8_t>*,std::optional<DWORD>)->DWORD{throw std::runtime_error("Unexpected rich publication");};
    access.publishers.image=[](HWND,const clip::Image&,std::optional<DWORD>)->DWORD{throw std::runtime_error("Unexpected image publication");};
    access.publishers.paths=access.putFiles;
    access.publishers.localTree=[](HWND,const std::vector<clip::TreeEntry>&,std::optional<DWORD>)->DWORD{throw std::runtime_error("Unexpected tree publication");};
    worker_=std::make_unique<host::ClipboardWorker>("diagnostic-qa",[this](const std::string& packet){receive(packet);},[]{return true;},std::filesystem::path{},std::move(access));
    std::string limits=scale_?
      R"({"chunkBytes":46080,"maxFiles":200000,"maxFileBytes":9007199254740991,"maxTotalBytes":9007199254740991,"maxTextUnits":67108864,"maxTextBytes":67108864,"limitsProfile":2,"formatsProfile":2,"maxEntries":200000,"maxDepth":512,"maxPathChars":1024,"maxRichBytes":67108864,"maxImageBytes":268435456,"maxImagePixels":64000000,"windowBytes":1048576,"windowUnits":32,"pullProfile":1,"maxOffers":2,"scaleProfile":1,"onPasteProfile":1,"pushTextBytes":1048576,"pushImageBytes":8388608})":
      R"({"chunkBytes":46080,"maxFiles":128,"maxFileBytes":4294967296,"maxTotalBytes":8589934592,"maxTextUnits":1048576,"maxTextBytes":1048576,"limitsProfile":2,"formatsProfile":2,"maxEntries":10000,"maxDepth":32,"maxPathChars":160,"maxRichBytes":8388608,"maxImageBytes":268435456,"maxImagePixels":64000000,"windowBytes":1048576,"windowUnits":32})";
    send(R"({"type":"clipboard.ready","sessionId":"diagnostic-qa","protocol":"clipboard-v1","clipboardSync":false,"limits":)"+limits+R"(,"clipboardPolicy":{"textToLocal":true,"filesToLocal":true,"textToRemote":true,"filesToRemote":true}})");
  }
  ~Rig(){worker_->stop();}
  void exercise(){
    send(R"({"type":"clipboard.read","requestId":"image-first"})");
    const auto previous=waitDiagnostic(host::ClipboardDiagnostic::sendEnd,0);
    check(previous.get("format").number()==4&&previous.get("items").number()==1&&previous.get("kib").number()==86,"The preceding image did not establish nonzero diagnostic metadata");
    failRead_=true;
    send(R"({"type":"clipboard.read","requestId":"folder-read-fails"})");
    const auto failed=waitDiagnostic(host::ClipboardDiagnostic::sendFailed,19);
    const auto error=waitError("folder-read-fails");
    check(observedEntries_==(scale_?200000u:10000u),"Negotiated bounds did not reach the fake native read");
    check(error.get("reason").string()==(scale_?"clipboard_entry_count_limit":"clipboard_limits_exceeded"),"The existing peer failure code changed");
    check(failed.get("ok").boolean()==false&&failed.get("cause").number()==19,"The real native admission cause or failed status was lost");
    check(failed.get("error").number()==(scale_?19:12),"The existing numeric peer reason changed");
    check(failed.get("expected").number()==(scale_?200000:10000),"The failure did not preserve its actual read-time entry bound");
    std::cout<<"profile="<<(scale_?"scale":"formats")<<" failedFormat="<<failed.get("format").number()<<" items="<<failed.get("items").number()<<" kib="<<failed.get("kib").number()<<" cause="<<failed.get("cause").number()<<" expected="<<failed.get("expected").number()<<'\n';
    check(failed.get("format").number()==0,"A fresh read failure was attributed to the preceding image format");
    check(failed.get("items").number()==0,"A fresh read failure reused the preceding image item count");
    check(failed.get("kib").number()==0,"A fresh read failure reused the preceding image size");
  }
 private:
  void send(const std::string& packet){worker_->message(host::JsonParser::parse(packet));}
  void receive(const std::string& raw){
    const auto wrapper=host::JsonParser::parse(raw);
    if(wrapper.get("type").string()=="clip.diag"){
      std::lock_guard lock(mutex_);diagnostics_.push_back(wrapper);condition_.notify_all();return;
    }
    if(wrapper.get("type").string()!="clip.packet")return;
    const auto packet=wrapper.get("payload");
    const auto type=packet.get("type").string(),id=packet.get("requestId").string();
    if(type=="clipboard.error"){std::lock_guard lock(mutex_);errors_.push_back(packet);condition_.notify_all();}
    // A wholly in-memory peer acknowledges the image bytes; it never renders or
    // publishes them, so no PNG decoder or real clipboard is involved.
    else if(type=="clipboard.chunk"){
      const auto bytes=host::clipboardChunk(packet.get("data").string(),clip::formatsChunkBytes);
      send("{\"type\":\"clipboard.chunk.ack\",\"requestId\":"+host::jsonQuote(id)+",\"index\":0,\"offset\":"+std::to_string(uint64_t(packet.get("offset").number())+bytes.size())+"}");
    }else if(type=="clipboard.item.end")send("{\"type\":\"clipboard.item.ack\",\"requestId\":"+host::jsonQuote(id)+",\"index\":0}");
    else if(type=="clipboard.commit")send("{\"type\":\"clipboard.complete\",\"requestId\":"+host::jsonQuote(id)+"}");
  }
  host::Json waitDiagnostic(uint32_t stage,uint32_t cause){
    std::unique_lock lock(mutex_);
    const auto matches=[&](const host::Json& packet){return packet.get("stage").number()==stage&&packet.get("cause").number()==cause;};
    require(condition_.wait_for(lock,std::chrono::seconds(4),[&]{return std::any_of(diagnostics_.begin(),diagnostics_.end(),matches);}),"Diagnostic response timed out");
    return *std::find_if(diagnostics_.begin(),diagnostics_.end(),matches);
  }
  host::Json waitError(const std::string& id){
    std::unique_lock lock(mutex_);
    const auto matches=[&](const host::Json& packet){return packet.get("requestId").string()==id;};
    require(condition_.wait_for(lock,std::chrono::seconds(4),[&]{return std::any_of(errors_.begin(),errors_.end(),matches);}),"Peer error response timed out");
    return *std::find_if(errors_.begin(),errors_.end(),matches);
  }
  const bool scale_;std::atomic<bool> failRead_{false};std::atomic<size_t> observedEntries_{0};
  std::unique_ptr<host::ClipboardWorker> worker_;std::mutex mutex_;std::condition_variable condition_;
  std::vector<host::Json> diagnostics_,errors_;
};
void serviceValidation(){
  // recordHostEvent returns before any machine-directory/file/registry lookup
  // when disabled. Never enable product operational logging in this fixture.
  require(!host::operationalLogEnabled,"Product logging must remain disabled");
  const auto packet=[](const std::string& extra){return host::JsonParser::parse(R"({"type":"clip.diag","sessionId":"diagnostic-qa","stage":6,"ok":false,"error":19,"cause":19,"format":0,"items":0,"kib":0)"+extra+"}");};
  check(host::recordClipboardWorkerDiagnostic(packet(""),"diagnostic-qa"),"Old worker without optional bound was rejected");
  for(const char* expected:{"0","10000","200000"})check(host::recordClipboardWorkerDiagnostic(packet(std::string(",\"expected\":")+expected),"diagnostic-qa"),"A valid bounded entry snapshot was rejected");
  for(const char* expected:{"-1","200001","0.5","\"10000\"","true","null"})check(!host::recordClipboardWorkerDiagnostic(packet(std::string(",\"expected\":")+expected),"diagnostic-qa"),"An invalid optional entry bound was accepted");
}
}
int main(){
  try{for(bool scale:{false,true}){Rig rig(scale);rig.exercise();}serviceValidation();
    std::cout<<"checks="<<checks<<" failures="<<failures<<" realClipboard=false network=false devices=false\n";return failures?1:0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
}
