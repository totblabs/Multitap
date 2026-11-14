#pragma once
#include <string_view>
#include "support.hpp"
#include "audio_capture_control.hpp"
namespace host {
struct StreamViewportDiagnostic {uint32_t nativeStage=0,nativeError=0,profileStage=0,nativeReason=0;bool consoleReturned=false;};
inline std::optional<StreamViewportDiagnostic> streamViewportDiagnostic(const Json& value){
  StreamViewportDiagnostic result;
  const auto integer=[&](const char* name,uint32_t maximum,uint32_t& target){const auto* number=std::get_if<double>(&value.get(name).value);if(!number||!std::isfinite(*number)||std::floor(*number)!=*number||*number<0||*number>maximum)return false;target=uint32_t(*number);return true;};
  const auto* fields=std::get_if<Json::Object>(&value.value);if(!fields)return {};
  if(fields->contains("nativeReason")&&!integer("nativeReason",255,result.nativeReason))return {};
  // 1..22 topology reasons, 23..38 fixed worker pre-change sites, 255 unknown.
  if(result.nativeReason>38&&result.nativeReason!=255)return {};
  if(fields->contains("consoleReturned")){const auto* bit=std::get_if<bool>(&value.get("consoleReturned").value);if(!bit)return {};result.consoleReturned=*bit;}
  if(!std::holds_alternative<bool>(value.get("ok").value)||!integer("nativeStage",9,result.nativeStage)||!integer("nativeError",UINT32_MAX,result.nativeError)||!integer("profileStage",7,result.profileStage)||(value.get("ok").boolean()&&(result.nativeStage||result.nativeError||result.profileStage||result.nativeReason)))return {};
  return result;
}
// Private pipe observations only. No arbitrary exception text, device identity,
// credentials, readiness or authorization is carried by this packet.
inline std::string streamPreparationPacket(const std::string& session,uint32_t stage,uint32_t error){
  if(session.empty()||session.size()>80||session.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos||!stage||stage>255)throw std::runtime_error("invalid_stream_diagnostic");
  return "{\"type\":\"worker.stream.failure\",\"sessionId\":"+jsonQuote(session)+",\"stage\":"+std::to_string(stage)+",\"error\":"+std::to_string(error)+'}';
}
// The worker sends observations through the same packet as failures: stage 41 (the capture followed a new
// mode), 42 (the GPU priority that was applied) and 43 (the GPU path that was chosen, gpu_capture.hpp). Those
// are ok records; only a GPU path that broke (reasons 5-8) and every other stage are failures. Recorded
// ok:false, the observations would read as streaming failures in every Host log.
inline bool streamObservationOk(uint32_t stage,uint32_t error)noexcept{
  if(stage==41||stage==42)return true;
  if(stage==43){const uint32_t reason=error&0xffu;return reason<5||reason>8;}
  // 44: a UAC secure desktop of this unlocked session. 1 the notice was shown, 2 the desktop came back
  // (seconds<<8), 5 the SYSTEM helper streamed the real prompt (secure_desktop_scope.hpp), 6/7/8 the helper could
  // not launch / was lost / produced no frame and the notice stood in. All of these continue the stream. 3 and 4
  // are its ends (locked, 10-minute limit), which end the stream.
  if(stage==44){const uint32_t what=error&0xffu;return what==1||what==2||(what>=5&&what<=8);}
  // 45: the way back to the GPU encoder (encoder_gpu_retry.hpp): an attempt began (1) or the stream is on the GPU
  // encoder again (2); 3, 4 and 5 are a failed attempt and the end of the attempts.
  if(stage==45){const uint32_t kind=error&0xffu;return kind==1||kind==2;}
  // 46: where input went (secure_desktop_scope.hpp, system_input.hpp): counts and levels only, every one an
  // observation. The stream is never ended by any of them.
  if(stage==46)return true;
  // 47: the stream's sound capture (audio_capture_control.hpp): started, restarting and a changed default
  // device are observations; a failed capture is a failure, which the capture itself answers by opening again.
  if(stage==audioCaptureRecordStage)return audioCaptureRecordOk(error);
  // 48: why a separated session's pointer was shown or hidden over a capture epoch (cursor_channel.hpp
  // CursorDiagnosis): counts only, all observations.
  if(stage==48)return true;
  return false;
}
// Stage 43 with reason 2 (gpu_capture.hpp gpuPathNoHardwareCapture): the capture ran on WARP or GDI, so the
// GPU path could not arm. The service answers it with one render adapter re-arm (native_display_service.hpp).
inline bool streamObservationNoHardwareCapture(uint32_t stage,uint32_t error)noexcept{return stage==43&&(error&0xffu)==2;}
inline std::optional<std::pair<uint32_t,uint32_t>> streamPreparationObservation(const Json& value,const std::string& session){
  const auto* fields=std::get_if<Json::Object>(&value.value);if(!fields||fields->size()!=4||value.get("type").string()!="worker.stream.failure"||session.empty()||value.get("sessionId").string()!=session)return {};
  const auto stage=value.get("stage").number(-1),error=value.get("error").number(-1);
  if(!std::isfinite(stage)||std::floor(stage)!=stage||stage<1||stage>255||!std::isfinite(error)||std::floor(error)!=error||error<0||error>UINT32_MAX)return {};
  return std::pair{uint32_t(stage),uint32_t(error)};
}
inline uint32_t streamCaptureFailureCode(const std::string& text){
  static const std::array<const char*,13> known{"No initial virtual display frame","Virtual display geometry changed; reconnect","Virtual display geometry changed; reconnect the session","Interactive desktop is locked, secure or unavailable","Desktop capture is unavailable","Selected display rectangle cuts a monitor","Capture monitor pixel geometry mismatch","Selected displays are not contiguous","Desktop capture failed; check the interactive desktop","frame_layout_unsupported","frame_memory_unavailable","frame_memory_insufficient","encoder_format_unsupported"};
  for(size_t i=0;i<known.size();++i)if(text==known[i])return uint32_t(i+1);return 0;
}
// Only fixed public codes from the negotiated v2 vocabulary cross the wire.
// Legacy peers and arbitrary native exception messages retain their category.
inline const char* streamCapturePublicFailure(std::string_view text,const char* fallback,bool v2)noexcept{
  if(v2)for(const auto* code:{"frame_layout_unsupported","frame_memory_unavailable","frame_memory_insufficient","encoder_format_unsupported"})if(text==code)return code;
  return fallback;
}
inline bool streamInitialSdkUnsupported(bool v2,bool current,bool failed,uint32_t stage,uint32_t error)noexcept{
  return v2&&current&&failed&&stage==4&&error==ERROR_NOT_SUPPORTED;
}
inline uint32_t streamPreparationReasonCode(const std::string& reason){
  static const std::array<const char*,8> known{"worker_failed","capture_failed","virtual_display_required","no_interactive_session","host_maintenance","encoder_failed","initial_viewport_failed","native_stream_profile_pending"};
  for(size_t i=0;i<known.size();++i)if(reason==known[i])return uint32_t(i+1);return 0;
}
inline uint32_t streamProfileFailureCode(const std::string& reason){
  static const std::array<const char*,17> known{"native_stream_profile_identity_changed","native_stream_profile_recovery_required","native_stream_profile_console_unverified","native_stream_profile_launch_unverified","native_stream_profile_entry_failed","native_stream_profile_access_missing","native_stream_profile_topology_changed","native_stream_profile_path_unavailable","native_stream_profile_prepare_unacknowledged","native_stream_profile_validate_failed","native_stream_profile_apply_failed","native_stream_profile_verification_expired","native_stream_profile_foreign_changed","native_stream_profile_complete_unacknowledged","native_stream_profile_query_failed","native_stream_profile_failed","native_stream_profile_pending"};
  for(size_t i=0;i<known.size();++i)if(reason==known[i])return uint32_t(i+1);return 0;
}
}
