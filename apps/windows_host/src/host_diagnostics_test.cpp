#include "host_diagnostics.hpp"
#include "host_diagnostic_schedule.hpp"
#include <iostream>
using namespace host;
namespace {
unsigned cases=0;
void require(bool value,const char* reason){++cases;if(!value)throw std::runtime_error(reason);}
}
int wmain(int argc,wchar_t** argv){try{
  HostDiagnosticSchedule schedule;
  schedule.Request();require(!schedule.Claim(1000),"Old/unacknowledged broker enabled automatic transmission");
  schedule.Negotiate(true);require(schedule.Claim(1000),"Explicit negotiation did not schedule initial diagnostics");
  schedule.Negotiate(true);require(!schedule.Claim(5999),"Repeated capability ACK bypassed the report rate limit");
  schedule.Request();require(!schedule.Claim(5999)&&schedule.Claim(6000),"A remote request was lost during the rate limit");
  require(!schedule.Claim(35999)&&schedule.Claim(36000),"Periodic diagnostics did not follow the thirty-second cadence");
  require(!schedule.Claim(1000),"Monotonic-clock rollback bypassed the report rate limit");
  schedule.Negotiate(false);schedule.Request();require(!schedule.Claim(100000),"Unsupported protocol continued transmitting");
  schedule.Reset();schedule.Negotiate(true);require(schedule.Claim(2000),"A new connection retained the previous generation's rate-limit clock");
  DisplayState display;display.installed=display.deviceReady=display.owned=true;display.recoveryRequired=display.configurationPending=true;
  display.monitors={{"0",L"DO_NOT_EXPORT_GDI_NAME",1920,0,1920,1080,false},{"1",L"DO_NOT_EXPORT_DEVICE_NAME",3840,-120,1280,720,false}};display.width=3200;display.height=1200;
  auto native=JsonParser::parse(R"({"schema":1,"installed":true,"started":true,"leaseActive":false,"activationVerified":false,"configurationPending":true,"recoveryRequired":true,"countControlAvailable":false,"phase":"recovery","failureReason":"controller_start_failed","deviceInstanceId":"DO_NOT_EXPORT_DEVICE","owner":"DO_NOT_EXPORT_OWNER","lease":"DO_NOT_EXPORT_TOKEN"})");
  std::get<Json::Object>(native.value)["activation"]=JsonParser::parse(nativeActivationDiagnosticJson({"native_topology_validate_failed",ERROR_BAD_CONFIGURATION,false,false}));std::get<Json::Object>(native.value)["activationCurrent"].value=false;
  std::string eventBytes="[";
  for(unsigned i=0;i<20;++i){if(i)eventBytes+=',';eventBytes+="{\"time\":"+jsonQuote(hostLogTime())+",\"event\":\"displayObserved\",\"values\":{\"monitors\":"+std::to_string(i%3)+",\"installed\":true,\"active\":false,\"token\":\"DO_NOT_EXPORT_TOKEN\",\"error\":\"DO_NOT_EXPORT_ERROR\"}}";}
  eventBytes+=']';const auto events=JsonParser::parse(eventBytes);
  const auto consoleRecovery=JsonParser::parse(R"({"reason":"rdp_session_disconnected","ready":false,"eligible":true,"complete":true,"configuredIdentity":true,"consoleIdentityKnown":true,"consoleOccupied":false,"activeRdpSessions":0,"disconnectedRdpSessions":1,"disconnectedConsoleSessions":1,"unknownSessions":0,"lockedSessions":1})");
  // Optional profile-recovery explanation (fixed vocabulary).
  const auto recovery=JsonParser::parse(R"({"path":"retirement","blocker":"started","quiescent":"none","retirement":"console_not_primary","worker":"retire_rejected","hold":"profile_not_restored","ledger":"settled_shape","records":{"profile":"present","launch":"present","runtime":"present","identity":"present","baseline":"absent","viewport":"absent","reclaim":"absent","retirement":"absent","expiredConsole":"absent"},"profilePhase":"recovery","profileResize":true,"profileStopRequested":true,"profileLaunchBound":true,"runtimePhase":"recovery","runtimeCheckpoint":false,"runtimeMatchesProfile":true,"launchMatchesProfile":true,"launchService":"gone","launchWorker":"gone","logon":"new","identityMatchesLaunch":true,"retirementPhase":"none","expiredConsolePhase":"none","attempts":3,"budgetExhausted":true,"lastAttemptAgeMs":4200,"operation":"DO_NOT_EXPORT_OPERATION"})");
  const auto report=buildHostDiagnosticReport(display,true,true,false,"native_topology_recovery_required",native,events,true,0,consoleRecovery,recovery);
  std::string readinessReport;
  const auto parsed=JsonParser::parse(report);
  require(parsed.get("native").get("recovery").get("retirement").string()=="console_not_primary"&&parsed.get("native").get("recovery").get("records").get("profile").string()=="present"&&std::holds_alternative<double>(parsed.get("native").get("recovery").get("lastAttemptAgeMs").value),"Profile-recovery explanation was not projected");
  for(int scenario=0;scenario<4;++scenario){auto hostile=recovery;auto& object=std::get<Json::Object>(hostile.value);if(scenario==0)object["blocker"].value=std::string("DO_NOT_EXPORT_EXCEPTION");if(scenario==1)object["records"].value=Json::Object{};if(scenario==2)object["attempts"].value=-1.0;if(scenario==3)object.erase("ledger");const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,events,true,0,consoleRecovery,hostile);require(bytes.find("DO_NOT_EXPORT")==std::string::npos&&std::holds_alternative<std::nullptr_t>(JsonParser::parse(bytes).get("native").get("recovery").value)&&JsonParser::parse(bytes).get("native").get("phase").string()=="recovery","Hostile recovery explanation escaped or discarded the base report");}
  require(report.size()<=6144,"Production diagnostic exceeded the negotiated control budget");
  {
    // Admission failures must survive ordinary observations long enough to
    // distinguish worker retirement, maintenance and actual display failure.
    Json history;auto& rows=history.value.emplace<Json::Array>();
    const auto failed=[&](unsigned action,unsigned stage,const char* time){
      rows.push_back(JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":"+std::to_string(action)+",\"stage\":"+std::to_string(stage)+",\"ok\":false,\"error\":13,\"path\":\"DO_NOT_EXPORT_PATH\"}}"));
    };
    failed(253,5,"2026-10-28T09:00:00.000Z");
    failed(253,7,"2026-10-28T09:00:01.000Z");
    failed(220,3,"2026-10-28T09:00:02.000Z");
    failed(220,4,"2026-10-28T09:00:03.000Z");
    failed(220,5,"2026-10-28T09:00:04.000Z");
    failed(210,7,"2026-10-28T09:00:05.000Z");
    failed(200,107,"2026-10-28T09:00:06.000Z");
    failed(214,7,"2026-10-28T09:00:07.000Z");
    for(unsigned i=0;i<30;++i)rows.push_back(std::get<Json::Array>(events.value)[i%20]);
    const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,history,true,0,consoleRecovery,recovery);
    const auto retained=JsonParser::parse(bytes).get("events");
    bool worker=false,maintenance=false,preparation=false,nativeFailure=false,restore=false;unsigned clipboard=0,observations=0;
    for(const auto& row:std::get<Json::Array>(retained.value)){
      const auto& values=row.get("values");const auto action=values.get("action").number();const auto stage=values.get("stage").number();
      worker|=action==220&&stage==3;maintenance|=action==220&&stage==5;
      preparation|=action==200&&stage==107;nativeFailure|=action==210&&stage==7;restore|=action==214&&stage==7;
      if(action==253)++clipboard;if(row.get("event").string()=="displayObserved")++observations;
      if(action==214)require(row.get("time").string()=="2026-10-28T09:00:07.000Z","Historical failure timestamp was replaced by the collection time");
    }
    require(worker&&maintenance&&preparation&&nativeFailure&&restore,"Preparation/retirement/restore causes were erased by success spam");
    require(clipboard==2&&observations==1&&std::get<Json::Array>(retained.value).size()==8,"Failure history displaced clipboard evidence or the newest observation");
    require(bytes.size()<=6144&&bytes.find("DO_NOT_EXPORT")==std::string::npos,"Admission evidence escaped the diagnostic privacy/size bounds");
    require(JsonParser::parse(bytes).get("display").get("reason").string()=="viewport_probe_failed","Historical failures became current readiness authority");
    readinessReport=bytes;
    // A malformed latest failure cannot hide the older, valid category.
    auto malformed=rows[2];auto& object=std::get<Json::Object>(malformed.value);
    std::get<Json::Object>(object["values"].value)["ok"].value=std::string("false");
    object["time"].value=std::string("2026-10-28T09:00:08.000Z");rows.push_back(malformed);
    const auto safe=JsonParser::parse(buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,history));
    bool validWorker=false;for(const auto& row:std::get<Json::Array>(safe.get("events").value))if(row.get("values").get("action").number()==220&&row.get("values").get("stage").number()==3&&row.get("time").string()=="2026-10-28T09:00:02.000Z")validWorker=true;
    require(validWorker,"Malformed boolean hid valid worker-retirement evidence");
  }
  {
    // Code245 count changed physically, but its channel-close cause was lost
    // behind old admission failures and newer ordinary success observations.
    Json history;auto& rows=history.value.emplace<Json::Array>();
    const auto add=[&](unsigned action,unsigned stage,const char* time,unsigned error=87){
      rows.push_back(JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":"+std::to_string(action)+",\"stage\":"+std::to_string(stage)+",\"ok\":false,\"error\":"+std::to_string(error)+",\"path\":\"DO_NOT_EXPORT_CHANNEL_PATH\",\"nonce\":\"DO_NOT_EXPORT_CHANNEL_NONCE\"}}"));
    };
    add(253,5,"2026-10-31T15:49:00.000Z");add(253,7,"2026-10-31T15:49:01.000Z");
    add(220,3,"2026-10-31T15:49:02.000Z");add(220,5,"2026-10-31T15:49:03.000Z");
    add(200,107,"2026-10-31T15:49:04.000Z");add(210,6,"2026-10-31T15:49:05.000Z");add(214,6,"2026-10-31T15:49:06.000Z");
    add(241,2,"2026-10-31T15:49:55.000Z",12030);add(241,1,"2026-10-31T15:49:56.000Z",0);
    for(unsigned i=0;i<30;++i)rows.push_back(JsonParser::parse(R"({"time":"2026-10-31T15:50:00.000Z","event":"hostAction","values":{"action":202,"stage":100,"ok":true,"error":0}})"));
    const auto close=rows[8];
    const auto check=[&](const Json& input,unsigned stage,unsigned error,unsigned cause=0){
      const auto bytes=buildHostDiagnosticReport(display,true,true,true,"",native,input,true,0,consoleRecovery,recovery);
      const auto decoded=JsonParser::parse(bytes);const auto& kept=std::get<Json::Array>(decoded.get("events").value);
      unsigned channels=0,clipboard=0,admission=0;std::string prior;
      for(const auto& row:kept){const auto& v=row.get("values");const auto action=v.get("action").number();
        require(prior<=row.get("time").string(),"Channel retention changed original event order");prior=row.get("time").string();
        if(action==241){++channels;require(v.get("stage").number()==stage&&v.get("error").number()==error&&row.get("time").string()=="2026-10-31T15:49:56.000Z"&&(cause?v.get("cause").number()==cause:std::holds_alternative<std::nullptr_t>(v.get("cause").value)),"Wrong or malformed channel close replaced the latest valid cause");}
        if(action==253)++clipboard;if(action==220||action==200||action==210||action==214)++admission;
      }
      require(channels==1&&clipboard==2&&admission==5&&kept.size()==8,"Success noise erased the channel cause or displaced existing failure categories");
      require(bytes.size()<=6144&&bytes.find("DO_NOT_EXPORT")==std::string::npos,"Channel retention escaped privacy or negotiated size bounds");
      require(decoded.get("captureAvailable").boolean()&&decoded.get("display").get("reason").string().empty()&&decoded.get("display").get("recoveryRequired").boolean()==display.recoveryRequired,"Historical channel loss changed current readiness");
    };
    check(history,1,0);
    for(unsigned cause:{1000u,1006u,4999u}){auto input=history;auto& object=std::get<Json::Object>(std::get<Json::Array>(input.value)[8].value);std::get<Json::Object>(object["values"].value)["cause"].value=double(cause);check(input,1,0,cause);}
    for(unsigned stage=1;stage<=6;++stage){auto input=history;auto& v=std::get<Json::Object>(std::get<Json::Array>(input.value)[8].value)["values"];
      auto& fields=std::get<Json::Object>(v.value);fields["stage"].value=double(stage);fields["error"].value=4294967295.0;check(input,stage,UINT32_MAX);}
    for(unsigned scenario=0;scenario<13;++scenario){auto input=history;auto malformed=close;auto& object=std::get<Json::Object>(malformed.value);auto& fields=std::get<Json::Object>(object["values"].value);object["time"].value=std::string("2026-10-31T15:49:57.000Z");
      switch(scenario){case 0:fields["stage"].value=0.0;break;case 1:fields["stage"].value=7.0;break;case 2:fields["stage"].value=1.5;break;case 3:fields["error"].value=-1.0;break;case 4:fields["error"].value=4294967296.0;break;case 5:fields["error"].value=0.5;break;case 6:fields["error"].value=std::string("0");break;case 7:fields.erase("error");break;case 8:fields["ok"].value=true;break;case 9:fields["ok"].value=std::string("false");break;case 10:fields["action"].value=std::string("241");break;case 11:object["time"].value=std::string("DO_NOT_EXPORT_TIMESTAMP");break;case 12:fields["stage"].value=std::numeric_limits<double>::infinity();break;}
      std::get<Json::Array>(input.value).push_back(std::move(malformed));check(input,1,0);
    }
    for(unsigned scenario=0;scenario<6;++scenario){auto input=history;auto malformed=close;auto& object=std::get<Json::Object>(malformed.value);auto& cause=std::get<Json::Object>(object["values"].value)["cause"];object["time"].value=std::string("2026-10-31T15:49:57.000Z");
      switch(scenario){case 0:cause.value=0.0;break;case 1:cause.value=999.0;break;case 2:cause.value=5000.0;break;case 3:cause.value=1006.5;break;case 4:cause.value=std::string("1006");break;case 5:cause.value=std::numeric_limits<double>::infinity();break;}
      std::get<Json::Array>(input.value).push_back(std::move(malformed));check(input,1,0);
    }
  }
  {
    // The capture owner can report a fixed failure before the service emits
    // worker_failed (stage 50). Preserve that cause in the SAME history slot;
    // successful observations and recovered x264 fallback are not substitutes.
    for(unsigned scenario=0;scenario<3;++scenario){
      Json history;auto& rows=history.value.emplace<Json::Array>();
      const auto add=[&](unsigned action,unsigned stage,const char* time,bool malformed=false){
        rows.push_back(JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":"+std::to_string(action)+",\"stage\":"+std::to_string(stage)+",\"ok\":"+(malformed?"\"false\"":"false")+",\"error\":17,\"path\":\"DO_NOT_EXPORT_WORKER_PATH\"}}"));
      };
      add(253,5,"2026-10-29T04:53:40.000Z");
      add(253,7,"2026-10-29T04:53:41.000Z");
      add(220,3,"2026-10-29T04:53:42.000Z");
      add(220,5,"2026-10-29T04:53:43.000Z");
      add(210,7,"2026-10-29T04:53:44.000Z");
      add(214,7,"2026-10-29T04:53:45.000Z");
      if(scenario!=1)add(200,207,"2026-10-29T04:53:56.000Z",scenario==2);
      add(200,240,"2026-10-29T04:53:57.000Z"); // Fallback can recover.
      add(200,50,"2026-10-29T04:53:58.000Z");  // Terminal category, not cause.
      for(unsigned i=0;i<30;++i)rows.push_back(std::get<Json::Array>(events.value)[i%20]);
      const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,history,true,0,consoleRecovery,recovery);
      const auto projected=JsonParser::parse(bytes);
      const auto& retained=std::get<Json::Array>(projected.get("events").value);
      bool cause=false,terminal=false,fallback=false;unsigned clipboard=0,observations=0;
      for(const auto& row:retained){
        const auto& values=row.get("values");const auto action=values.get("action").number(),stage=values.get("stage").number();
        if(action==200&&stage==207){cause=true;require(row.get("time").string()=="2026-10-29T04:53:56.000Z"&&values.get("error").number()==17,"Worker cause timestamp or fixed error was changed");}
        terminal|=action==200&&stage==50;fallback|=action==200&&stage==240;
        if(action==253)++clipboard;if(row.get("event").string()=="displayObserved")++observations;
      }
      require(scenario==0?cause&&!terminal:terminal&&!cause,"First worker failure was replaced by its terminal category");
      require(!fallback,"Recovered encoder fallback became the fatal worker cause");
      require(clipboard==2&&observations==1&&retained.size()==8,"Worker cause expanded the event cap or displaced protected categories");
      require(bytes.size()<=6144&&bytes.find("DO_NOT_EXPORT")==std::string::npos,"Worker cause escaped the diagnostic privacy/size bounds");
      require(projected.get("display").get("reason").string()=="viewport_probe_failed","Historical worker cause became current readiness authority");
    }
  }
  {
    auto failure=JsonParser::parse(R"({"time":"2026-10-27T09:38:22.000Z","event":"hostAction","values":{"action":253,"stage":5,"ok":false,"error":6,"cause":2,"win32":65541,"path":"DO_NOT_EXPORT_PATH"}})");
    Json history;auto& rows=history.value.emplace<Json::Array>();rows.push_back(failure);
    auto duplicate=failure;std::get<Json::Object>(duplicate.value)["time"].value=std::string("2026-10-27T09:40:00.000Z");rows.push_back(duplicate);
    auto other=failure;
    auto object=std::get<Json::Object>(other.value);auto details=std::get<Json::Object>(object["values"].value);details["cause"].value=3.0;object["values"].value=details;other.value=object;rows.push_back(other);
    for(unsigned i=0;i<30;++i)rows.push_back(std::get<Json::Array>(events.value)[i%20]);
    const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,history);
    const auto projected=JsonParser::parse(bytes);const auto& retained=std::get<Json::Array>(projected.get("events").value);
    unsigned failures=0;bool original=false;
    for(const auto& row:retained)if(row.get("values").get("action").number()==253){++failures;original|=row.get("time").string()=="2026-10-27T09:40:00.000Z";}
    require(retained.size()==8&&failures==2&&original,"Distinct clipboard failures were erased by success spam or their original timestamps changed");
    require(bytes.size()<=6144&&bytes.find("DO_NOT_EXPORT")==std::string::npos,"Priority failure evidence escaped size/privacy bounds");
  }
  {
    // Seven existing protected categories leave one slot for the newest actual
    // CPU-output GPU failure even after later success/observation spam.
    Json history;auto& rows=history.value.emplace<Json::Array>();
    const auto failure=[&](unsigned action,unsigned stage,const char* time){rows.push_back(JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":"+std::to_string(action)+",\"stage\":"+std::to_string(stage)+",\"ok\":false,\"error\":17}}"));};
    failure(253,5,"2026-10-30T00:00:00.000Z");failure(253,7,"2026-10-30T00:00:01.000Z");
    failure(220,3,"2026-10-30T00:00:02.000Z");failure(220,5,"2026-10-30T00:00:03.000Z");failure(200,207,"2026-10-30T00:00:04.000Z");failure(210,7,"2026-10-30T00:00:05.000Z");failure(214,7,"2026-10-30T00:00:06.000Z");
    auto gpuFailure=JsonParser::parse(R"({"time":"2026-10-30T00:00:07.000Z","event":"hostAction","values":{"action":255,"stage":103,"win32":8,"format":4318,"cause":11266,"error":42,"fallback":1,"items":0,"expected":2,"ok":false,"active":true,"path":"DO_NOT_EXPORT_GPU_PATH"}})");
    rows.push_back(gpuFailure);auto newer=gpuFailure;std::get<Json::Object>(newer.value)["time"].value=std::string("2026-10-30T00:00:08.000Z");auto& newValues=std::get<Json::Object>(std::get<Json::Object>(newer.value)["values"].value);newValues["stage"].value=112.0;newValues["win32"].value=4294967295.0;newValues["fallback"].value=2.0;newValues["items"].value=1.0;newValues["expected"].value=3.0;rows.push_back(newer);
    for(unsigned i=0;i<30;++i)rows.push_back(std::get<Json::Array>(events.value)[i%20]);
    rows.push_back(JsonParser::parse(R"({"time":"2026-10-30T00:00:09.000Z","event":"hostAction","values":{"action":254,"stage":3,"format":4318,"fallback":0,"items":1,"expected":4,"ok":true,"owned":true,"active":true}})"));
    for(unsigned scenario=0;scenario<3;++scenario){auto attempt=history;auto& attempts=std::get<Json::Array>(attempt.value);if(scenario){auto bad=newer;std::get<Json::Object>(bad.value)["time"].value=std::string("2026-10-30T00:00:10.000Z");auto& badValues=std::get<Json::Object>(std::get<Json::Object>(bad.value)["values"].value);if(scenario==1)badValues["win32"].value=std::string("forged");else badValues["format"].value=4098.0;attempts.push_back(bad);}
      const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,attempt,true,0,consoleRecovery,recovery);const auto projected=JsonParser::parse(bytes);const auto& retained=std::get<Json::Array>(projected.get("events").value);
      unsigned clips=0,gpu=0;std::set<unsigned> categories;size_t logBytes=2;
      for(const auto& row:retained){const auto& value=row.get("values");const auto action=value.get("action").number(),stage=value.get("stage").number();logBytes+=safeHostLogRecord(row).size()+(logBytes==2?0:1);if(action==253)++clips;
        const unsigned category=action==220&&stage==3?1:action==220&&(stage==4||stage==5)?2:action==200?3:action==210?4:action==214?5:0;if(category)categories.insert(category);
        if(action==255){++gpu;require(row.get("time").string()=="2026-10-30T00:00:08.000Z"&&stage==112&&value.get("win32").number()==4294967295.0,"Newest valid GPU failure timestamp or raw status was lost");require(value.get("fallback").number()==2&&value.get("items").number()==1&&value.get("expected").number()==3,"Historical actual CPU backend/track epoch became current GPU evidence");}
      }
      require(retained.size()==8&&clips==2&&categories.size()==5&&gpu==1,"GPU detail displaced protected clipboard/admission history or exceeded eight slots");
      require(bytes.size()<=6144&&logBytes<=4000&&bytes.find("DO_NOT_EXPORT")==std::string::npos,"GPU history escaped unchanged privacy/byte budgets");
      require(projected.get("display").get("reason").string()=="viewport_probe_failed","Historical encoder failure changed current readiness authority");
    }
  }
  {
    // hostAction 270: the newest result outlives newer ordinary events, at most
    // one is kept, and a row outside its fixed shape earns no protected slot.
    const auto probe=[](const char* time,unsigned stage,unsigned relay,bool ok){return JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":270,\"stage\":"+std::to_string(stage)+",\"error\":"+std::to_string(relay)+",\"ok\":"+(ok?"true":"false")+",\"ticket\":\"DO_NOT_EXPORT_TICKET\"}}");};
    const auto retained=[&](std::vector<Json> results){Json history;auto& rows=history.value.emplace<Json::Array>();for(auto& row:results)rows.push_back(std::move(row));for(unsigned i=0;i<30;++i)rows.push_back(std::get<Json::Array>(events.value)[i%20]);
      const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,history);require(bytes.size()<=6144&&bytes.find("DO_NOT_EXPORT")==std::string::npos,"A probe row escaped the diagnostic bounds or carried a private field");
      const auto projected=JsonParser::parse(bytes);std::vector<Json> kept;for(const auto& row:std::get<Json::Array>(projected.get("events").value))if(row.get("values").get("action").number()==270)kept.push_back(row);return kept;};
    {const auto kept=retained({probe("2026-11-01T00:00:00.000Z",4,0,false),probe("2026-11-01T00:00:01.000Z",1,1,true)});
      require(kept.size()==1&&kept[0].get("time").string()=="2026-11-01T00:00:01.000Z"&&kept[0].get("values").get("stage").number()==1&&kept[0].get("values").get("error").number()==1&&kept[0].get("values").get("ok").boolean(),"The newest UDP probe result was lost under newer events");}
    for(const unsigned stage:{2u,3u,4u}){const auto kept=retained({probe("2026-11-01T00:00:00.000Z",stage,7,false)});require(kept.size()==1&&kept[0].get("values").get("stage").number()==stage&&!kept[0].get("values").get("ok").boolean(),"A UDP probe failure was lost under newer events");}
    require(retained({probe("2026-11-01T00:00:00.000Z",5,0,false)}).empty(),"An unknown probe stage took a protected slot");
    require(retained({probe("2026-11-01T00:00:00.000Z",1,8,true)}).empty(),"A relay index past the grant's bound took a protected slot");
    require(retained({probe("2026-11-01T00:00:00.000Z",4,0,true)}).empty(),"A failed stage claiming success took a protected slot");
  }
  {
    // devices-v1 (top level): counts and fixed codes reach the report; anything else drops it whole.
    const std::string devices=R"({"schema":1,"displayAdapters":{"read":true,"current":1,"legacy":0,"duplicate":0,"unproven":4,"foreign":2,"currentStarted":true,"blocker":"","cleanup":"none"},"usbController":"duplicates"})";
    const auto projected=JsonParser::parse(buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,events,true,0,consoleRecovery,recovery,devices));
    require(projected.get("devices").get("displayAdapters").get("unproven").number()==4&&projected.get("devices").get("usbController").string()=="duplicates","The device inventory did not reach the report");
    for(const auto* hostile:{R"({"schema":1,"displayAdapters":{"read":true,"current":1},"usbController":"none"})",R"({"schema":1,"displayAdapters":{"read":false},"usbController":"ROOT\\USB\\0000"})",R"({"schema":1,"displayAdapters":{"read":true,"current":1,"legacy":0,"duplicate":0,"unproven":65,"foreign":2,"currentStarted":true,"blocker":"","cleanup":"none"},"usbController":"none"})"}){
      const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,events,true,0,consoleRecovery,recovery,hostile);
      require(bytes.find("\"devices\"")==std::string::npos&&bytes.find("ROOT\\")==std::string::npos,"A malformed device inventory reached the report");++cases;}
    // hostAction 273: the newest adapter inventory or cleanup outlives newer ordinary events.
    const auto adapter=[](const char* time,unsigned stage){return JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":273,\"stage\":"+std::to_string(stage)+",\"ok\":false,\"fallback\":4}}");};
    Json history;auto& rows=history.value.emplace<Json::Array>();rows.push_back(adapter("2026-11-01T00:00:00.000Z",0));rows.push_back(adapter("2026-11-01T00:00:01.000Z",1));for(unsigned i=0;i<30;++i)rows.push_back(std::get<Json::Array>(events.value)[i%20]);
    const auto kept=JsonParser::parse(buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,history));unsigned adapters=0;std::string newest;
    for(const auto& row:std::get<Json::Array>(kept.get("events").value))if(row.get("values").get("action").number()==273){++adapters;newest=row.get("time").string();}
    require(adapters==1&&newest=="2026-11-01T00:00:01.000Z","The newest display adapter event was lost under newer events");++cases;
    // hostAction 250 stage 31: the newest game-mouse summary outlives newer ordinary events.
    const auto mouse=[](const char* time,unsigned items){return JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":250,\"stage\":31,\"ok\":true,\"items\":"+std::to_string(items)+",\"error\":0,\"expected\":1}}");};
    Json mice;auto& mouseRows=mice.value.emplace<Json::Array>();mouseRows.push_back(mouse("2026-11-01T00:00:00.000Z",5));mouseRows.push_back(mouse("2026-11-01T00:00:01.000Z",0));for(unsigned i=0;i<30;++i)mouseRows.push_back(std::get<Json::Array>(events.value)[i%20]);
    const auto mouseKept=JsonParser::parse(buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,mice));unsigned summaries=0;std::string mouseNewest;
    for(const auto& row:std::get<Json::Array>(mouseKept.get("events").value))if(row.get("values").get("action").number()==250){++summaries;mouseNewest=row.get("time").string();}
    require(summaries==1&&mouseNewest=="2026-11-01T00:00:01.000Z","The newest game-mouse summary was lost under newer events");++cases;
    // hostAction 274 (boot_timeline.hpp): the newest boot timeline outlives a day of
    // newer ordinary events, with its stage fields; a game-mouse summary keeps its slot.
    const auto boot=[](const char* time,unsigned ready){return JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":274,\"stage\":"+std::to_string(ready?1:2)+",\"ok\":"+(ready?"true":"false")+",\"uptimeMs\":9000,\"configMs\":3,\"brokerMs\":640,\"virtualDisplayMs\":410,\"logonMs\":21000"+(ready?",\"sessionWorkerMs\":26000,\"readyMs\":26100":"")+"}}");};
    Json boots;auto& bootRows=boots.value.emplace<Json::Array>();bootRows.push_back(boot("2026-11-01T00:00:00.000Z",0));bootRows.push_back(boot("2026-11-01T00:00:01.000Z",1));bootRows.push_back(mouse("2026-11-01T00:00:02.000Z",3));for(unsigned i=0;i<30;++i)bootRows.push_back(std::get<Json::Array>(events.value)[i%20]);
    const auto bootKept=JsonParser::parse(buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,boots));unsigned timelines=0,bootMice=0;Json bootNewest;
    for(const auto& row:std::get<Json::Array>(bootKept.get("events").value)){if(row.get("values").get("action").number()==274){++timelines;bootNewest=row;}if(row.get("values").get("action").number()==250)++bootMice;}
    require(timelines==1&&bootMice==1&&bootNewest.get("time").string()=="2026-11-01T00:00:01.000Z"&&bootNewest.get("values").get("readyMs").number()==26100&&bootNewest.get("values").get("uptimeMs").number()==9000&&bootNewest.get("values").get("virtualDisplayMs").number()==410,"The newest boot timeline or its stages were lost under newer events");++cases;
  }
  {
    // hostAction 272: the newest route result outlives newer ordinary events (it is
    // recorded right before displayObserved), at most one is kept, and only its fixed shape.
    const auto route=[](const char* time,unsigned stage,bool ok){return JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":272,\"stage\":"+std::to_string(stage)+",\"ok\":"+(ok?"true":"false")+"}}");};
    const auto retained=[&](std::vector<Json> results){Json history;auto& rows=history.value.emplace<Json::Array>();for(auto& row:results)rows.push_back(std::move(row));for(unsigned i=0;i<30;++i)rows.push_back(std::get<Json::Array>(events.value)[i%20]);
      const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,history);require(bytes.size()<=6144,"A route row escaped the diagnostic bounds");
      const auto projected=JsonParser::parse(bytes);std::vector<Json> kept;for(const auto& row:std::get<Json::Array>(projected.get("events").value))if(row.get("values").get("action").number()==272)kept.push_back(row);return kept;};
    {const auto kept=retained({route("2026-11-01T00:00:00.000Z",2,false),route("2026-11-01T00:00:01.000Z",1,true)});
      require(kept.size()==1&&kept[0].get("time").string()=="2026-11-01T00:00:01.000Z"&&kept[0].get("values").get("stage").number()==1&&kept[0].get("values").get("ok").boolean(),"The newest Broker route result was lost under newer events");}
    {const auto kept=retained({route("2026-11-01T00:00:00.000Z",2,false)});require(kept.size()==1&&!kept[0].get("values").get("ok").boolean(),"A Broker route failure was lost under newer events");}
    require(retained({route("2026-11-01T00:00:00.000Z",3,false)}).empty(),"An unknown route stage took a protected slot");
    require(retained({route("2026-11-01T00:00:00.000Z",2,true)}).empty(),"A failed route claiming success took a protected slot");
  }
  {
    // Tuning #8: rows an accepted report carried step aside, so 30 events reach a
    // broker that keeps a history over four reports instead of the newest eight each
    // time; a refused report delivers nothing; untracked reports are unchanged.
    const auto row=[](unsigned i){return JsonParser::parse("{\"time\":\"2026-11-02T00:00:"+std::string(i<10?"0":"")+std::to_string(i)+".000Z\",\"event\":\"hostAction\",\"values\":{\"action\":220,\"stage\":2,\"ok\":true,\"error\":"+std::to_string(i)+"}}");};
    Json log;auto& rows=log.value.emplace<Json::Array>();for(unsigned i=0;i<30;++i)rows.push_back(row(i));
    const auto carried=[&](HostDiagnosticDelivery* delivery){std::vector<unsigned> out;const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,log,true,0,{},{},{},delivery);require(bytes.size()<=6144,"A tracked report escaped the diagnostic bounds");
      const auto parsed=JsonParser::parse(bytes);for(const auto& event:std::get<Json::Array>(parsed.get("events").value))out.push_back(unsigned(event.get("values").get("error").number()));return out;};
    const auto newest=carried(nullptr);
    require(newest==std::vector<unsigned>{22,23,24,25,26,27,28,29},"Without delivery tracking the newest eight changed");
    HostDiagnosticDelivery delivery;
    require(carried(&delivery)==newest&&!delivery.acknowledged(false)&&delivery.delivered().empty(),"A refused report counted as delivered");
    std::set<unsigned> seen;
    for(unsigned pass=0;pass<4;++pass){
      const auto sent=carried(&delivery);require(sent.size()==8,"A report carried fewer rows than it had room for");
      seen.insert(sent.begin(),sent.end());
      require(delivery.acknowledged(true)==(pass<3),"The backlog signal did not follow the rows still waiting");
    }
    require(seen.size()==30,"Some events never reached the broker");
    require(carried(&delivery)==newest&&!delivery.acknowledged(true),"A drained log no longer reported its newest rows");
    HostDiagnosticDelivery bounded;for(unsigned i=0;i<300;++i){bounded.sent({std::to_string(i)},false);bounded.acknowledged(true);}
    const auto kept=bounded.delivered();require(kept.size()==256&&!kept.count("0")&&kept.count("299"),"The delivered rows were not bounded to the newest");
  }
  {
    // hostAction 271: the newest fallback (stage 2, with its cause) or hold (stage 3)
    // outlives newer ordinary events; a switch onto UDP (stage 1) earns no slot.
    const auto path=[](const char* time,unsigned stage,unsigned reason,unsigned cause){return JsonParser::parse("{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":{\"action\":271,\"stage\":"+std::to_string(stage)+",\"error\":"+std::to_string(reason)+",\"cause\":"+std::to_string(cause)+",\"ok\":"+(stage==2?"false":"true")+"}}");};
    const auto retained=[&](std::vector<Json> results){Json history;auto& rows=history.value.emplace<Json::Array>();for(auto& row:results)rows.push_back(std::move(row));for(unsigned i=0;i<30;++i)rows.push_back(std::get<Json::Array>(events.value)[i%20]);
      const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,history);require(bytes.size()<=6144,"A path row escaped the diagnostic bounds");
      const auto projected=JsonParser::parse(bytes);std::vector<Json> kept;for(const auto& row:std::get<Json::Array>(projected.get("events").value))if(row.get("values").get("action").number()==271)kept.push_back(row);return kept;};
    {const auto kept=retained({path("2026-11-01T00:00:00.000Z",2,3,1),path("2026-11-01T00:00:01.000Z",2,3,3)});
      require(kept.size()==1&&kept[0].get("time").string()=="2026-11-01T00:00:01.000Z"&&kept[0].get("values").get("cause").number()==3,"The newest UDP fallback and its cause were lost under newer events");}
    {const auto kept=retained({path("2026-11-01T00:00:00.000Z",3,3,1)});require(kept.size()==1&&kept[0].get("values").get("stage").number()==3,"A hold through a dark network was lost under newer events");}
    require(retained({path("2026-11-01T00:00:00.000Z",1,1,0)}).empty(),"A switch onto UDP took a protected slot");
  }
  require(report.find("DO_NOT_EXPORT")==std::string::npos,"Private fields escaped the production diagnostic projection");
  require(parsed.get("protocol").string()=="host-diagnostics-v1"&&parsed.get("source").string()=="host-service","Diagnostic protocol/source was not explicit");
  require(parsed.get("display").get("installed").boolean()&&!parsed.get("display").get("activeDisplay").boolean()&&parsed.get("display").get("recoveryRequired").boolean(),"Installed adapter was mistaken for a working virtual output");
  require(parsed.get("native").get("failureReason").string()=="controller_start_failed"&&parsed.get("native").get("phase").string()=="recovery","Native initialization cause was lost");
  {auto pending=native;std::get<Json::Object>(pending.value)["failureReason"].value=std::string("native_stream_profile_recovery_required");const auto projected=JsonParser::parse(buildHostDiagnosticReport(display,true,true,false,"native_topology_recovery_required",pending,events));require(projected.get("native").get("failureReason").string()=="native_stream_profile_recovery_required","Pending profile recovery cause became an unspecified internal error");}
  require(parsed.get("consoleRecovery").get("reason").string()=="rdp_session_disconnected"&&parsed.get("consoleRecovery").get("disconnectedConsoleSessions").number()==1&&parsed.get("consoleRecovery").get("eligible").boolean(),"Disconnected local-protocol login recovery was lost");
  for(int scenario=0;scenario<5;++scenario){auto invalid=consoleRecovery;auto& object=std::get<Json::Object>(invalid.value);if(scenario==0)object["windowsSid"].value=std::string("DO_NOT_EXPORT_SID");if(scenario==1)object["reason"].value=std::string("DO_NOT_EXPORT_EXCEPTION");if(scenario==2)object["unknownSessions"].value=65.0;if(scenario==3)object["ready"].value=std::string("false");if(scenario==4)object.erase("complete");const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",native,events,true,0,invalid);require(bytes.find("DO_NOT_EXPORT")==std::string::npos&&std::holds_alternative<std::nullptr_t>(JsonParser::parse(bytes).get("consoleRecovery").value),"Malformed console summary escaped its bounded projection");}
  require(parsed.get("native").get("activation").get("reason").string()=="native_topology_validate_failed"&&parsed.get("native").get("activation").get("systemError").number()==ERROR_BAD_CONFIGURATION&&!parsed.get("native").get("activationCurrent").boolean(),"Precise or retired Windows activation failure was lost");
  {DWORD flags=0;const BOOL success=GetHandleInformation(nullptr,&flags);const DWORD original=GetLastError();bool caught=false;
    try{winCheck(success,"DO_NOT_EXPORT_WORKER_OPERATION");}catch(const WindowsOperationError& error){caught=true;SetLastError(ERROR_CANCELLED);auto failed=native;auto& object=std::get<Json::Object>(failed.value);object["activation"]=JsonParser::parse(nativeActivationDiagnosticJson({"native_activation_worker_unavailable",error.systemError(),false,false}));object["activationCurrent"].value=true;
      const auto bytes=buildHostDiagnosticReport(display,true,true,false,"viewport_probe_failed",failed,events);const auto observed=JsonParser::parse(bytes).get("native").get("activation");
      require(original&&observed.get("systemError").number()==original&&!observed.get("attempted").boolean()&&!observed.get("windowsVerified").boolean()&&bytes.find("DO_NOT_EXPORT")==std::string::npos,"Worker launch error was lost, exposed raw text or claimed Windows activation");}
    require(caught,"Expected actual worker-style Windows API error was not observed");}
  for(int scenario=0;scenario<3;++scenario){auto invalid=native;auto& fields=std::get<Json::Object>(invalid.value);auto& activation=std::get<Json::Object>(fields["activation"].value);switch(scenario){case 0:activation["reason"].value=std::string("DO_NOT_EXPORT_EXCEPTION");break;case 1:activation["systemError"].value=-1.0;break;case 2:activation["windowsVerified"].value=true;break;}const auto bytes=buildHostDiagnosticReport(display,true,true,false,"native_topology_not_ready",invalid,events);require(bytes.find("DO_NOT_EXPORT")==std::string::npos&&std::holds_alternative<std::nullptr_t>(JsonParser::parse(bytes).get("native").get("activation").value),"Unsafe diagnostic fields were transmitted or invalidated the whole report");}
  const auto* log=std::get_if<Json::Array>(&parsed.get("events").value);
  require(log&&log->size()==8&&(*log)[0].get("values").get("monitors").number()==0,"Recent log subset/order was not bounded to the last eight entries");
  auto hostile=native;auto& fields=std::get<Json::Object>(hostile.value);fields["failureReason"].value=std::string("DO_NOT_EXPORT_ERROR");fields["phase"].value=std::string("DO_NOT_EXPORT_PHASE");
  const auto safe=buildHostDiagnosticReport(display,true,false,false,"DO_NOT_EXPORT_ERROR",hostile,events);
  require(safe.find("DO_NOT_EXPORT")==std::string::npos&&JsonParser::parse(safe).get("display").get("reason").string()=="viewport_probe_failed","Raw exceptions were transmitted as diagnostics");
  const auto unavailable=JsonParser::parse(buildHostDiagnosticReport(display,false,false,false,"no_interactive_session",Json{},Json{}));
  require(!unavailable.get("native").get("available").boolean()&&std::get<Json::Array>(unavailable.get("events").value).empty(),"Unavailable observations invented native status or events");
  const auto unknown=JsonParser::parse(buildHostDiagnosticReport(DisplayState{},true,true,false,"viewport_probe_pending",Json{},Json{},false,-1));
  require(!unknown.get("display").get("observed").boolean()&&std::holds_alternative<std::nullptr_t>(unknown.get("display").get("observationAgeMs").value),"An unobserved worker cache was labelled as a fresh zero-monitor observation");
  const auto* outputs=std::get_if<Json::Array>(&parsed.get("display").get("monitors").value);
  require(outputs&&outputs->size()==2&&(*outputs)[0].get("id").string()=="0"&&(*outputs)[0].get("left").number()==1920&&(*outputs)[1].get("top").number()==-120&&!(*outputs)[0].get("primary").boolean()&&!(*outputs)[1].get("primary").boolean(),"Owned secondary-output geometry or absence of owned primary was lost");
  require(std::get<Json::Object>((*outputs)[0].value).size()==6,"Private identity fields entered output diagnostics");
  const auto project=[&](const DisplayState& value,bool observed=true){return JsonParser::parse(buildHostDiagnosticReport(value,true,true,false,"viewport_probe_failed",native,events,observed)).get("display");};
  auto single=display;single.monitors.resize(1);single.monitors[0].left=single.monitors[0].top=0;single.monitors[0].primary=true;
  const auto one=project(single);require(std::get<Json::Array>(one.get("monitors").value).size()==1&&std::get<Json::Array>(one.get("monitors").value)[0].get("primary").boolean(),"One owned primary output was not preserved");
  auto empty=display;empty.monitors.clear();require(std::get<Json::Array>(project(empty).get("monitors").value).empty(),"Observed empty topology was omitted");
  auto foreign=display;foreign.owned=false;
  require(std::holds_alternative<std::nullptr_t>(project(foreign).get("monitors").value)&&std::holds_alternative<std::nullptr_t>(project(display,false).get("monitors").value),"Foreign or unobserved topology was reported as owned");
  for(int scenario=0;scenario<11;++scenario){auto invalid=display;auto& first=invalid.monitors[0];switch(scenario){
    case 0:first.id="DO_NOT_EXPORT_ID";break;case 1:first.id="1";break;case 2:first.width=0;break;case 3:first.height=16385;break;
    case 4:first.left=-32769;break;case 5:first.top=32769;break;case 6:first.primary=true;break;
    case 7:first.left=first.top=0;break;case 8:invalid.monitors.push_back(first);break;
    case 9:for(auto& item:invalid.monitors){item.left=item.top=0;item.primary=true;}break;case 10:first.width=MAXDWORD;break;}
    const auto result=project(invalid);require(std::holds_alternative<std::nullptr_t>(result.get("monitors").value)&&result.get("installed").boolean()&&result.get("recoveryRequired").boolean(),"Malformed optional geometry escaped or discarded base diagnostics");
  }
  auto bounded=display;bounded.monitors[0].left=bounded.monitors[0].top=-32768;bounded.monitors[1].left=bounded.monitors[1].top=32768;for(auto& item:bounded.monitors)item.width=item.height=16384;
  Json::Object maximumValues;for(const auto* key:{"action","stage","monitors","width","height","error","expected","missing","missingOwned","fallback","stale"})maximumValues[key].value=4294967295.0;
  for(const auto* key:{"ok","installed","started","active","owned","pending","recovery","reboot","remote","running","registered","online","changed","restartAttempted","consoleExpected","consoleActive","remapped"})maximumValues[key].value=false;
  auto maximumEvent=(*log)[0];std::get<Json::Object>(maximumEvent.value)["values"].value=maximumValues;Json maximumEvents;maximumEvents.value=Json::Array(8,maximumEvent);
  auto maximumConsole=consoleRecovery;for(const auto* key:{"activeRdpSessions","disconnectedRdpSessions","disconnectedConsoleSessions","unknownSessions","lockedSessions"})std::get<Json::Object>(maximumConsole.value)[key].value=64.0;
  const auto maximumReport=buildHostDiagnosticReport(bounded,true,true,false,"native_display_configuration_untrusted",native,maximumEvents,true,4294967295LL,maximumConsole);
  require(maximumReport.size()<=6144&&std::get<Json::Array>(JsonParser::parse(maximumReport).get("display").get("monitors").value).size()==2,"Bounded geometry exceeded the automatic diagnostic budget");
  if(argc==3&&(std::wstring(argv[1])==L"--output"||std::wstring(argv[1])==L"--readiness-output")){
    const auto& output=std::wstring(argv[1])==L"--readiness-output"?readinessReport:report;
    WinHandle file(CreateFileW(argv[2],GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD written=0;
    require(file.value!=INVALID_HANDLE_VALUE&&WriteFile(file.value,output.data(),DWORD(output.size()),&written,nullptr)&&written==output.size()&&FlushFileBuffers(file.value),"Could not create the exact native diagnostic fixture");
  }else require(argc==1,"Unknown QA arguments");
  std::cout<<"{\"kind\":\"host-diagnostic-production-projection\",\"checkpoints\":"<<cases<<",\"recentEvents\":8,\"privateFieldsExcluded\":true,\"actualHelperInvoked\":false,\"actualNetworkSend\":false,\"windowsSettingsChanged\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
