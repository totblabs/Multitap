#pragma once
#include "support.hpp"
#include "remote_pointer.hpp"
#include <cmath>
namespace host {
inline bool validDirectInput(const Json& input,bool relative=false){
  const auto whole=[](const Json& value,double low,double high){const auto* n=std::get_if<double>(&value.value);return n&&std::isfinite(*n)&&std::floor(*n)==*n&&*n>=low&&*n<=high;};
  const auto boolean=[](const Json& value){return std::holds_alternative<bool>(value.value);};
  const auto type=input.get("type").string();if(type=="input.reset")return true;if(type!="input")return false;
  const auto* fields=std::get_if<Json::Object>(&input.value);if(!fields)return false;
  const auto event=input.get("event").string();
  if(event=="key")return whole(input.get("vk"),1,255)&&whole(input.get("scan"),0,65535)&&boolean(input.get("down"))&&boolean(input.get("extended"));
  if(event=="moveRelative")return relative&&whole(input.get("dx"),-relativeMouseMaximum,relativeMouseMaximum)&&whole(input.get("dy"),-relativeMouseMaximum,relativeMouseMaximum)&&(input.get("dx").number()!=0||input.get("dy").number()!=0)&&!fields->contains("x")&&!fields->contains("y");
  const auto* x=std::get_if<double>(&input.get("x").value);const auto* y=std::get_if<double>(&input.get("y").value);
  const bool bare=relative&&!fields->contains("x")&&!fields->contains("y");
  if(!bare&&(!x||!y||!std::isfinite(*x)||!std::isfinite(*y)||*x<0||*x>1||*y<0||*y>1))return false;
  const auto button=input.get("button").string();return (event=="move"&&!bare)||(event=="button"&&(button=="left"||button=="right"||button=="middle"||button=="x1"||button=="x2")&&boolean(input.get("down")))||(event=="wheel"&&whole(input.get("delta"),-12000,12000));
}
}
