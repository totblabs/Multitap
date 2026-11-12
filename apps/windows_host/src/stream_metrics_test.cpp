#include "support.hpp"
#include "stream_metrics.hpp"
#include "remote_engine.h"
#include <iomanip>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using host::Clock;
using V = flutter::EncodableValue;
void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
int64_t integer(const flutter::EncodableMap& stats, const char* field) {
  const auto item=stats.find(V(field));
  require(item!=stats.end(), "Native statistics field missing");
  const auto* value=std::get_if<int64_t>(&item->second);
  require(value!=nullptr, "Native counter is not an integer");
  return *value;
}
double real(const flutter::EncodableMap& stats, const char* field) {
  const auto item=stats.find(V(field));
  require(item!=stats.end(), "Native timing field missing");
  const auto* value=std::get_if<double>(&item->second);
  require(value&&std::isfinite(*value)&&*value>=0, "Native timing is negative or nonfinite");
  return *value;
}
bool boolean(const flutter::EncodableMap& stats, const char* field) {
  const auto item=stats.find(V(field));
  require(item!=stats.end(), "Native state field missing");
  const auto* value=std::get_if<bool>(&item->second);
  require(value!=nullptr, "Native state is not boolean");
  return *value;
}
class FakeRegistrar final : public flutter::TextureRegistrar {
 public:
  ~FakeRegistrar() override { retire(); }
  int64_t RegisterTexture(flutter::TextureVariant* texture) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id=++serial_; textures_.emplace(id,texture); return id;
  }
  bool MarkTextureFrameAvailable(int64_t id) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!textures_.contains(id)) return false;
    ++marks_[id];
    if (pauseNext_) {
      pauseNext_=false; paused_=true; wake_.notify_all();
      wake_.wait(lock,[this]{return resumed_;}); paused_=false;
    }
    return true;
  }
  void UnregisterTexture(int64_t id, std::function<void()> callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    // Deliberately retain the texture and its callback after unregister. This
    // models an in-flight Flutter render callback while the next stream starts.
    require(textures_.contains(id), "Unregistered an unknown QA texture");
    retired_.emplace_back(id,std::move(callback));
  }
  bool UnregisterTexture(int64_t id) override {
    UnregisterTexture(id,{}); return true;
  }
  const FlutterDesktopPixelBuffer* pull(int64_t id, size_t width, size_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found=textures_.find(id);
    require(found!=textures_.end(), "Requested a retired QA texture");
    return std::get<flutter::PixelBufferTexture>(*found->second).CopyPixelBuffer(width,height);
  }
  uint64_t marks(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_); return marks_[id];
  }
  void pauseNextMark() {
    std::lock_guard<std::mutex> lock(mutex_); pauseNext_=true; resumed_=false;
  }
  void awaitPausedMark() {
    std::unique_lock<std::mutex> lock(mutex_);
    require(wake_.wait_for(lock,std::chrono::seconds(60),[this]{return paused_;}),"QA native worker did not reach the controlled texture callback");
  }
  void resumeMark() {
    std::lock_guard<std::mutex> lock(mutex_); resumed_=true; pauseNext_=false; wake_.notify_all();
  }
  void retire() {
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& entry:retired_) { textures_.erase(entry.first); callbacks.push_back(std::move(entry.second)); }
      retired_.clear();
    }
    for (auto& callback:callbacks) if (callback) callback();
  }
 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool pauseNext_=false,paused_=false,resumed_=false;
  int64_t serial_=0;
  std::map<int64_t,flutter::TextureVariant*> textures_;
  std::map<int64_t,uint64_t> marks_;
  std::vector<std::pair<int64_t,std::function<void()>>> retired_;
};
struct PausedMark {
  FakeRegistrar& registrar;
  explicit PausedMark(FakeRegistrar& value):registrar(value){registrar.pauseNextMark();}
  ~PausedMark(){registrar.resumeMark();}
};
struct HeldPixel {
  const FlutterDesktopPixelBuffer* value=nullptr;
  ~HeldPixel() { if(value&&value->release_callback) value->release_callback(value->release_context); }
  HeldPixel()=default;
  explicit HeldPixel(const FlutterDesktopPixelBuffer* buffer):value(buffer){}
  HeldPixel(const HeldPixel&)=delete;
  HeldPixel& operator=(const HeldPixel&)=delete;
  HeldPixel(HeldPixel&& other) noexcept:value(std::exchange(other.value,nullptr)){}
};
uint64_t checksum(const FlutterDesktopPixelBuffer* pixel) {
  require(pixel&&pixel->buffer&&pixel->width&&pixel->height, "Decoder did not publish a pixel buffer");
  uint64_t sum=1469598103934665603ULL;
  for(size_t i=0;i<pixel->width*pixel->height*4;++i) sum=(sum^pixel->buffer[i])*1099511628211ULL;
  return sum;
}
std::string metricsJson(const host::StreamMetricsWindow& metrics) {
  std::ostringstream out; out<<std::fixed<<std::setprecision(2)<<"{\"kind\":\"metrics-qa\"";
  metrics.writeJson(out); out<<'}'; return out.str();
}
void windowGuards() {
  host::StreamMetricsWindow window;
  require(metricsJson(window).find("pipeline")==std::string::npos,"Empty metric window invented measurements");
  window.observe({1,2,3,4}); window.observe({2,4,6,8});
  auto pipeline=host::JsonParser::parse(metricsJson(window)).get("pipeline");
  require(pipeline.get("sampleCount").number()==2&&pipeline.get("topologyAvgMs").number()==1.5&&pipeline.get("captureAvgMs").number()==3&&pipeline.get("convertAvgMs").number()==4.5&&pipeline.get("encodeAvgMs").number()==6,"Metric stage averages are incorrect");
  require(pipeline.get("workAvgMs").number()==15&&pipeline.get("workP95Ms").number()==20&&pipeline.get("workMaxMs").number()==20,"Metric total or nearest-rank percentile is incorrect");
  require(pipeline.get("topologyMaxMs").number()==2&&pipeline.get("captureMaxMs").number()==4&&pipeline.get("convertMaxMs").number()==6&&pipeline.get("encodeMaxMs").number()==8,"Metric stage maxima are incorrect");
  window.reset(); window.observe({0,0,0,1});
  pipeline=host::JsonParser::parse(metricsJson(window)).get("pipeline");
  require(pipeline.get("encodeMaxMs").number()==1&&pipeline.get("captureMaxMs").number()==0,"Reset kept the previous window's stage maxima");
  for(double invalid:{-1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),60001.0}) {
    window.reset(); window.observe({1,2,3,4}); window.observe({0,invalid,0,0});
    require(metricsJson(window).find("pipeline")==std::string::npos,"Invalid timing leaked into JSON");
  }
  window.reset(); window.observe({20000,20000,20000,1});
  require(metricsJson(window).find("pipeline")==std::string::npos,"Excessive combined work leaked into JSON");
  window.reset(); for(int i=0;i<256;++i) window.observe({0,0,0,double(i)});
  pipeline=host::JsonParser::parse(metricsJson(window)).get("pipeline");
  require(pipeline.get("sampleCount").number()==256&&pipeline.get("workAvgMs").number()==127.5&&pipeline.get("workP95Ms").number()==243&&pipeline.get("workMaxMs").number()==255,"Full fixed metric window is incorrect");
  window.observe({0,0,0,1});
  require(metricsJson(window).find("pipeline")==std::string::npos,"Overflow silently reported a partial metric window");
  window.reset(); window.observe({0,0,0,0});
  pipeline=host::JsonParser::parse(metricsJson(window)).get("pipeline");
  require(pipeline.get("sampleCount").number()==1&&pipeline.get("workMaxMs").number(-1)==0,"Metric reset retained an invalid or old window");
}
std::vector<std::vector<uint8_t>> encodedPackets(int width,int height) {
  host::Encoder encoder(width,height,60,6000000);
  std::vector<std::vector<uint8_t>> packets;
  std::vector<uint8_t> bgra(size_t(width)*height*4),nv12(size_t(width)*height*3/2);
  const auto output=[&](host::Frame frame){packets.push_back(host::mediaPacket(1,frame.ptsUs,frame.bytes,frame.key));};
  for(int frame=0;frame<8;++frame) {
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
      const size_t at=(size_t(y)*width+x)*4;
      bgra[at]=uint8_t(30+frame*12+(x/32)%3);bgra[at+1]=uint8_t(80+frame*7+(y/32)%3);bgra[at+2]=uint8_t(150-frame*9);bgra[at+3]=255;
    }
    host::bgraToNv12(bgra.data(),nv12.data(),width,height);
    encoder.encode(nv12.data(),uint64_t(frame)*16666,output);
  }
  encoder.finish(output);
  require(packets.size()==8&&packets.front()[1]==1,"Actual encoder did not produce 8 frames starting with an IDR");
  return packets;
}
flutter::EncodableMap awaitDecoded(multitap::RemoteEngine& engine,int64_t count) {
  // Generous ceiling for a loaded release machine; returns on the count.
  const auto deadline=Clock::now()+std::chrono::seconds(60);
  for(;;) {
    auto stats=engine.Stats();
    require(integer(stats,"error")==S_OK,"Actual decoder reported failure");
    if(integer(stats,"decodedFrames")==count&&integer(stats,"decodeCalls")==count)return stats;
    require(Clock::now()<deadline,"Actual H.264 decoding did not reach the expected frame count in 60 seconds");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
void checkQuiescent(const flutter::EncodableMap& stats) {
  require(integer(stats,"queuedPackets")==0&&integer(stats,"queuedBytes")==0&&real(stats,"queueOldestMs")==0,"Processed native queue did not drain");
  require(!boolean(stats,"audioEnabled")&&integer(stats,"audioPlayedFrames")==0&&integer(stats,"audioReceivedFrames")==0&&integer(stats,"audioError")==S_OK,"Audio-off stream exposed playback activity");
  require(real(stats,"decodeTotalMs")>0&&real(stats,"decodeMaxMs")>0&&real(stats,"decodeTotalMs")>=real(stats,"decodeMaxMs"),"Actual decoder work was not measured");
  require(integer(stats,"textureNotifyCalls")==integer(stats,"decodedFrames")&&real(stats,"textureNotifyTotalMs")>=real(stats,"textureNotifyMaxMs")&&real(stats,"textureNotifyMaxMs")>=10,"Controlled texture notification delay was not reported separately");
}
void actualEngine() {
  host::Runtime runtime;
  FakeRegistrar registrar;
  multitap::RemoteEngine engine(&registrar);
  HeldPixel previous;
  uint64_t previousChecksum=0;
  int64_t retiredId=-1;
  unsigned generation=0;
  for(const auto [width,height]:std::vector<std::pair<int,int>>{{640,360},{800,450}}) {
    const auto packets=encodedPackets(width,height);
    const auto id=engine.Start(uint32_t(width),uint32_t(height),false);
    require(id>0,"Actual engine could not register a fresh texture");
    auto stats=engine.Stats();
    require(integer(stats,"decodedFrames")==0&&integer(stats,"textureFrames")==0&&integer(stats,"skippedDecodedFrames")==0&&integer(stats,"decodeCalls")==0&&integer(stats,"textureNotifyCalls")==0&&real(stats,"decodeTotalMs")==0&&real(stats,"textureNotifyTotalMs")==0,"New stream retained previous generation counters");
    require(registrar.pull(id,size_t(width),size_t(height))==nullptr,"New texture contained old geometry pixels before decoding");
    if(retiredId>0) {
      HeldPixel late(registrar.pull(retiredId,1,1));
      require(checksum(late.value)==previousChecksum&&checksum(previous.value)==previousChecksum,"Retired texture buffer was corrupted after Stop/start");
      stats=engine.Stats();
      require(integer(stats,"textureFrames")==0&&integer(stats,"skippedDecodedFrames")==0,"Old texture callback changed new stream counters");
      registrar.retire();
      require(checksum(previous.value)==previousChecksum,"Unregister completion freed a still-held pixel buffer");
    }
    require(!engine.Push(host::mediaPacket(2,0,std::vector<uint8_t>(8),false)),"Audio-off engine accepted an audio packet");
    {
      PausedMark paused(registrar);
      require(engine.Push(packets[0]),"Actual engine rejected the first video frame");
      registrar.awaitPausedMark();
      for(size_t i=1;i<4;++i)require(engine.Push(packets[i]),"Actual engine rejected a bounded video batch");
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      stats=engine.Stats();
      require(integer(stats,"queuedPackets")==3&&integer(stats,"queuedBytes")>0&&real(stats,"queueOldestMs")>=10,"Pending compressed packet age/queue length were not measured");
    }
    stats=awaitDecoded(engine,4);checkQuiescent(stats);
    // texture-pacing-v1: four frames published before any Flutter frame keep
    // the newest kPresentQueue (2) waiting; the older two are skipped.
    require(integer(stats,"textureFrames")==0&&integer(stats,"skippedDecodedFrames")==2&&registrar.marks(id)==4,"Decode callbacks were counted as Flutter texture consumption");
    HeldPixel first(registrar.pull(id,size_t(width),size_t(height)));
    require(first.value&&first.value->width==size_t(width)&&first.value->height==size_t(height),"Actual decoder exposed padded/stale dimensions");
    const auto firstChecksum=checksum(first.value);
    // The frame still waiting gets its own Flutter frame: the worker marks again.
    for(int n=0;n<60000&&registrar.marks(id)<5;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(registrar.marks(id)==5&&integer(engine.Stats(),"textureRemarks")==1,"A waiting frame did not request its Flutter frame");
    HeldPixel second(registrar.pull(id,size_t(width),size_t(height)));
    const auto secondChecksum=checksum(second.value);
    require(secondChecksum!=firstChecksum,"The waiting frame was not the next one shown");
    { HeldPixel repeated(registrar.pull(id,size_t(width),size_t(height)));require(checksum(repeated.value)==secondChecksum,"Repeated texture pull changed immutable pixels"); }
    stats=engine.Stats();
    require(integer(stats,"textureFrames")==2&&integer(stats,"skippedDecodedFrames")==2&&integer(stats,"textureQueued")==3,"Repeated callback inflated handoff FPS or skipped count");
    for(size_t i=4;i<8;++i)require(engine.Push(packets[i]),"Actual engine rejected the second bounded video batch");
    stats=awaitDecoded(engine,8);checkQuiescent(stats);
    HeldPixel third(registrar.pull(id,size_t(width),size_t(height)));
    for(int n=0;n<60000&&integer(engine.Stats(),"textureRemarks")<2;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    HeldPixel latest(registrar.pull(id,size_t(width),size_t(height)));
    require(checksum(first.value)==firstChecksum&&checksum(second.value)==secondChecksum,"Later frame publication mutated a previously held buffer");
    const auto latestChecksum=checksum(latest.value);
    require(latestChecksum!=secondChecksum&&latestChecksum!=checksum(third.value),"Synthetic motion did not update decoded pixels");
    stats=engine.Stats();
    require(integer(stats,"textureFrames")==4&&integer(stats,"skippedDecodedFrames")==4&&registrar.marks(id)==8+uint64_t(integer(stats,"textureRemarks"))&&integer(stats,"textureRemarks")==2,"Paced handoff counts are incorrect");
    // An upload is counted when Flutter releases the buffer; this test still
    // holds every buffer except the repeated pull's.
    require(integer(stats,"textureUploads")==1&&real(stats,"textureUploadTotalMs")>0&&real(stats,"textureWaitTotalMs")>0&&integer(stats,"texturePath")==0,"Texture upload/wait accounting");
    std::cout<<"{\"kind\":\"native-engine-metrics-case\",\"generation\":"<<++generation<<",\"width\":"<<width<<",\"height\":"<<height<<",\"actualEncodedDecodedFrames\":8,\"uniqueTextureHandoffs\":4,\"skippedDecodedFrames\":4,\"decodeTotalMs\":"<<real(stats,"decodeTotalMs")<<",\"decodeMaxMs\":"<<real(stats,"decodeMaxMs")<<",\"textureNotifyTotalMs\":"<<real(stats,"textureNotifyTotalMs")<<",\"textureNotifyMaxMs\":"<<real(stats,"textureNotifyMaxMs")<<",\"notificationArtificiallyPausedForQueueQA\":true,\"queueDrained\":true,\"audioEnabled\":false}\n";
    engine.Stop();
    require(!engine.Push(packets.front()),"Stopped engine accepted video");
    require(checksum(latest.value)==latestChecksum,"Stop invalidated a held pixel buffer");
    // Keep the previous generation's buffer held across the next Start and
    // deferred unregister completion without relying on its texture object.
    if(previous.value&&previous.value->release_callback)previous.value->release_callback(previous.value->release_context);
    previous.value=std::exchange(latest.value,nullptr);previousChecksum=latestChecksum;retiredId=id;
  }
  registrar.retire();
  require(checksum(previous.value)==previousChecksum,"Final texture retirement invalidated a held buffer");
}
}
int wmain() {
  try {
    windowGuards();actualEngine();
    std::cout<<"{\"kind\":\"native-stream-metrics-acceptance\",\"metricGuardsPassed\":true,\"actualSyntheticEncodedDecodedFrames\":16,\"actualGeometries\":2,\"pendingQueueAgeMeasured\":true,\"delayedTextureRetirement\":true,\"heldPixelLifetime\":true,\"audioDeviceOpened\":false,\"realDesktopCaptured\":false,\"physicalPresentationMeasured\":false,\"vmAccessed\":false,\"driverChanged\":false}\n";
    return 0;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
