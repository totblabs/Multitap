#include "remote_media_session.h"
#include "remote_track_configuration.h"
#include "remote_track_window.h"
#include "../../apps/windows_client/test/native/track_gpu_fixture.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>

using V=flutter::EncodableValue;
namespace {
unsigned checks=0;
// The whole suite runs twice: on the CPU path (RGBA pixel buffers) and on the GPU
// path (shared BGRA textures; WARP stands in for a GPU). `hardware` is the user's
// "hardware decoding" setting as the client passes it to startTracks.
bool gpuMode=false;
std::map<std::string,uint64_t> pictureHashes;
void require(bool condition,const char* why){++checks;if(!condition)throw std::runtime_error(why);}
int64_t number(const flutter::EncodableMap& map,const char* key){int64_t value=0;require(multitap::MediaInteger(multitap::MediaField(map,key),value),"Missing integer statistic");return value;}
bool flag(const flutter::EncodableMap& map,const char* key){const auto field=multitap::MediaField(map,key);const auto value=field?std::get_if<bool>(field):nullptr;require(value!=nullptr,"Missing boolean statistic");return *value;}
struct Registrar: flutter::TextureRegistrar {
  struct Texture {flutter::TextureVariant* texture;bool retired=false;};
  // A texture callback taken like Flutter's raster thread: a pixel buffer is held
  // until ReleaseFrames; a GPU surface is read back through another device (the
  // way Flutter opens a shared handle) and released at once, so the output's
  // texture pool is not held up.
  struct Frame {int64_t id;const FlutterDesktopPixelBuffer* buffer;uint32_t width,height;std::vector<uint8_t> copy;
    const uint8_t* data()const{return buffer?buffer->buffer:copy.data();}};
  Microsoft::WRL::ComPtr<ID3D11Device> reader;Microsoft::WRL::ComPtr<ID3D11DeviceContext> readerContext;
  std::vector<uint8_t> readSurface(const FlutterDesktopGpuSurfaceDescriptor& surface){
    if(!reader)require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&reader,nullptr,&readerContext)),"Reader device");
    Microsoft::WRL::ComPtr<ID3D11Texture2D> opened;
    require(SUCCEEDED(reader->OpenSharedResource(surface.handle,IID_PPV_ARGS(&opened))),"The shared handle cannot be opened by another device");
    D3D11_TEXTURE2D_DESC desc{};opened->GetDesc(&desc);
    require(desc.Width==surface.width&&desc.Height==surface.height&&desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM,"Descriptor does not describe the texture");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;require(SUCCEEDED(reader->CreateTexture2D(&desc,nullptr,&staging)),"Staging");
    readerContext->CopyResource(staging.Get(),opened.Get());
    D3D11_MAPPED_SUBRESOURCE map{};require(SUCCEEDED(readerContext->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"Map");
    std::vector<uint8_t> pixels(size_t(desc.Width)*desc.Height*4);
    for(uint32_t y=0;y<desc.Height;++y)for(uint32_t x=0;x<desc.Width;++x){
      const uint8_t* p=static_cast<const uint8_t*>(map.pData)+size_t(y)*map.RowPitch+size_t(x)*4;uint8_t* t=&pixels[(size_t(y)*desc.Width+x)*4];
      t[0]=p[2];t[1]=p[1];t[2]=p[0];t[3]=p[3];
    }
    readerContext->Unmap(staging.Get(),0);return pixels;
  }
  std::mutex mutex;
  std::map<int64_t,Texture> textures;
  std::vector<std::function<void()>> retiring;
  std::vector<Frame> frames;
  int64_t next=100;
  unsigned registrations=0,failAt=0,throwAt=0;
  bool failNotify=false;
  std::function<void()> onRegister;
  std::function<void()> onMark;
  ~Registrar(){ReleaseFrames();Flush();}
  int64_t RegisterTexture(flutter::TextureVariant* texture)override{
    ++registrations;if(registrations==throwAt)throw std::bad_alloc();if(registrations==failAt)return -1;
    const int64_t id=++next;{std::lock_guard<std::mutex> lock(mutex);textures.emplace(id,Texture{texture});}
    if(onRegister)onRegister();return id;
  }
  bool MarkTextureFrameAvailable(int64_t id)override{
    std::lock_guard<std::mutex> lock(mutex);
    const auto it=textures.find(id);if(it==textures.end()||it->second.retired||failNotify)return false;
    if(auto* surface=std::get_if<flutter::GpuSurfaceTexture>(it->second.texture)){
      require(gpuMode,"A GPU surface texture on the CPU path");
      const auto* descriptor=surface->ObtainDescriptor(0,0);
      if(!descriptor)return false;
      require(descriptor->handle&&descriptor->struct_size==sizeof(*descriptor)&&descriptor->format==kFlutterDesktopPixelFormatBGRA8888,"Flutter frame without a surface");
      Frame frame{id,nullptr,uint32_t(descriptor->width),uint32_t(descriptor->height),readSurface(*descriptor)};
      descriptor->release_callback(descriptor->release_context);
      frames.push_back(std::move(frame));if(onMark)onMark();return true;
    }
    auto& buffer=std::get<flutter::PixelBufferTexture>(*it->second.texture);
    const auto pixels=buffer.CopyPixelBuffer(0,0);if(!pixels)return false;
    frames.push_back({id,pixels,uint32_t(pixels->width),uint32_t(pixels->height),{}});if(onMark)onMark();return true;
  }
  void UnregisterTexture(int64_t id,std::function<void()> callback)override{
    std::lock_guard<std::mutex> lock(mutex);auto& value=textures.at(id);require(!value.retired,"Texture retired twice");value.retired=true;
    retiring.push_back([this,id,callback=std::move(callback)]{textures.erase(id);callback();});
  }
  bool UnregisterTexture(int64_t id)override{UnregisterTexture(id,[]{});return true;}
  void Flush(){std::vector<std::function<void()>> pending;{std::lock_guard<std::mutex> lock(mutex);pending.swap(retiring);}for(auto& callback:pending)callback();}
  void ReleaseFrames(){std::lock_guard<std::mutex> lock(mutex);for(const auto& f:frames)if(f.buffer)f.buffer->release_callback(f.buffer->release_context);frames.clear();}
  size_t Active(){std::lock_guard<std::mutex> lock(mutex);size_t count=0;for(const auto& value:textures)if(!value.second.retired)++count;return count;}
};
multitap::tracks::Configuration config(uint32_t generation=1,bool secondary=false){
  using Track=multitap::tracks::Track;
  const Track first{0,1,640,360,640,360,-640,-100,true};
  const Track second{1,generation,generation==1?800u:960u,generation==1?600u:540u,generation==1?800u:960u,generation==1?600u:540u,0,0,false};
  return {generation,false,secondary?std::vector<Track>{second}:std::vector<Track>{second,first}};
}
void prepare(multitap::RemoteMediaSession& session){if(gpuMode)session.AllowSoftwareRenderForTests(true);}
HRESULT startTracks(multitap::RemoteMediaSession& session,int64_t lease,const multitap::tracks::Configuration& configuration,std::map<uint8_t,int64_t>& textures,const std::function<void()>& reset={}){
  return session.StartTracks(lease,configuration,textures,reset,gpuMode);
}
uint64_t fnv(uint64_t hash,const uint8_t* bytes,size_t size){for(size_t i=0;i<size;++i){hash^=bytes[i];hash*=1099511628211ULL;}return hash;}
// The first byte of a decoded picture, whichever path produced it.
uint8_t firstByte(const multitap::DecodedFrame& frame){return trackgpu::rgba(frame).front();}
std::vector<uint8_t> load(const std::filesystem::path& directory,uint32_t generation,unsigned track,unsigned frame){
  const auto name="topology-"+std::to_string(generation)+"-track-"+std::to_string(track)+"-frame-"+std::to_string(frame)+".tvt1";
  std::ifstream file(directory/name,std::ios::binary);require(bool(file),"Native MFT fixture is unavailable");
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
  multitap::tracks::Packet packet;require(multitap::tracks::parse(bytes.data(),bytes.size(),packet)&&packet.track==track&&packet.topology==generation,"Native fixture identity changed");return bytes;
}
template<class Predicate>flutter::EncodableMap wait(multitap::RemoteMediaSession& owner,int64_t lease,Predicate predicate){
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(std::chrono::steady_clock::now()<end){flutter::EncodableMap stats;
    require(owner.Stats(lease,stats)==S_OK,"Current owner stats were rejected");
    if(predicate(stats))return stats;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  throw std::runtime_error("Native track engine progress timeout");
}
void parseContract(){
  flutter::EncodableList tracks;
  for(const auto& t:config().tracks)tracks.emplace_back(flutter::EncodableMap{{V("id"),V(std::to_string(t.id))},{V("epoch"),V(int64_t(t.epoch))},
    {V("width"),V(int64_t(t.width))},{V("height"),V(int64_t(t.height))},{V("sourceWidth"),V(int64_t(t.sourceWidth))},
    {V("sourceHeight"),V(int64_t(t.sourceHeight))},{V("left"),V(int64_t(t.left))},{V("top"),V(int64_t(t.top))},{V("primary"),V(t.primary)}});
  V valid(flutter::EncodableMap{{V("topologyEpoch"),V(1)},{V("audioEnabled"),V(false)},{V("tracks"),V(tracks)}});
  multitap::tracks::Configuration parsed;require(multitap::TrackConfiguration(&valid,parsed)&&parsed.tracks.size()==2,"Real channel parser rejected independent outputs");
  const auto original=parsed;
  auto wrong=std::get<flutter::EncodableMap>(valid);wrong[V("topologyEpoch")]=V(int64_t(0x100000000ULL));V bad(wrong);
  require(!multitap::TrackConfiguration(&bad,parsed)&&parsed.topology==original.topology&&parsed.audioEnabled==original.audioEnabled&&parsed.tracks==original.tracks,"Oversized epoch changed parsed state");
  wrong=std::get<flutter::EncodableMap>(valid);auto badTracks=tracks;auto& first=std::get<flutter::EncodableMap>(badTracks[0]);first[V("left")]=V(int64_t(0x100000000ULL));wrong[V("tracks")]=V(badTracks);bad=V(wrong);
  require(!multitap::TrackConfiguration(&bad,parsed),"Coordinate narrowing admitted hostile native input");
  wrong=std::get<flutter::EncodableMap>(valid);wrong[V("extra")]=V(true);bad=V(wrong);require(!multitap::TrackConfiguration(&bad,parsed),"Unexpected channel configuration field admitted");
}
void nativeFrameAccess(const std::filesystem::path& directory){
  Registrar registrar;std::array<std::atomic<unsigned>,2> signals{};std::atomic<unsigned> invalid{0};
  multitap::RemoteMediaSession owner(&registrar,[&](int64_t lease,uint8_t id,uint32_t topology,uint32_t epoch){
    if(id>1||!((lease==10&&topology==1&&epoch==1)||(lease==11&&topology==2&&epoch==(id?2u:1u)))){++invalid;return;}++signals[id];
  });
  prepare(owner);
  std::map<uint8_t,int64_t> textures;require(startTracks(owner,10,config(),textures)==S_OK,"Native frame access source failed to start");
  multitap::RemoteTrackEngine::FrameSnapshot first,second;
  require(owner.LatestTrackFrame(10,0,1,1,first)==S_FALSE&&!first.pixels,"Unproduced source frame was reported available");
  require(owner.LatestTrackFrame(9,0,1,1,first)==E_ABORT&&owner.LatestTrackFrame(10,0,2,1,first)==E_ABORT&&owner.LatestTrackFrame(10,0,1,2,first)==E_ABORT&&owner.LatestTrackFrame(10,2,1,1,first)==E_ABORT,"Retired native frame owner/topology/track was admitted");
  for(unsigned n=0;n<4;++n){for(unsigned id=0;id<2;++id)require(owner.Push(10,load(directory,1,id,n))==S_OK,"Native frame access packet failed");wait(owner,10,[&](const auto& stats){return number(stats,"decodedFrames")==int64_t(2*(n+1))&&signals[0]>=1&&signals[1]>=1;});}
  require(signals[0]==1&&signals[1]==1&&invalid==0,"Unconsumed UI notifications were not coalesced per output");
  require(owner.LatestTrackFrame(10,0,1,1,first)==S_OK&&first.track==0&&first.topology==1&&first.epoch==1&&first.serial==4&&first.pixels&&first.pixels->width==640&&first.pixels->height==360,"Primary source snapshot lost its identity or actual decoded size");
  const auto held=first.pixels;const auto pixel=firstByte(*held);
  require(owner.Push(10,load(directory,1,0,4))==S_OK,"Native frame follow-up failed");wait(owner,10,[&](const auto& stats){return number(stats,"decodedFrames")==9&&signals[0]==2;});
  require(signals[1]==1&&owner.LatestTrackFrame(10,1,1,1,second)==S_OK&&second.serial==4&&second.pixels->width==800&&second.pixels->height==600&&second.pixels.get()!=first.pixels.get(),"Secondary surface reused the primary pixels or notification ownership");
  flutter::EncodableMap stats;require(owner.Stats(10,stats)==S_OK,"Native read stats unavailable");const auto* outputs=std::get_if<flutter::EncodableList>(multitap::MediaField(stats,"tracks"));require(outputs&&outputs->size()==2,"Native read output stats missing");
  for(const auto& output:*outputs){const auto& row=std::get<flutter::EncodableMap>(output);require(number(row,"nativeFrameReads")==1&&number(row,"nativeSkippedDecodedFrames")==3,"Native frame reads were confused with texture acquisition or presentation");}
  require(startTracks(owner,11,config(2),textures)==S_OK&&owner.LatestTrackFrame(10,1,1,1,second)==E_ABORT&&owner.LatestTrackFrame(11,1,1,1,second)==E_ABORT,"Replaced native frame binding reached a new topology");
  require(held->width==640&&held->height==360&&firstByte(*held)==pixel,"Retired snapshot lost its shared pixel lifetime");
  for(unsigned id=0;id<2;++id)require(owner.Push(11,load(directory,2,id,0))==S_OK,"Replacement native frame source failed");wait(owner,11,[](const auto& value){return flag(value,"tracksReady");});
  require(owner.LatestTrackFrame(11,1,2,2,second)==S_OK&&second.topology==2&&second.epoch==2&&second.pixels->width==960&&second.pixels->height==540&&invalid==0,"New native frame size/epoch or notification lease was not isolated");
  owner.Close();require(owner.LatestTrackFrame(11,1,2,2,second)==E_ABORT&&firstByte(*held)==pixel,"Closed source remained readable or invalidated held pixels");registrar.ReleaseFrames();registrar.Flush();
}
size_t staleBuffers=0;
void suite(const std::filesystem::path& directory){
    Registrar registrar;multitap::RemoteMediaSession owner(&registrar);prepare(owner);
    int64_t legacy=-1;require(owner.Start(1,640,360,false,legacy)==S_OK,"Legacy production engine did not start");
    std::map<uint8_t,int64_t> textures;
    require(startTracks(owner,2,config(),textures)==S_OK&&textures.size()==2&&textures.at(0)!=textures.at(1),"Track outputs share one texture");
    require(registrar.Active()==2,"Legacy texture survived track replacement");
    auto invalid=config();invalid.tracks[0].id=0;
    require(startTracks(owner,99,invalid,textures)==E_INVALIDARG&&owner.Current(2)&&registrar.Active()==2,"Invalid output metadata retired current native owner");
    unsigned oldEffects=0;bool accepted=false;flutter::EncodableMap ignored;
    require(owner.Stop(1,[&]{++oldEffects;})==S_FALSE,"Retired stop touched current owner");
    require(owner.Push(1,load(directory,1,0,0))==E_ABORT,"Retired push reached current decoder");
    require(owner.Stats(1,ignored)==E_ABORT,"Retired stats observed current output");
    require(owner.Capture(1,[&]{++oldEffects;return true;},accepted)==E_ABORT&&oldEffects==0,"Retired capture/reset changed input");
    require(owner.Start(1,640,360,false,legacy)==E_ABORT&&startTracks(owner,2,config(),textures)==E_ABORT,"Replayed native start replaced decoder owner");
    unsigned totalFrames=0;
    for(unsigned phase=1;phase<=2;++phase){
      if(phase==2){
        require(startTracks(owner,3,config(2),textures)==S_OK,"New topology owner did not start");
        require(owner.Push(3,load(directory,1,0,0))==S_OK,"Bounded old packet was not admitted for stale filtering");
        wait(owner,3,[](const auto& stats){return number(stats,"stalePackets")==1;});
        totalFrames=0;
      }
      const int64_t lease=phase+1;
      for(unsigned n=0;n<8;++n)for(unsigned id:{1u,0u}){
        require(owner.Push(lease,load(directory,phase,id,n))==S_OK,"Native track queue rejected paced fixture");
        ++totalFrames;const bool expectedReady=!(n==0&&id==1);
        const auto stats=wait(owner,lease,[&](const auto& value){return number(value,"decodedFrames")==totalFrames&&flag(value,"tracksReady")==expectedReady;});
        require(flag(stats,"tracksReady")==expectedReady,"First output unlocked missing second decoded output");
      }
      std::lock_guard<std::mutex> lock(registrar.mutex);unsigned perTrack[2]{};uint64_t hashes[2]{1469598103934665603ULL,1469598103934665603ULL};
      for(const auto& f:registrar.frames){
        const auto id=f.id==textures.at(0)?0:f.id==textures.at(1)?1:2;if(id==2)continue;
        require(f.width==(id?(phase==1?800u:960u):640u)&&f.height==(id?(phase==1?600u:540u):360u),"Flutter texture geometry crossed output/epoch");
        const size_t center=(f.height/2*f.width+f.width/2)*4;
        require(id?f.data()[center]>170:f.data()[center]<65,"Actual decoder pixels crossed outputs");++perTrack[id];
        hashes[id]=fnv(hashes[id],f.data(),size_t(f.width)*f.height*4);
      }
      require(perTrack[0]==8&&perTrack[1]==8,"Actual texture callbacks lost independent frames");
      for(unsigned id=0;id<2;++id)pictureHashes[std::string(gpuMode?"gpu":"cpu")+"/"+std::to_string(phase)+"/"+std::to_string(id)]=hashes[id];
      {
        // texturePath / hardwareDecode per output and overall; requested path = applied path.
        flutter::EncodableMap stats;require(owner.Stats(lease,stats)==S_OK,"Track statistics unavailable");
        require(number(stats,"texturePath")==(gpuMode?1:0)&&flag(stats,"hardwareDecode")==(number(stats,"decodeTier")>0),"Overall texture path or hardware flag is wrong");
        const auto* rows=std::get_if<flutter::EncodableList>(multitap::MediaField(stats,"tracks"));require(rows&&rows->size()==2,"Per-output statistics missing");
        for(const auto& value:*rows){const auto& row=std::get<flutter::EncodableMap>(value);
          require(number(row,"texturePath")==(gpuMode?1:0)&&flag(row,"flutterSurface")==gpuMode,"Per-output texture path is wrong");
          require(flag(row,"hardwareDecode")==(number(row,"decodeTier")>0)&&number(row,"gpuDroppedFrames")==0&&number(row,"decoderFallbacks")>=0,"Per-output decoder statistics are wrong");}
      }
    }
    // Buffers remain valid after async texture retirement and replacement.
    require(owner.Stop(3)==S_OK&&registrar.Active()==0,"Current native stop left live textures");registrar.Flush();
    {std::lock_guard<std::mutex> lock(registrar.mutex);require(registrar.frames.size()==32&&registrar.frames[0].data()[0]>170,"Retired held pixel buffer was freed early");}
    staleBuffers=registrar.frames.size();registrar.ReleaseFrames();
    require(startTracks(owner,4,config(1,true),textures)==S_OK&&textures.size()==1&&textures.count(1),"Sparse connector1 invented output0");
    require(owner.Push(4,load(directory,1,1,0))==S_OK,"Sparse output packet rejected");
    wait(owner,4,[](const auto& stats){return flag(stats,"tracksReady");});owner.Stop(4);registrar.ReleaseFrames();registrar.Flush();
    {
      Registrar failed;failed.failAt=2;multitap::RemoteMediaSession partial(&failed);prepare(partial);
      require(startTracks(partial,1,config(),textures)==E_FAIL&&partial.Lease()==0&&failed.Active()==0,"Partial registration did not roll back both textures");failed.Flush();
      require(startTracks(partial,1,config(),textures)==E_ABORT,"Failed native lease was reusable");
      failed.throwAt=failed.registrations+1;require(startTracks(partial,2,config(),textures)==E_OUTOFMEMORY&&partial.Lease()==0,"Allocation exception escaped owner boundary");
      require(startTracks(partial,3,config(),textures)==S_OK,"New owner could not recover after failed texture registration");
      HRESULT nested=S_OK;
      require(partial.Capture(3,[&]{nested=partial.Stop(3);return true;},accepted)==S_OK&&nested==HRESULT_FROM_WIN32(ERROR_BUSY)&&partial.Current(3),"Reentrant capture stopped in-flight native command");
      partial.Stop(3);failed.Flush();
    }
    {
      Registrar deleting;auto self=std::make_unique<multitap::RemoteMediaSession>(&deleting);prepare(*self);
      require(startTracks(*self,1,config(),textures,[&]{self.reset();})==E_ABORT&&!self&&deleting.Active()==0,"Owner deletion during startup was not deferred");
      self=std::make_unique<multitap::RemoteMediaSession>(&deleting);prepare(*self);
      require(startTracks(*self,1,config(),textures)==S_OK,"Deletion fixture startup failed");
      require(self->Capture(1,[&]{self.reset();return true;},accepted)==E_ABORT&&!self&&deleting.Active()==0,"Owner deletion during capture invalidated pinned lifetime");deleting.Flush();
      self=std::make_unique<multitap::RemoteMediaSession>(&deleting);prepare(*self);deleting.onRegister=[&]{self.reset();};
      require(startTracks(*self,1,config(),textures)==E_ABORT&&!self&&deleting.Active()==0,"Registrar reentry destroyed in-progress engine");deleting.onRegister={};deleting.Flush();
    }
    {
      Registrar failed;failed.failNotify=true;multitap::RemoteMediaSession notify(&failed);prepare(notify);
      require(startTracks(notify,1,config(1,true),textures)==S_OK&&notify.Push(1,load(directory,1,1,0))==S_OK,"Notification failure fixture setup failed");
      wait(notify,1,[](const auto& stats){return FAILED(static_cast<HRESULT>(number(stats,"error")))&&!flag(stats,"tracksReady");});
      require(notify.Push(1,load(directory,1,1,1))==multitap::kMediaDecoderStopped,"Failed texture delivery silently resumed");
      notify.Stop(1);failed.Flush();
    }
    {
      Registrar bounded;multitap::RemoteMediaSession queue(&bounded);prepare(queue);
      struct Gate {
        std::mutex mutex;std::condition_variable cv;bool entered=false,released=false;
        void Release(){std::lock_guard<std::mutex> lock(mutex);released=true;cv.notify_all();}
        ~Gate(){Release();}
      } gate;
      struct ReleaseWorker {
        Gate& gate;multitap::RemoteMediaSession& queue;Registrar& registrar;
        ~ReleaseWorker(){gate.Release();queue.Close();registrar.onMark={};}
      } cleanup{gate,queue,bounded};
      bounded.onMark=[&]{std::unique_lock<std::mutex> lock(gate.mutex);gate.entered=true;gate.cv.notify_all();gate.cv.wait(lock,[&]{return gate.released;});};
      require(startTracks(queue,1,config(1,true),textures)==S_OK,"Bounded queue fixture did not start");
      require(queue.Push(1,load(directory,1,1,0))==S_OK,"Bounded queue initial frame rejected");
      {std::unique_lock<std::mutex> lock(gate.mutex);require(gate.cv.wait_for(lock,std::chrono::seconds(5),[&]{return gate.entered;}),"Texture notification did not reach intentional stall");}
      const auto bytes=load(directory,1,1,0);
      for(unsigned n=0;n<8;++n){auto next=bytes;multitap::tracks::write64(next.data()+16,n+1);require(queue.Push(1,next)==S_OK,"Queue rejected bounded slot");}
      // A full queue is transient backpressure (offer again), not a failure.
      require(queue.Push(1,bytes)==multitap::kMediaQueueFull,"Queue admitted a ninth waiting packet");
      flutter::EncodableMap stats;require(queue.Stats(1,stats)==S_OK&&number(stats,"queuedPackets")==8&&number(stats,"queuedBytes")==int64_t(bytes.size()*8),"Bounded queue accounting differs from admitted bytes");
      gate.Release();queue.Stop(1);bounded.onMark={};bounded.ReleaseFrames();bounded.Flush();
      require(startTracks(queue,2,config(1,true),textures)==S_OK&&queue.Stats(2,stats)==S_OK&&number(stats,"queuedPackets")==0,"New native owner inherited queued old frames");queue.Stop(2);bounded.Flush();
    }
    nativeFrameAccess(directory);
}
// Native track windows (the swap-chain presentation): each output's engine
// picture - a CPU RGBA picture or a shared GPU texture - is presented by its own
// RemoteTrackWindow. On the GPU path the window's swap chain lives on the device
// that owns the picture (copy GPU to GPU) and never falls back.
void nativeWindows(const std::filesystem::path& directory){
  Registrar registrar;multitap::RemoteMediaSession owner(&registrar);prepare(owner);
  std::map<uint8_t,int64_t> textures;require(startTracks(owner,20,config(),textures)==S_OK,"Native window source failed to start");
  unsigned total=0;
  for(unsigned n=0;n<4;++n)for(unsigned id:{1u,0u}){require(owner.Push(20,load(directory,1,id,n))==S_OK,"Native window packet rejected");++total;
    wait(owner,20,[&](const auto& stats){return number(stats,"decodedFrames")==total;});}
  const HWND host=CreateWindowExW(0,L"STATIC",L"Multitap track window QA owner",WS_POPUP,0,0,16,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  require(host!=nullptr,"Owner window");
  {
    std::array<multitap::RemoteTrackWindow,2> views;const RECT bounds[2]{{200,100,840,460},{1000,100,1800,700}};
    for(unsigned id=0;id<2;++id){
      multitap::RemoteTrackWindow::Binding binding;binding.lease=20;binding.track=uint8_t(id);binding.topology=1;binding.epoch=1;binding.bounds=bounds[id];
      require(views[id].Open(host,binding,{},false,true)==S_OK,"Hidden track window did not open");
    }
    for(unsigned id=0;id<2;++id){
      multitap::RemoteTrackEngine::FrameSnapshot snapshot;require(owner.LatestTrackFrame(20,uint8_t(id),1,1,snapshot)==S_OK&&snapshot.pixels,"Output picture unavailable");
      require(gpuMode?(snapshot.pixels->gpu&&snapshot.pixels->rgba.empty()):(!snapshot.pixels->gpu),"Engine delivered the wrong picture kind to the window");
      const HRESULT hr=views[id].Render(20,snapshot);require(hr==S_OK||hr==DXGI_STATUS_OCCLUDED,"Track window did not accept the picture");
      const auto stats=views[id].stats();
      require(stats.uploads==1&&SUCCEEDED(stats.failure),"Track window upload failed");
      require(gpuMode?(stats.gpu&&stats.gpuUploads==1&&stats.gpuFallbacks==0&&SUCCEEDED(stats.gpuFailure)):(!stats.gpu&&stats.gpuUploads==0),"Track window did not use the expected presentation path");
    }
  }
  DestroyWindow(host);owner.Close();registrar.ReleaseFrames();registrar.Flush();
}
}
int wmain(int argc,wchar_t** argv){
  try{
    if(argc!=3||std::wstring(argv[1])!=L"--vectors")throw std::runtime_error("Usage: MultitapClientTrackEngineQa --vectors native-vector-directory");
    const std::filesystem::path directory(argv[2]);parseContract();
    const auto previousDpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    for(const bool gpu:{false,true}){gpuMode=gpu;suite(directory);nativeWindows(directory);}
    // The GPU path shows exactly the pixels of the CPU path, picture for picture.
    unsigned compared=0;
    for(const auto& [key,hash]:pictureHashes)if(key.rfind("cpu/",0)==0){const auto other=pictureHashes.find("gpu/"+key.substr(4));require(other!=pictureHashes.end()&&other->second==hash,"The GPU path presented other pixels than the CPU path");++compared;}
    require(compared==4,"CPU/GPU picture comparison is incomplete");
    if(previousDpi)SetThreadDpiAwarenessContext(previousDpi);
    std::cout<<"{\"nativeFrameAccessAndNotificationLease\":true,\"coalescedUiNotifications\":true,\"sharedPixelsSurviveOwnerRetirement\":true,\"physicalPresentation\":false,\"gpuPath\":\"warp\",\"gpuPixelsIdenticalToCpu\":true,\"nativeWindowsGpuAndCpu\":true}\n";
    std::cout<<"{\"kind\":\"client-track-engine-candidate\",\"checkpoints\":"<<checks<<",\"verifiedDualSequenceFrames\":32,\"mainSequenceTextureCallbacks\":32,\"retiredHeldBuffersVerified\":"<<staleBuffers
      <<",\"actualMicrosoftCpuDecoder\":true,\"flutterRegistrarSubstitute\":true,\"physicalPresentation\":false,\"realVmAcceptance\":false,\"windowsDisplaysChanged\":false,\"actualKeyboardCapture\":false,\"actualAudioPlayed\":false}"<<std::endl;return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}