#include "frame_encode_policy.hpp"
#include <functional>
#include <iostream>
#include <limits>

using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F action,const char* reason){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed,reason);}
struct EncodedFixture {uint64_t pts=0;bool key=false;std::vector<uint8_t> pixels;};
struct DelayedEncoder {
  explicit DelayedEncoder(size_t bytes,size_t delay=1):policy(bytes),delay(delay){}
  FrameEncodePolicy policy;size_t delay;size_t inputs=0,refreshes=0;
  std::deque<EncodedFixture> queue;std::vector<EncodedFixture> outputs;
  std::vector<uint64_t> outputTimes;
  FrameEncodeDecision tick(const std::vector<uint8_t>& frame,uint64_t now){
    const auto decision=policy.prepare(frame,now);
    if(!decision.submit)return decision;
    if(decision.refresh)++refreshes;
    queue.push_back({now,inputs++==0,frame});
    if(queue.size()>delay){auto packet=std::move(queue.front());queue.pop_front();require(policy.output(packet.pts,packet.key,now),"A real delayed fixture output was not accepted");outputs.push_back(std::move(packet));outputTimes.push_back(now);}
    return decision;
  }
};
void staticThirtySeconds(){
  const std::vector<uint8_t> scene(96,80);DelayedEncoder encoder(scene.size());size_t captures=0;
  for(uint64_t now=0;now<=30000000;now+=16666){encoder.tick(scene,now);++captures;require(encoder.policy.pendingInputs()<=2,"Idle refresh accumulated unbounded pending inputs");}
  require(captures>1800&&encoder.inputs>30&&encoder.inputs<70&&encoder.refreshes>=28,"Thirty static seconds were constantly encoded or lost periodic refresh");
  require(!encoder.outputs.empty()&&encoder.outputs.front().key&&encoder.policy.hasUsableIdr(),"Static startup never produced a usable IDR");
  uint64_t previous=0;for(size_t i=0;i<encoder.outputs.size();++i){require(encoder.outputs[i].pixels==scene,"Idle refresh changed the image");require(encoder.outputTimes[i]-previous<1100000,"Static output gap could trip the 15-second service/client watchdog");previous=encoder.outputTimes[i];}
  require(30000000-previous<1100000,"End of static interval lost the last refresh");
}
void changedFrameAndOwnedSnapshot(){
  std::vector<uint8_t> reusable(96,60);DelayedEncoder encoder(reusable.size(),3);
  for(uint64_t now=0;now<100000;now+=10000)encoder.tick(reusable,now);
  require(!encoder.policy.awaitingOutput(),"Initial delayed IDR did not settle");
  reusable[17]^=1;const auto changed=encoder.tick(reusable,100000);
  require(changed.submit&&changed.contentChanged&&encoder.policy.awaitingOutput(),"A cursor-like single-byte change was ignored");
  // The producer owns and reuses this same buffer, exactly like Capture::nv12_.
  reusable[18]^=1;const auto changedAgain=encoder.tick(reusable,110000);
  require(changedAgain.contentChanged,"Policy retained the capture reference instead of owning its snapshot");
  for(uint64_t now=120000;now<=150000;now+=10000)encoder.tick(reusable,now);
  require(!encoder.policy.awaitingOutput()&&encoder.outputs.back().pixels==reusable,"Last changed frame was stranded until the one-second idle refresh");
  require(!encoder.tick(reusable,160000).submit,"Duplicate priming submissions kept moving the dirty PTS watermark");
}
void refreshMustReachOutput(){
  std::vector<uint8_t> scene(96,71);DelayedEncoder encoder(scene.size());
  encoder.tick(scene,0);encoder.tick(scene,10000);
  require(!encoder.tick(scene,1009999).submit,"Refresh ran before one second from actual output");
  const auto refresh=encoder.tick(scene,1010000);
  require(refresh.submit&&refresh.refresh&&encoder.policy.awaitingOutput(),"An older delayed output incorrectly completed the newly submitted refresh");
  const auto duplicate=encoder.tick(scene,1020000);
  require(duplicate.submit&&!duplicate.contentChanged&&!duplicate.refresh&&!encoder.policy.awaitingOutput(),"Refresh burst stranded its final frame or moved its watermark");
  require(!encoder.tick(scene,1020001).submit,"Completed refresh failed to quiesce");
  FrameEncodePolicy delayed(scene.size());require(delayed.prepare(scene,0).submit,"Delayed start did not submit");
  require(delayed.output(0,true,1000000),"Delayed first IDR rejected");
  require(!delayed.prepare(scene,1000001).submit,"Refresh used input time rather than actual output time");
  require(delayed.prepare(scene,2000000).refresh,"Output-based refresh did not occur at its exact deadline");
}
void usableIdrAndPtsProof(){
  std::vector<uint8_t> scene(96,20);FrameEncodePolicy policy(scene.size());
  require(policy.prepare(scene,0).submit,"First frame did not submit");
  require(!policy.output(500,true,1)&&!policy.output(1,true,2),"Unsubmitted/future PTS fabricated output progress");
  require(policy.output(0,false,2)&&!policy.hasUsableIdr(),"An ordinary output was mislabeled a usable IDR");
  require(policy.prepare(scene,3).submit&&policy.output(3,false,3)&&policy.awaitingOutput(),"No-IDR startup stopped priming");
  require(policy.prepare(scene,4).submit&&policy.output(4,true,5)&&policy.hasUsableIdr(),"Actual submitted IDR failed to complete priming");
  require(!policy.prepare(scene,6).submit&&!policy.output(4,true,7),"A repeated output PTS kept the idle stream alive");
  scene.back()^=1;require(policy.prepare(scene,10).contentChanged,"Single final-byte change missed");
  require(!policy.output(9,false,11)&&!policy.output(10,false,9)&&policy.awaitingOutput(),"Mismatched or backwards-clock output completed dirty content");
  require(policy.output(10,false,11)&&!policy.awaitingOutput()&&!policy.prepare(scene,12).submit,"Exact delayed PTS failed to complete the change");
}
void independentOutputsAndResize(){
  std::vector<uint8_t> still(96,33),moving(96,44);DelayedEncoder left(still.size()),right(moving.size());
  for(uint64_t now=0;now<=3000000;now+=16666){moving[9]^=1;left.tick(still,now);right.tick(moving,now);}
  require(left.inputs<10&&right.inputs>170,"One changing output forced its unchanged neighbour to encode");
  FrameEncodePolicy fresh(120);std::vector<uint8_t> resized(120,33);
  require(!fresh.hasUsableIdr()&&fresh.awaitingOutput()&&fresh.pendingInputs()==0,"Fresh resize instance retained the old output's state");
  require(fresh.prepare(resized,0).contentChanged&&!fresh.output(2000000,true,2000000),"Resize accepted an old output PTS");
  require(fresh.output(0,true,2000000)&&!fresh.awaitingOutput(),"Fresh resize IDR could not start independently");
}
void boundedAndMonotonic(){
  rejects([]{FrameEncodePolicy invalid(0);},"Empty frame size accepted");
  rejects([]{FrameEncodePolicy invalid(size_t(UINT32_MAX)+1);},"Unrepresentable DWORD frame allocation accepted");
  std::vector<uint8_t> scene(96,0);FrameEncodePolicy policy(scene.size());
  rejects([&]{policy.prepare(std::span(scene).first(95),0);},"Mismatched final NV12 size accepted");
  policy.prepare(scene,10);rejects([&]{policy.prepare(scene,9);},"Backwards input clock accepted");
  rejects([&]{policy.prepare(scene,10);},"Duplicate submitted PTS accepted");
  require(policy.output(10,true,11),"Valid proof after rejected clock did not recover");
  require(!policy.prepare(scene,12).submit,"Rejected input corrupted the stored content");
  FrameEncodePolicy stalled(scene.size());
  for(uint64_t now=0;now<FrameEncodePolicy::maximumPendingInputs;++now)require(stalled.prepare(scene,now).submit,"No-output startup stopped priming");
  rejects([&]{stalled.prepare(scene,FrameEncodePolicy::maximumPendingInputs);},"Silent/mismatched encoder grew its PTS ledger without bound");
  require(stalled.pendingInputs()==FrameEncodePolicy::maximumPendingInputs,"Rejected overflow changed the pending ledger");
  require(stalled.output(FrameEncodePolicy::maximumPendingInputs-1,true,FrameEncodePolicy::maximumPendingInputs+1)&&stalled.pendingInputs()==0,"A valid dropped-input successor could not retire bounded old PTS entries");
}
// ---- bounded progressive refinement (FrameRefinement)
constexpr uint64_t tick=16667;
FrameRefinement smallRefinement(){FrameRefinement r;r.budgetBytes=1000;r.triggerBytes=500;r.settledBytes=250;r.settledRun=3;r.maximumFrames=90;r.quietUs=50000;return r;}
// Immediate (low-latency) encoder fixture: every submission is output at once
// with a size chosen by the scenario, like the software MFT's synchronous path.
struct SizedEncoder {
  explicit SizedEncoder(size_t bytes,FrameRefinement refinement=smallRefinement()):policy(bytes,refinement){}
  FrameEncodePolicy policy;size_t inputs=0,refines=0,changes=0,refreshes=0;
  FrameEncodeDecision tick(const std::vector<uint8_t>& frame,uint64_t now,const std::function<size_t(const FrameEncodeDecision&)>& size){
    const auto decision=policy.prepare(frame,now);
    if(!decision.submit){require(!decision.refine&&!decision.contentChanged&&!decision.refresh,"Unsubmitted decision carried a reason");return decision;}
    require(!decision.refine||(!decision.contentChanged&&!decision.refresh),"A refinement submission replaced a change or refresh");
    ++inputs;refines+=decision.refine;changes+=decision.contentChanged;refreshes+=decision.refresh;
    require(policy.output(now,inputs==1,now,size(decision)),"A sized fixture output was rejected");
    return decision;
  }
};
size_t sized(size_t change,size_t other,const FrameEncodeDecision& d){return d.contentChanged?change:other;}
void refinementBuildsThenQuiesces(){
  const std::vector<uint8_t> page(96,1);SizedEncoder encoder(page.size());
  // A starved 5x-budget change: the policy waits for quietUs, then refines.
  require(encoder.tick(page,0,[](auto& d){return sized(5000,100,d);}).contentChanged&&encoder.policy.refinementArmed(),"A large change output did not arm refinement");
  require(!encoder.tick(page,tick,[](auto& d){return sized(5000,100,d);}).submit&&!encoder.tick(page,2*tick,[](auto& d){return sized(5000,100,d);}).submit,"Refinement began before content was quiet");
  uint64_t now=3*tick;std::vector<uint64_t> refined;
  for(;now<=3000000;now+=tick){const auto d=encoder.tick(page,now,[](auto& x){return sized(5000,100,x);});if(d.refine)refined.push_back(now);}
  // Debt 4000 B repaid at 900 B per tiny output: settling starts with the
  // fourth output, so exactly 4 + (settledRun - 1) = 6 refinement frames.
  require(refined.size()==6&&refined.front()==3*tick,"Refinement ignored the modeled CBR debt or did not stop once settled");
  for(size_t i=1;i<refined.size();++i)require(refined[i]-refined[i-1]==tick,"Refinement skipped capture ticks");
  require(!encoder.policy.refinementArmed()&&encoder.changes==1,"Settled refinement stayed armed");
  // Quiescent again: only the ordinary 1 s output-clock refresh remains.
  require(encoder.refreshes>=1&&encoder.refreshes<=2&&encoder.inputs==1+6+encoder.refreshes,"Settled stream did not return to the 1 s refresh");
}
// agent.hpp shareTiles: only a refinement output leaves the tiles' share alone.
void lastOutputNamesRefinement(){
  const std::vector<uint8_t> page(96,1);SizedEncoder encoder(page.size());
  encoder.tick(page,0,[](auto& d){return sized(5000,100,d);});
  require(!encoder.policy.lastOutputRefinement(),"The change output was reported as refinement");
  size_t refined=0,others=0;
  for(uint64_t now=tick;now<=3000000;now+=tick){
    const auto d=encoder.tick(page,now,[](auto& x){return sized(5000,100,x);});
    if(!d.submit)continue;
    require(encoder.policy.lastOutputRefinement()==d.refine,"lastOutputRefinement disagreed with the submission it answered");
    (d.refine?refined:others)++;
  }
  require(refined==6&&others>=1,"The scenario did not exercise both refinement and refresh outputs");
  std::vector<uint8_t> changed(96,2);encoder.tick(changed,3000000+tick,[](auto& d){return sized(5000,100,d);});
  require(!encoder.policy.lastOutputRefinement(),"A later change output kept the refinement mark");
}
void refinementIsBounded(){
  const std::vector<uint8_t> page(96,2);SizedEncoder encoder(page.size());
  // Outputs that never become small (noise, or an encoder still refining).
  for(uint64_t now=0;now<=4000000;now+=tick)encoder.tick(page,now,[](auto& d){return sized(5000,1000,d);});
  require(encoder.refines==90,"Non-converging refinement was not bounded by maximumFrames");
  require(encoder.refreshes>=1&&encoder.refreshes<=3,"Bounded refinement did not fall back to the 1 s refresh");
}
void smallChangesNeverArm(){
  std::vector<uint8_t> page(96,3);SizedEncoder encoder(page.size());FrameEncodePolicy plain(page.size());
  for(uint64_t now=0,n=0;now<=6000000;now+=tick,++n){
    if(n%18==0&&n)page[n%96]^=1;  // cursor/caret-sized change every 300 ms
    const auto a=encoder.tick(page,now,[](auto&){return size_t(100);});const auto b=plain.prepare(page,now);if(b.submit)plain.output(now,now==0,now);
    require(a.submit==b.submit&&a.contentChanged==b.contentChanged&&a.refresh==b.refresh&&!a.refine,"Small changes armed refinement or altered the original decisions");
  }
  require(encoder.refines==0&&!encoder.policy.refinementArmed(),"Small outputs armed refinement");
}
void motionIsUnchanged(){
  for(const uint64_t every:{uint64_t(1),uint64_t(2),uint64_t(3)}){  // 60, 30 and 20 fps content
    std::vector<uint8_t> frame(96,4);SizedEncoder encoder(frame.size());FrameEncodePolicy plain(frame.size());
    for(uint64_t n=0;n<180;++n){
      const uint64_t now=n*tick;if(n%every==0)frame[n%96]^=0x55;
      const auto a=encoder.tick(frame,now,[](auto& d){return sized(5000,100,d);});const auto b=plain.prepare(frame,now);if(b.submit)plain.output(now,now==0,now);
      require(a.submit==b.submit&&a.contentChanged==b.contentChanged&&!a.refine,"Refinement changed decisions while content kept moving");
    }
    require(encoder.refines==0&&encoder.policy.refinementArmed(),"Moving content was refined or lost its pending refinement");
    // Motion stops: the final frame is refined after exactly quietUs.
    uint64_t first=0;for(uint64_t n=180;n<200;++n){const auto d=encoder.tick(frame,n*tick,[](auto& x){return sized(5000,100,x);});if(d.refine&&!first)first=n*tick;}
    const uint64_t lastChange=((179/every)*every)*tick;
    require(first&&first-lastChange>=50000&&first-lastChange<50000+tick,"Refinement after motion did not start at the quiet boundary");
  }
  // Production settings: 15 and 10 fps animation (large frames) is never refined between frames.
  for(const uint64_t every:{uint64_t(4),uint64_t(6)}){
    std::vector<uint8_t> frame(96,9);SizedEncoder encoder(frame.size(),FrameRefinement::forCbr(12000000,60));FrameEncodePolicy plain(frame.size());
    for(uint64_t n=0;n<240;++n){
      const uint64_t now=n*tick;if(n%every==0)frame[n%96]^=0x33;
      const auto a=encoder.tick(frame,now,[](auto& d){return sized(40000,2000,d);});const auto b=plain.prepare(frame,now);if(b.submit)plain.output(now,now==0,now);
      require(a.submit==b.submit&&!a.refine,"Production refinement fired between >=10 fps animation frames");
    }
    require(encoder.refines==0&&encoder.policy.refinementArmed(),"Production animation lost its pending refinement");
  }
}
void changesInterruptRefinement(){
  std::vector<uint8_t> page(96,5);SizedEncoder encoder(page.size());
  uint64_t now=0;encoder.tick(page,now,[](auto& d){return sized(5000,1000,d);});
  for(int n=0;n<10;++n){now+=tick;encoder.tick(page,now,[](auto& d){return sized(5000,1000,d);});}
  const auto before=encoder.refines;require(before>0,"Refinement did not start");
  // A cursor-sized change during refinement: submitted at once as a change,
  // refinement waits for quiet again and its bound is NOT reset.
  page[7]^=1;now+=tick;const auto change=encoder.tick(page,now,[](auto& d){return sized(100,1000,d);});
  require(change.submit&&change.contentChanged&&!change.refine,"A change during refinement was not submitted as a change");
  require(!encoder.tick(page,now+tick,[](auto& d){return sized(100,1000,d);}).submit,"Refinement resumed before content was quiet again");
  for(int n=0;n<300;++n){now+=tick;encoder.tick(page,now,[](auto& d){return sized(100,1000,d);});}
  require(encoder.refines==90,"A small change reset or extended the refinement bound");
  // A large change starts a fresh bounded episode.
  page[8]^=1;now+=tick;encoder.tick(page,now,[](auto& d){return sized(5000,1000,d);});
  for(int n=0;n<300;++n){now+=tick;encoder.tick(page,now,[](auto& d){return sized(5000,1000,d);});}
  require(encoder.refines==180,"A large change did not start a new bounded episode");
}
void refinementNeedsActualSizes(){
  const std::vector<uint8_t> page(96,6);FrameEncodePolicy policy(page.size(),smallRefinement());size_t inputs=0,refines=0;
  for(uint64_t now=0;now<=3000000;now+=tick){const auto d=policy.prepare(page,now);if(!d.submit)continue;++inputs;refines+=d.refine;require(policy.output(now,now==0,now),"Unsized output rejected");}
  require(refines==0&&inputs<=5,"Unknown output sizes armed refinement");
  // An encoder that drops a refinement input: its successor retires it.
  FrameEncodePolicy dropping(page.size(),smallRefinement());uint64_t now=0;dropping.prepare(page,now);dropping.output(now,true,now,5000);
  std::vector<uint64_t> submitted;for(now=tick;submitted.size()<3;now+=tick)if(dropping.prepare(page,now).refine)submitted.push_back(now);
  require(dropping.output(submitted[0],false,now,1000)&&dropping.pendingInputs()==2,"A refinement output was rejected");
  // submitted[1] is never output (dropped by the encoder).
  require(dropping.output(submitted[2],false,now,1000)&&dropping.pendingInputs()==0,"A dropped refinement input stranded the PTS ledger");
}
void refinementSettingsValidated(){
  auto bad=smallRefinement();bad.settledRun=0;rejects([&]{FrameEncodePolicy p(96,bad);},"Zero settled run accepted");
  bad=smallRefinement();bad.maximumFrames=FrameEncodePolicy::maximumRefinementFrames+1;rejects([&]{FrameEncodePolicy p(96,bad);},"Unbounded refinement accepted");
  bad=smallRefinement();bad.quietUs=FrameEncodePolicy::refreshIntervalUs+1;rejects([&]{FrameEncodePolicy p(96,bad);},"Quiet period beyond the refresh interval accepted");
  bad=smallRefinement();bad.settledBytes=bad.budgetBytes+1;rejects([&]{FrameEncodePolicy p(96,bad);},"Settled size above the budget accepted");
  const auto hd=FrameRefinement::forCbr(12000000,60),qhd=FrameRefinement::forCbr(20000000,60),low=FrameRefinement::forCbr(4000000,30);
  require(hd.budgetBytes==25000&&hd.triggerBytes==12500&&hd.settledBytes==6250&&hd.maximumFrames==90&&hd.quietUs==100000&&hd.enabled(),"1080p production refinement values changed");
  require(qhd.budgetBytes==41666&&low.budgetBytes==16666&&low.maximumFrames==45,"Per-track/fps refinement budget is not bitrate/8/fps");
  require(!FrameRefinement::forCbr(0,60).enabled()&&!FrameRefinement{}.enabled(),"Disabled refinement reported enabled");
  FrameEncodePolicy p(96,hd);(void)p;
}
// Adaptive bitrate: a live target change retargets the refinement budget of the
// running policy (episode kept); disabling or invalid budgets are refused.
void refinementRetarget(){
  FrameEncodePolicy policy(96,FrameRefinement::forCbr(20000000,60));
  policy.retarget(FrameRefinement::forCbr(60000000,60));require(policy.refinement().budgetBytes==125000&&policy.refinement().maximumFrames==90,"Retarget did not follow the live bitrate");
  policy.retarget(FrameRefinement::forCbr(3000000,60));require(policy.refinement().budgetBytes==6250,"Retarget down did not follow the live bitrate");
  rejects([&]{policy.retarget(FrameRefinement{});},"Retarget disabled a running refinement policy");
  FrameEncodePolicy plain(96);rejects([&]{plain.retarget(FrameRefinement::forCbr(20000000,60));},"Retarget enabled refinement on a plain policy");
  auto bad=FrameRefinement::forCbr(20000000,60);bad.settledBytes=bad.budgetBytes+1;rejects([&]{policy.retarget(bad);},"Retarget accepted an invalid budget");
}
}int main(){try{staticThirtySeconds();changedFrameAndOwnedSnapshot();refreshMustReachOutput();usableIdrAndPtsProof();independentOutputsAndResize();boundedAndMonotonic();
  refinementBuildsThenQuiesces();lastOutputNamesRefinement();refinementIsBounded();smallChangesNeverArm();motionIsUnchanged();changesInterruptRefinement();refinementNeedsActualSizes();refinementSettingsValidated();refinementRetarget();
  std::cout<<"{\"kind\":\"changed-frame-encode-policy\",\"groups\":14,\"syntheticIdleSeconds\":30,\"delayedEncoderFixture\":true,\"finalOwnedNv12Comparison\":true,\"cursorLikeByteChanges\":true,\"changedAndRefreshPtsPriming\":true,\"actualOutputClockRefresh\":true,\"usableIdrRequired\":true,\"boundedPtsLedger\":true,\"perOutputPolicyIndependent\":true,"
              "\"boundedRefinement\":true,\"refinementDebtModel\":true,\"refinementMotionUnchanged\":true,\"refinementSmallChangesUnchanged\":true,\"refinementRetarget\":true,\"realDualStreamingImplemented\":false,\"actualCaptureOrCodecExecuted\":false,\"windowsSettingsChanged\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
