#include "media.hpp"
#include "remote_track_decoder.h"
#include <iostream>
#include <array>
#include <numeric>
#include <filesystem>
#include <sstream>

namespace wire=multitap::tracks;
namespace {
unsigned checks=0;
void require(bool condition,const char* reason){++checks;if(!condition)throw std::runtime_error(reason);}
wire::Track track(uint8_t id,uint32_t epoch,uint32_t width,uint32_t height,int32_t left,int32_t top){return {id,epoch,width,height,width,height,left,top,id==0};}
const std::vector<uint8_t> initial{0,0,0,1,0x67,1,0,0,1,0x68,1,0,0,1,0x65,1};
std::vector<uint8_t> packet(uint8_t kind,uint8_t id,uint32_t topology,uint32_t epoch,uint64_t pts,const std::vector<uint8_t>& payload,bool key=false){
  std::vector<uint8_t> value;require(wire::pack({kind,uint8_t(key?1:0),id,topology,epoch,pts,payload.data(),payload.size()},value),"Packet writer rejected bounded fixture");return value;
}
wire::Packet view(const std::vector<uint8_t>& bytes){wire::Packet value;require(wire::parse(bytes.data(),bytes.size(),value),"Packet parser rejected writer output");return value;}
wire::Configuration configuration(uint32_t topology=1,uint32_t secondWidth=800,uint32_t secondHeight=600){
  return {topology,true,{track(1,topology,secondWidth,secondHeight,0,0),track(0,1,640,360,-640,-100)}};
}
void contracts(){
  auto data=packet(wire::video,1,1,1,0,initial,true);auto parsed=view(data);
  require(parsed.kind==wire::video && parsed.track==1 && parsed.topology==1 && parsed.epoch==1 && !parsed.pts && parsed.size==initial.size(),"Header round trip changed output identity");
  for(const auto size:{size_t(0),size_t(12),wire::headerBytes,data.size()-1}){wire::Packet empty;require(!wire::parse(data.data(),size,empty),"Truncated or legacy packet admitted");}
  struct Change{size_t at;uint8_t byte;};
  for(const auto change:std::array<Change,8>{{{0,'X'},{4,3},{5,2},{6,2},{7,1},{8,0},{12,0},{24,1}}}){auto bad=data;bad[change.at]=change.byte;wire::Packet sentinel;sentinel.track=77;require(!wire::parse(bad.data(),bad.size(),sentinel)&&sentinel.track==77,"Malformed packet changed parser state");}
  auto tooBig=std::vector<uint8_t>(wire::maximumPacketBytes+1);wire::Packet empty;
  require(!wire::parse(tooBig.data(),tooBig.size(),empty),"Oversized packet admitted");
  std::vector<uint8_t> unchanged{3,4,5};const auto sentinel=unchanged;
  require(!wire::pack({wire::video,0,0,1,1,0,initial.data(),wire::maximumPacketBytes-wire::headerBytes+1},unchanged) && unchanged==sentinel,"Oversized writer allocated or changed output");
  require(!wire::pack({wire::video,0,0,0,1,0,initial.data(),initial.size()},unchanged) && unchanged==sentinel,"Invalid writer changed output");
  const std::vector<uint8_t> maximum(wire::maximumPacketBytes-wire::headerBytes,0);
  auto maximumPacket=packet(wire::video,0,1,1,0,maximum);require(maximumPacket.size()==wire::maximumPacketBytes && view(maximumPacket).size==maximum.size(),"Exact wire size boundary rejected");
  auto maxPts=packet(wire::video,0,1,1,wire::maximumPts,initial,true);require(view(maxPts).pts==wire::maximumPts,"Safe integer PTS boundary lost");
  auto badPts=maxPts;wire::write64(badPts.data()+16,wire::maximumPts+1);require(!wire::parse(badPts.data(),badPts.size(),empty),"Unrepresentable client PTS admitted");
  const std::vector<uint8_t> pcm{0,1,0,2,0,3,0,4};
  auto sound=packet(wire::audio,wire::audioTrack,1,0,0,pcm);require(view(sound).track==wire::audioTrack,"Shared audio track lost");
  auto badAudio=sound;badAudio[6]=0;require(!wire::parse(badAudio.data(),badAudio.size(),empty),"Audio routed to video output");
  auto config=configuration();require(wire::valid(config),"Negative mixed geometry rejected");
  auto mixed=config;mixed.tracks={track(0,1,1920,1080,-1920,0),track(1,1,2560,1440,0,-1440)};require(wire::valid(mixed),"FHD plus QHD rejected");
  auto max=config;max.tracks={track(0,1,3840,2160,-3840,0),track(1,1,3840,2160,0,0)};require(!wire::valid(max),"Aggregate pixel budget ignored");
  auto overlap=config;overlap.tracks[1].left=1;require(!wire::valid(overlap),"Overlapping outputs admitted");
  auto duplicate=config;duplicate.tracks[1].id=1;require(!wire::valid(duplicate),"Duplicate connector identity admitted");
  auto invalid=config;invalid.tracks[0].width=801;require(!wire::valid(invalid),"Odd H264 dimensions admitted");
  wire::Gate gate;require(gate.configure(config)==wire::Configured::changed && !gate.ready(),"New metadata was considered ready");
  require(gate.accept(view(sound))==wire::Decision::awaitingAllTracks,"Audio preceded both output keys");
  auto p=packet(wire::video,0,1,1,0,std::vector<uint8_t>{0,0,1,0x41,1});require(gate.accept(view(p))==wire::Decision::awaitingKey,"First P-frame admitted");
  auto lying=packet(wire::video,0,1,1,0,std::vector<uint8_t>{0,0,1,0x41,1},true);require(gate.accept(view(lying))==wire::Decision::invalid,"Key flag substituted for SPS/PPS/IDR");
  auto first=packet(wire::video,0,1,1,0,initial,true),second=packet(wire::video,1,1,1,0,initial,true);
  require(gate.accept(view(first))==wire::Decision::accepted && !gate.ready(),"One key unlocked two outputs");
  require(gate.accept(view(second))==wire::Decision::accepted && gate.ready(),"Independent simultaneous PTS rejected");
  require(gate.accept(view(first))==wire::Decision::invalid,"Duplicate PTS accepted");
  require(gate.configure(config)==wire::Configured::unchanged && gate.ready(),"Idempotent metadata reset readiness");
  auto sameChanged=config;sameChanged.tracks[0].height=602;require(gate.configure(sameChanged)==wire::Configured::invalid && gate.ready(),"Same topology replaced geometry");
  require(gate.configure(configuration(2,960,540))==wire::Configured::changed && !gate.ready(),"New topology kept old readiness");
  require(gate.accept(view(first))==wire::Decision::stale,"Old topology was decoded");
  auto oldEpoch=packet(wire::video,1,2,1,1,initial,true);require(gate.accept(view(oldEpoch))==wire::Decision::stale,"Old output epoch was decoded");
  auto future=packet(wire::video,1,3,3,1,initial,true);require(gate.accept(view(future))==wire::Decision::invalid,"Future metadata inferred from video");
  wire::Gate secondary;require(secondary.configure({1,false,{track(1,1,640,360,-640,0)}})==wire::Configured::changed,"Only connector1 required invented connector0");
  require(secondary.accept(view(first))==wire::Decision::invalid && secondary.accept(view(second))==wire::Decision::accepted,"Unavailable connector fell back to another output");
}
using Frames=std::array<std::vector<host::Frame>,2>;
Frames encode(const wire::Configuration& config,unsigned count){
  Frames frames;
  std::array<std::unique_ptr<host::Encoder>,2> encoders;
  std::array<std::vector<uint8_t>,2> pixels;
  for(const auto& t:config.tracks){
    encoders[t.id]=std::make_unique<host::Encoder>(int(t.width),int(t.height),30,4000000);
    pixels[t.id].assign(size_t(t.width)*t.height*3/2,128);
    std::fill(pixels[t.id].begin(),pixels[t.id].begin()+size_t(t.width)*t.height,uint8_t(t.id?200:40));
  }
  // Keep both actual MFTs alive and interleave submissions. Constructing and
  // destroying one complete encoder before the other would miss shared state.
  for(unsigned n=0;n<count;++n)for(const auto& t:config.tracks){
    encoders[t.id]->encode(pixels[t.id].data(),n*1000000ULL/30,[&,id=t.id](host::Frame frame){frames[id].push_back(std::move(frame));});
  }
  for(const auto& t:config.tracks){
    encoders[t.id]->finish([&,id=t.id](host::Frame frame){frames[id].push_back(std::move(frame));});
    require(frames[t.id].size()==count && frames[t.id].front().key,"Actual MFT did not produce all independent fixture frames");
  }
  return frames;
}
std::vector<uint8_t> wrap(const host::Frame& frame,const wire::Track& track,uint32_t topology){return packet(wire::video,track.id,topology,track.epoch,frame.ptsUs,frame.bytes,frame.key);}
wire::Track selected(const wire::Configuration& config,uint8_t id){for(const auto& t:config.tracks)if(t.id==id)return t;throw std::runtime_error("Fixture track missing");}
void decodePhase(multitap::RemoteTrackDecoder& decoder,const wire::Configuration& config,const Frames& frames,
                 std::array<unsigned,2>& decoded,std::array<unsigned,2>& live,unsigned& audioPackets,bool alreadyConfigured=false){
  require(decoder.Configure(config)==(alreadyConfigured?S_FALSE:S_OK) && !decoder.ready(),"Decoder metadata was considered actual pixels");
  std::array<uint64_t,2> last{};std::array<bool,2> seen{};
  auto receive=[&](uint8_t id,uint32_t topology,uint32_t epoch,multitap::DecodedFrame&& frame){
    const auto t=selected(config,id);
    require(id<2 && topology==config.topology && epoch==t.epoch && frame.width==t.width && frame.height==t.height,"Actual output delivered to wrong track or generation");
    require(!seen[id] || frame.pts_us>last[id],"Decoder PTS crossed track order");seen[id]=true;last[id]=frame.pts_us;
    const auto center=(size_t(frame.height/2)*frame.width+frame.width/2)*4;
    require(frame.rgba.size()==size_t(frame.width)*frame.height*4 && (id?frame.rgba[center]>170:frame.rgba[center]<65),"Independent output pixels were exchanged");
    require(decoder.Configure(config)==E_PENDING,"Callback reentered decoder replacement");++decoded[id];
  };
  auto audio=[&](const uint8_t* bytes,size_t size,uint64_t pts){require(size==8 && bytes[1]==1 && pts==1,"Shared PCM changed");++audioPackets;return S_OK;};
  auto sound=packet(wire::audio,wire::audioTrack,config.topology,0,1,{0,1,0,2,0,3,0,4});
  wire::Decision decision;require(decoder.Push(sound.data(),sound.size(),receive,audio,&decision)==S_FALSE && decision==wire::Decision::awaitingAllTracks,"Audio emitted before actual decoded output");
  for(size_t n=0;n<frames[0].size();++n)for(const uint8_t id:{uint8_t(1),uint8_t(0)}){
    auto bytes=wrap(frames[id][n],selected(config,id),config.topology);
    host::check(decoder.Push(bytes.data(),bytes.size(),receive,audio),"Actual dual packet decode failed");
    if(n==0 && id==1)require(!decoder.ready() && decoder.Push(sound.data(),sound.size(),receive,audio)==S_FALSE,"First actual output unlocked the missing second output");
  }
  live=decoded;require(live[0]>0 && live[1]>0 && decoder.ready(),"Outputs only appeared after end-of-stream drain");
  require(decoder.Push(sound.data(),sound.size(),receive,audio)==S_OK && audioPackets>0,"Shared audio did not unlock after both decoded outputs");
  host::check(decoder.Drain(receive),"Independent decoder drain failed");
  require(decoded[0]==frames[0].size() && decoded[1]==frames[1].size(),"Independent decoder lost actual MFT frames");
  auto afterEnd=wrap(frames[0].back(),selected(config,0),config.topology);
  require(!decoder.ready() && decoder.Push(afterEnd.data(),afterEnd.size(),receive,audio)==E_UNEXPECTED && decoder.Drain(receive)==S_FALSE,"Ended decoder accepted live media or drained twice");
}
void writeNew(const std::filesystem::path& path,const uint8_t* bytes,size_t size){
  const HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  require(file!=INVALID_HANDLE_VALUE,"Refuse to overwrite native fixture output");
  DWORD written=0;const bool valid=size<=MAXDWORD && WriteFile(file,bytes,static_cast<DWORD>(size),&written,nullptr) &&
    written==size && FlushFileBuffers(file);
  const bool closed=CloseHandle(file)!=FALSE;require(valid && closed,"Native fixture output was not durable");
}
unsigned exportFixtures(const std::filesystem::path& directory,const wire::Configuration& first,const Frames& frames,
                        const wire::Configuration& next,const Frames& resized){
  const auto attributes=GetFileAttributesW(directory.c_str());
  require(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY) && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
    std::filesystem::is_empty(directory),"Fixture output must be an existing empty ordinary directory");
  std::ostringstream manifest;
  manifest<<"{\"format\":\"multitap-native-track-fixture-v1\",\"actualMicrosoftCpuEncoder\":true,\"syntheticPixels\":true,\"windowsDisplaysChanged\":false,\"frames\":[";
  unsigned count=0;
  for(const auto& phase:std::array<std::pair<const wire::Configuration*,const Frames*>,2>{{{&first,&frames},{&next,&resized}}}){
    const auto& config=*phase.first;const auto& encoded=*phase.second;
    for(size_t n=0;n<encoded[0].size();++n)for(const uint8_t id:{uint8_t(1),uint8_t(0)}){
      const auto track=selected(config,id);const auto& frame=encoded[id][n];const auto bytes=wrap(frame,track,config.topology);
      const auto name="topology-"+std::to_string(config.topology)+"-track-"+std::to_string(id)+"-frame-"+std::to_string(n)+".tvt1";
      writeNew(directory/name,bytes.data(),bytes.size());
      if(count++)manifest<<",";
      manifest<<"{\"file\":\""<<name<<"\",\"track\":\""<<unsigned(id)<<"\",\"topologyEpoch\":"<<config.topology<<",\"epoch\":"<<track.epoch
        <<",\"width\":"<<track.width<<",\"height\":"<<track.height<<",\"ptsUs\":"<<frame.ptsUs<<",\"key\":"<<(frame.key?"true":"false")<<"}";
    }
  }
  manifest<<"]}";const auto json=manifest.str();writeNew(directory/"manifest.json",reinterpret_cast<const uint8_t*>(json.data()),json.size());return count;
}
}
int wmain(int argc,wchar_t** argv){
  try{
    const bool exportRequested=argc==3 && std::wstring(argv[1])==L"--fixture-output";
    if(argc!=1 && !exportRequested)throw std::runtime_error("Usage: MultitapTrackMediaQa [--fixture-output existing-empty-directory]");
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    contracts();const auto contractChecks=checks;
    host::Runtime runtime;
    auto first=configuration(),next=configuration(2,960,540);
    const auto frames=encode(first,8),resized=encode(next,8);
    multitap::RemoteTrackDecoder decoder;
    std::array<unsigned,2> decoded{},live{};unsigned audio=0;
    decodePhase(decoder,first,frames,decoded,live,audio);
    const auto before=decoded;const auto old=wrap(frames[0][0],selected(first,0),1);
    require(decoder.Configure(next)==S_OK && !decoder.ready(),"Epoch replacement kept decoder pixels ready");
    wire::Decision decision;require(decoder.Push(old.data(),old.size(),[](uint8_t,uint32_t,uint32_t,multitap::DecodedFrame&&){throw std::runtime_error("Stale callback");},{},&decision)==S_FALSE && decision==wire::Decision::stale,"Stale packet crossed real decoder reconfiguration");
    decoded={};std::array<unsigned,2> nextLive{};decodePhase(decoder,next,resized,decoded,nextLive,audio,true);
    std::unique_ptr<multitap::RemoteTrackDecoder> owner=std::make_unique<multitap::RemoteTrackDecoder>();
    require(owner->Configure({1,false,{selected(first,0)}})==S_OK,"Deleting callback setup failed");
    unsigned deleted=0;
    for(const auto& frame:frames[0]){
      if(!owner)break;const auto bytes=wrap(frame,selected(first,0),1);
      const HRESULT hr=owner->Push(bytes.data(),bytes.size(),[&](uint8_t,uint32_t,uint32_t,multitap::DecodedFrame&&){owner.reset();++deleted;});
      require(SUCCEEDED(hr)||hr==E_ABORT,"Deleting callback failed before retirement");
    }
    require(!owner && deleted==1,"Decoder owner could not retire inside output callback");
    {
      multitap::RemoteTrackDecoder invalid;
      require(invalid.Drain([](uint8_t,uint32_t,uint32_t,multitap::DecodedFrame&&){})==E_UNEXPECTED,"Unconfigured decoder drained successfully");
      require(invalid.Configure(first)==S_OK,"Invalid-packet fixture setup failed");
      auto bad=wrap(frames[0][0],selected(first,0),1);bad[7]=1;
      require(invalid.Push(bad.data(),bad.size(),{})==E_INVALIDARG && !invalid.ready(),"Malformed wire did not latch failure");
      auto good=wrap(frames[0][0],selected(first,0),1);
      require(invalid.Push(good.data(),good.size(),{})==E_INVALIDARG,"Failed decoder silently resumed");
      require(invalid.Configure(next)==S_OK && !invalid.ready(),"Fresh authorized metadata could not replace failed decoder");
      invalid.Close();require(invalid.Configure(configuration(3))==E_ABORT,"Closed decoder resurrected");
    }
    {
      multitap::RemoteTrackDecoder throwing;
      require(throwing.Configure({1,false,{selected(first,0)}})==S_OK,"Throwing callback setup failed");
      HRESULT last=S_OK;
      for(const auto& frame:frames[0]){
        auto bytes=wrap(frame,selected(first,0),1);
        last=throwing.Push(bytes.data(),bytes.size(),[](uint8_t,uint32_t,uint32_t,multitap::DecodedFrame&&){throw std::runtime_error("injected frame consumer failure");});
        if(FAILED(last))break;
      }
      require(last==E_FAIL && !throwing.ready(),"Consumer exception escaped or kept a ready decoder");
    }
    const unsigned fixtureCount=exportRequested?exportFixtures(std::filesystem::path(argv[2]),first,frames,next,resized):0;
    std::cout<<"{\"kind\":\"dual-track-codec-candidate\",\"contractChecks\":"<<contractChecks<<",\"totalChecks\":"<<checks<<",\"firstDecoded\":["<<before[0]<<","<<before[1]<<"],\"firstLiveBeforeDrain\":["<<live[0]<<","<<live[1]<<"],\"resizedDecoded\":["<<decoded[0]<<","<<decoded[1]<<"],\"resizedLiveBeforeDrain\":["<<nextLive[0]<<","<<nextLive[1]<<"],\"sharedAudioCallbacks\":"<<audio<<",\"deletedOwnerInCallback\":"<<deleted<<",\"fixturePacketsWritten\":"<<fixtureCount<<",\"actualMicrosoftCpuEncodersAndDecoders\":true,\"syntheticPixels\":true,\"windowsDisplaysChanged\":false,\"publicRelayUsed\":false,\"localWindowsPresented\":false,\"realVmAcceptance\":false}"<<std::endl;
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
