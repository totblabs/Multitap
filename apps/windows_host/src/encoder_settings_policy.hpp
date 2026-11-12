#pragma once
#include "gpu_h264_policy.hpp"
#include <algorithm>
#include <string>
#include <vector>
namespace host {
struct EncoderSettingsChoice {std::string backend="auto",operationId,revision;std::optional<uint64_t> adapter;};
struct EncoderSettingsRequest {std::string backend,operationId,expectedRevision;std::optional<uint64_t> adapter;bool confirmed=false;};
inline bool encoderSettingsHex(const std::string& text,size_t size){return text.size()==size&&text.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline bool encoderSettingsUuid(const std::string& id){if(id.size()!=36)return false;for(size_t i=0;i<id.size();++i){if(i==8||i==13||i==18||i==23){if(id[i]!='-')return false;}else if(std::string("0123456789abcdef").find(id[i])==std::string::npos)return false;}return true;}
inline const char* encoderSettingsRequestError(const EncoderSettingsRequest& input){
 if(!input.confirmed)return "confirmation_required";
 if(!encoderSettingsUuid(input.operationId)||!encoderSettingsHex(input.expectedRevision,64)||(input.backend!="x264"&&input.backend!="mft"&&input.backend!="gpu"&&input.backend!="auto")||(input.backend!="gpu"&&input.adapter)||(input.backend=="gpu"&&(!input.adapter||!*input.adapter)))return "encoder_settings_invalid_request";
 return "";
}
inline const char* encoderSettingsPlan(const EncoderSettingsChoice& current,const EncoderSettingsRequest& input,const std::vector<uint64_t>& candidates){
 if(const std::string error=encoderSettingsRequestError(input);!error.empty())return encoderSettingsRequestError(input);
 if(input.backend=="gpu"&&(!input.adapter||!*input.adapter||std::find(candidates.begin(),candidates.end(),*input.adapter)==candidates.end()))return "encoder_adapter_changed";
 if(input.operationId==current.operationId)return input.backend==current.backend&&input.adapter==current.adapter?"duplicate":"encoder_settings_invalid_request";
 if(input.expectedRevision!=current.revision)return "encoder_settings_changed";
 return "apply";
}
}
