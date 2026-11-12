#include "encoder_diagnostics.hpp"
#include "encoder_settings_policy.hpp"
#include <iostream>
#include <limits>
#include <map>
#include <utility>
using namespace host;
namespace {int checks=0,failures=0;void check(bool value,const char* label){++checks;if(!value){++failures;std::cerr<<"FAIL "<<label<<'\n';}}
struct Info{bool initialized=true,hardwareVerified=false;uint64_t adapterLuid=0;uint32_t vendorId=0,deviceId=0,gpuFallbackCode=0;std::string adapterName;EncoderGpuFailure gpuFailure;};
// Memory-only JSON value for the production templated private parser.
struct DiagnosticJson {
 using Object=std::map<std::string,DiagnosticJson>;
 std::variant<std::nullptr_t,bool,double,std::string,Object> value=nullptr;
 DiagnosticJson()=default;DiagnosticJson(std::nullptr_t):value(nullptr){}DiagnosticJson(bool v):value(v){}DiagnosticJson(double v):value(v){}DiagnosticJson(const char* v):value(std::string(v)){}DiagnosticJson(std::string v):value(std::move(v)){}DiagnosticJson(Object v):value(std::move(v)){}
 const DiagnosticJson& get(const char* key)const{if(const auto* object=std::get_if<Object>(&value))if(const auto found=object->find(key);found!=object->end())return found->second;static const DiagnosticJson absent;return absent;}
 std::string string()const{if(const auto* text=std::get_if<std::string>(&value))return *text;return {};}
 double number(double fallback=0)const{if(const auto* n=std::get_if<double>(&value))return *n;return fallback;}
};
}
int main(){
 Info hardware;hardware.adapterLuid=0xfedcba9876543210ULL;hardware.vendorId=0x10de;hardware.deviceId=0x2684;hardware.adapterName="Fixture NVIDIA adapter";
 auto pending=encoderDiagnostic("nvenc",hardware,0);check(encoderDiagnosticValid(pending)&&!pending.hardwareVerified,"initialization is not first output verification");
 hardware.hardwareVerified=true;auto active=encoderDiagnostic("nvenc",hardware,0);check(encoderDiagnosticValid(active)&&active.hardwareVerified,"actual output snapshot can identify hardware");
 const auto encoded=encoderDiagnosticJson(active);check(encoded.find("fedcba98:76543210")!=std::string::npos,"full64bit LUID is hex not JSON double");
 check(gpu::parseAdapterLuid(gpu::adapterLuidText(UINT64_MAX))==UINT64_MAX,"UINT64 max roundtrip without precision loss");
 auto cpu=encoderDiagnostic("x264",hardware,0);check(!cpu.hardwareVerified&&!cpu.adapterLuid&&!cpu.vendorId&&cpu.adapterName.empty(),"CPU fallback cannot retain old GPU identity");
 hardware.gpuFallbackCode=43;cpu=encoderDiagnostic("x264",hardware,0);check(cpu.gpuFallbackCode==43&&encoderDiagnosticValid(cpu),"fixed device-lost fallback survives CPU transition");
 auto invalid=active;invalid.backend="mft";check(!encoderDiagnosticValid(invalid),"software cannot claim GPU hardware");
 invalid=active;invalid.initialized=false;check(!encoderDiagnosticValid(invalid),"first-output proof requires initialization");
 invalid=active;invalid.adapterLuid=0;check(!encoderDiagnosticValid(invalid),"hardware proof needs selected actual adapter");
 invalid=active;invalid.vendorId=0x1002;check(!encoderDiagnosticValid(invalid),"NVIDIA backend cannot claim AMD adapter");
 invalid=active;invalid.gpuFallbackCode=31;check(!encoderDiagnosticValid(invalid),"unrecognized fallback code rejected");
 check(!encoderAdapterName(std::string("bad\nname")),"control name rejected");
 check(!encoderAdapterName(std::string("\xc0\xaf",2)),"overlong UTF8 name rejected");
 check(encoderAdapterName("Fixture GPU"),"bounded adapter name retained");
 EncoderDiagnosticCache cache;const auto first=cache.begin();check(cache.observe(first,{{"0",1,active}},100),"current worker snapshot accepted");
 check(cache.json(5100).find("\"observed\":true")!=std::string::npos,"bounded fresh snapshot shown");
 check(cache.json(5101).find("\"observed\":false")!=std::string::npos,"stale snapshot becomes unknown");
 const auto second=cache.begin();check(!cache.observe(first,{{"0",1,active}},101),"old worker cannot publish into new lifetime");
 check(cache.observe(second,{{"0",2,cpu},{"1",2,active}},102),"mixed CPU GPU tracks remain distinct");
 const auto mixed=cache.json(103);check(mixed.find("x264")!=std::string::npos&&mixed.find("nvenc")!=std::string::npos,"mixed snapshot is not one invented GPU");
 cache.retire(first);check(cache.json(103).find("\"observed\":true")!=std::string::npos,"late old exit cannot clear new snapshot");
 cache.invalidate();check(cache.json(103).find("\"observed\":false")!=std::string::npos,"resize withdraws previous encoder snapshot");
 check(!cache.observe(second,{{"0",2,cpu},{"0",2,active}},104),"duplicate track IDs rejected");
 cache.retire();check(!cache.observe(second,{{"0",2,active}},105),"stop rejects delayed same-worker update");
 EncoderUsageJournalPolicy usage;std::vector<std::string> journal;
 const auto record=[&](uint64_t current,uint64_t ticket,const EncoderDiagnosticTrack& track,bool output){if(usage.observe(current,ticket,track,output))journal.push_back(encoderDiagnosticJson(track.info));};
 record(second,first,{"0",1,active},true);check(journal.empty(),"historical worker cannot record GPU use in current session");
 record(second,second,{"0",2,pending},true);check(journal.empty(),"GPU initialization without verified first H264 cannot enter usage journal");
 record(second,second,{"0",2,active},false);check(journal.empty(),"zero encoded output cannot become GPU usage evidence");
 record(second,second,{"0",2,active},true);record(second,second,{"0",2,active},true);check(journal.size()==1,"verified current epoch records once instead of every stats tick");
 cache.retire();check(cache.json(9000).find("\"observed\":false")!=std::string::npos&&journal.size()==1&&journal.back().find("nvenc")!=std::string::npos,"live snapshot expiry and disconnect must not erase protected last-output journal");
 record(0,second,{"0",2,cpu},true);check(journal.size()==1,"late disconnected CPU observation cannot replace last actual GPU record");
 record(second+1,second+1,{"0",1,cpu},true);check(journal.size()==2&&journal.back().find("\"hardwareVerified\":false")!=std::string::npos,"new current CPU output replaces historical GPU without stale identity");
 check(usage.observe(second+1,second+1,{"1",1,active},true),"separate dual track GPU output is not collapsed into the other track CPU record");
 const std::string usageSession="12345678-1234-4234-8234-123456789abc";
 EncoderUsageObservation outputUsage{usageSession,5,{"1",2,active}};std::vector<EncoderDiagnosticTrack> expectedUsage{{"0",2,cpu},{"1",2,active}};
 check(!encoderUsagePacket(outputUsage).empty()&&encoderUsageMatches(outputUsage,usageSession,5,expectedUsage),"actual dual output private packet has exact current track identity");
 auto oldUsage=outputUsage;oldUsage.track.epoch=1;check(!encoderUsageMatches(oldUsage,usageSession,5,expectedUsage),"late previous track epoch cannot become current output usage");
 oldUsage=outputUsage;oldUsage.topology=4;check(!encoderUsageMatches(oldUsage,usageSession,5,expectedUsage),"late previous topology cannot become current output usage");
 check(!encoderUsageMatches(outputUsage,"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",5,expectedUsage),"old authenticated session cannot reuse a new session ticket");
 oldUsage=outputUsage;oldUsage.track.info=pending;check(encoderUsagePacket(oldUsage).empty(),"GPU initialization without actual verified H264 cannot enter private event");
 EncoderUsageObservation legacyUsage{usageSession,0,{"0",0,cpu}};std::vector<EncoderDiagnosticTrack> expectedLegacy{{"0",0,{}}};check(!encoderUsagePacket(legacyUsage).empty()&&encoderUsageMatches(legacyUsage,usageSession,0,expectedLegacy),"legacy single epoch0 output remains supported after negotiated sessions");
 legacyUsage.track.id="1";check(encoderUsagePacket(legacyUsage).empty(),"single output event cannot invent a second track");

 // Exact private6 legacy and private7 detail; the released encoder JSON stays10.
 Info rawFailure;rawFailure.gpuFallbackCode=42;rawFailure.gpuFailure={112,UINT32_MAX,0x10de,0x2c02};
 auto failureCpu=encoderDiagnostic("x264",rawFailure,0),publicCpu=failureCpu;publicCpu.gpuFailure={};
 check(failureCpu.gpuFailure==rawFailure.gpuFailure,"immutable CPU output keeps separate failed GPU detail");
 check(encoderDiagnosticJson(failureCpu)==encoderDiagnosticJson(publicCpu)&&encoderDiagnosticJson(failureCpu).find("gpuFailure")==std::string::npos,"private detail never changes released ten-field encoder JSON");
 using J=DiagnosticJson;
 const auto failureFields=J::Object{{"site",J(112.0)},{"nativeStatus",J(double(UINT32_MAX))},{"vendorId",J(double(0x10de))},{"deviceId",J(double(0x2c02))}};
 const auto packetValue=[&](bool detail){
  J info(J::Object{{"protocol",J("h264-encoder-v1")},{"backend",J("x264")},{"initialized",J(true)},{"hardwareVerified",J(false)},{"adapterLuid",J(nullptr)},{"vendorId",J(nullptr)},{"deviceId",J(nullptr)},{"adapterName",J(nullptr)},{"gpuFallbackCode",J(42.0)},{"softwareFallbackCode",J(0.0)}});
  J::Object object{{"type",J("worker.encoder.usage")},{"sessionId",J(usageSession)},{"topologyEpoch",J(0.0)},{"id",J("0")},{"epoch",J(0.0)},{"encoderInfo",std::move(info)}};
  if(detail)object.emplace("gpuFailure",J(failureFields));return J(std::move(object));
 };
 const auto legacyPrivate=parseEncoderUsagePacket(packetValue(false));
 check(legacyPrivate.has_value()&&encoderUsagePacket(*legacyPrivate).find("gpuFailure")==std::string::npos,"exact six-field private legacy output remains valid");
 const auto detailedPrivate=parseEncoderUsagePacket(packetValue(true));
 check(detailedPrivate.has_value()&&detailedPrivate->track.info.gpuFailure.nativeStatus==UINT32_MAX,"exact seven-field private packet retains raw unsigned32 status");
 check(encoderUsagePacket({usageSession,0,{"0",0,failureCpu}}).find("gpuFailure")!=std::string::npos,"known CPU fallback output attaches optional private detail");
 auto unknownFailure=failureCpu;unknownFailure.gpuFailure.vendorId=0;
 const auto unknownPacket=encoderUsagePacket({usageSession,0,{"0",0,unknownFailure}});
 check(!unknownPacket.empty()&&unknownPacket.find("gpuFailure")==std::string::npos,"unknown candidate detail cannot erase legacy actual CPU output");
 auto currentGpu=active;currentGpu.gpuFailure=failureCpu.gpuFailure;
 const auto currentGpuPacket=encoderUsagePacket({usageSession,0,{"0",0,currentGpu}});
 check(!currentGpuPacket.empty()&&currentGpuPacket.find("gpuFailure")==std::string::npos,"current successful GPU cannot publish old candidate failure as CPU fallback");
 const auto projectedFailure=encoderGpuFailureJournalRecord({usageSession,0,{"0",0,failureCpu}});
 check(projectedFailure&&projectedFailure->site==112&&projectedFailure->nativeStatus==UINT32_MAX&&projectedFailure->vendorId==0x10de&&projectedFailure->deviceId==0x2c02&&projectedFailure->gpuFallbackCode==42&&projectedFailure->cpuBackend==1&&projectedFailure->track==0&&projectedFailure->epoch==0,"journal projection has fixed site/status/vendor/device and actual CPU backend/track epoch");
 auto mftFailure=failureCpu;mftFailure.backend="mft";
 const auto projectedMft=encoderGpuFailureJournalRecord({usageSession,0,{"0",0,mftFailure}});check(projectedMft&&projectedMft->cpuBackend==2,"software MFT fallback is distinct from actual x264 output");
 check(!encoderGpuFailureJournalRecord({usageSession,0,{"0",0,currentGpu}})&&!encoderGpuFailureJournalRecord({usageSession,0,{"0",0,unknownFailure}}),"active GPU and unknown identity do not invent a failure journal record");
 EncoderUsageJournalPolicy detailUsage;const EncoderDiagnosticTrack detailTrack{"0",0,failureCpu};
 check(!detailUsage.observe(second,first,detailTrack,true)&&!detailUsage.observe(second,second,detailTrack,false),"old ticket or absent actual output cannot admit failure details");
 auto notOutput=detailTrack;notOutput.info.initialized=false;check(!detailUsage.observe(second,second,notOutput,true),"CPU initialization alone is not fallback-output evidence");
 check(detailUsage.observe(second,second,detailTrack,true)&&!detailUsage.observe(second,second,detailTrack,true),"same valid detailed failure is deduplicated");
 auto different=detailTrack;different.info.gpuFailure.nativeStatus=7;check(detailUsage.observe(second,second,different,true),"different native status in same ticket epoch backend is preserved");
 different.info.gpuFailure.site=113;check(detailUsage.observe(second,second,different,true),"different failure site is not erased by public ten-field dedupe key");
 different.info.gpuFailure={9,7,0x1002,0x164e};check(detailUsage.observe(second,second,different,true),"different known candidate vendor in common site is preserved");
 EncoderUsageJournalPolicy oldKey;check(oldKey.observe(second,second,{"0",0,publicCpu},true)&&!oldKey.observe(second,second,{"0",0,unknownFailure},true),"absent or unknown optional detail retains the exact legacy dedupe key");
 for(unsigned scenario=0;scenario<19;++scenario){auto hostile=packetValue(true);auto& top=std::get<J::Object>(hostile.value);auto& detail=std::get<J::Object>(top["gpuFailure"].value);
  switch(scenario){case 0:top["exception"]=J("forged");break;case 1:top.erase("epoch");break;case 2:detail["extra"]=J(1.0);break;case 3:detail.erase("deviceId");break;case 4:detail["site"]=J("112");break;case 5:detail["site"]=J(-1.0);break;case 6:detail["site"]=J(112.5);break;case 7:detail["site"]=J(120.0);break;case 8:detail["nativeStatus"]=J(4294967296.0);break;case 9:detail["nativeStatus"]=J(-1.0);break;case 10:detail["nativeStatus"]=J(std::numeric_limits<double>::quiet_NaN());break;case 11:detail["nativeStatus"]=J(true);break;case 12:detail["vendorId"]=J(double(0x1002));break;case 13:detail["vendorId"]=J(0.0);break;case 14:detail["deviceId"]=J(4294967296.0);break;case 15:std::get<J::Object>(top["encoderInfo"].value)["gpuFallbackCode"]=J(0.0);break;case 16:std::get<J::Object>(top["encoderInfo"].value)["initialized"]=J(false);break;case 17:top["gpuFailure"]=J(nullptr);break;case 18:detail["site"]=J(320.0);break;}
  check(!parseEncoderUsagePacket(hostile),"invalid optional private detail rejects the entire new packet");
 }
 auto unknownSite=packetValue(true);std::get<J::Object>(std::get<J::Object>(unknownSite.value)["gpuFailure"].value)["site"]=J(0.0);
 check(parseEncoderUsagePacket(unknownSite).has_value(),"unknown site0 is allowed only with a known candidate vendor");
 for(const auto family:std::array<std::pair<uint32_t,uint32_t>,3>{{{112,0x10de},{200,0x1002},{300,0x8086}}}){check(encoderGpuFailureDetailKnown({family.first,0,family.second,1}),"fixed provider site family matches its known vendor");check(!encoderGpuFailureDetailKnown({family.first,0,family.second==0x10de?0x1002u:0x10deu,1}),"cross-vendor provider site rejected");}
 if(detailedPrivate){auto late=*detailedPrivate;check(!encoderUsageMatches(late,"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",0,expectedLegacy),"detailed late foreign session cannot enter current journal");late.topology=1;check(!encoderUsageMatches(late,usageSession,0,expectedLegacy),"detailed late topology cannot enter current journal");late.topology=0;late.track.epoch=1;check(!encoderUsageMatches(late,usageSession,0,expectedLegacy),"detailed old epoch cannot enter current journal");}

 EncoderDiagnosticCache exhausted(UINT64_MAX);check(exhausted.begin()==0,"ticket generation overflow fails closed");
 check(appendEncoderDiagnostic("{}",cache.json(100)).find("encoder")!=std::string::npos,"optional Host diagnostic appended");
 check(appendEncoderDiagnostic(std::string(6143,'x')+'}',cache.json(100))==std::string(6143,'x')+'}',"optional encoder never discards bounded base diagnostic");

 EncoderSettingsChoice configured;configured.revision=std::string(64,'a');
 EncoderSettingsRequest choice;choice.operationId="12345678-1234-4234-8234-123456789abc";choice.expectedRevision=configured.revision;choice.backend="gpu";choice.adapter=0xfedcba9876543210ULL;
 check(std::string(encoderSettingsPlan(configured,choice,{*choice.adapter}))=="confirmation_required","inventory does not replace explicit consent");
 choice.confirmed=true;check(std::string(encoderSettingsPlan(configured,choice,{*choice.adapter}))=="apply","explicit current candidate can be saved as request only");
 check(std::string(encoderSettingsPlan(configured,choice,{}))=="encoder_adapter_changed","missing exact adapter never chooses another");
 auto stale=choice;stale.expectedRevision=std::string(64,'b');check(std::string(encoderSettingsPlan(configured,stale,{*choice.adapter}))=="encoder_settings_changed","stale revision cannot overwrite current selection");
 auto zero=choice;zero.adapter=0;check(std::string(encoderSettingsRequestError(zero))=="encoder_settings_invalid_request","zero adapter cannot become GPU choice");
 auto cpuChoice=choice;cpuChoice.backend="x264";check(std::string(encoderSettingsRequestError(cpuChoice))=="encoder_settings_invalid_request","CPU cannot keep stale GPU identity");
 cpuChoice.adapter.reset();check(std::string(encoderSettingsPlan(configured,cpuChoice,{}))=="apply","CPU remains usable without GPU inventory");
 auto autoChoice=cpuChoice;autoChoice.backend="auto";check(std::string(encoderSettingsPlan(configured,autoChoice,{}))=="apply","explicit automatic request is saveable without pretending GPU inventory support");
 autoChoice.adapter=5;check(std::string(encoderSettingsRequestError(autoChoice))=="encoder_settings_invalid_request","automatic request cannot silently retain a manual GPU identity");
 auto unknown=choice;unknown.backend="automatic-ready";check(std::string(encoderSettingsRequestError(unknown))=="encoder_settings_invalid_request","unknown backend is not invented support");
 auto malformed=choice;malformed.operationId="ABC";check(std::string(encoderSettingsRequestError(malformed))=="encoder_settings_invalid_request","operation UUID required before mutation");
 configured.backend=choice.backend;configured.adapter=choice.adapter;configured.operationId=choice.operationId;configured.revision=std::string(64,'c');
 check(std::string(encoderSettingsPlan(configured,choice,{*choice.adapter}))=="duplicate","same owned UUID and desired selection is idempotent");
 auto replay=choice;replay.backend="mft";replay.adapter.reset();check(std::string(encoderSettingsPlan(configured,replay,{}))=="encoder_settings_invalid_request","UUID cannot be reused for opposite selection");
 std::cout<<"{\"kind\":\"encoder-diagnostics-memory\",\"checks\":"<<checks<<",\"failures\":"<<failures<<",\"nativeCalls\":false}\n";return failures?1:0;
}
