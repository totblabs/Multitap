// Actual production decoder-limit candidate helper and metadata predicate only.
// No ServiceBridge, driver, encoder, capture, files, network or Windows API runs.
#include "stream_resize.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace host;
int checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
template<class Call> void refused(Call call, const char* message) {
  bool rejected = false;
  try { call(); } catch (...) { rejected = true; }
  require(rejected, message);
}
Json limits(int width, int height) {
  return JsonParser::parse("{\"maxWidth\":" + std::to_string(width) + ",\"maxHeight\":" + std::to_string(height) +
    ",\"maxPixels\":" + std::to_string(uint64_t(width) * uint64_t(height)) + ",\"maxPixelRate\":0}");
}
Json request(int sourceWidth, int sourceHeight, int boxWidth, int boxHeight) {
  auto value = JsonParser::parse(R"({"type":"stream.resize","sessionId":"00000000-0000-0000-0000-000000000001","requestId":"portrait-change","resizeId":"00000000-0000-0000-0000-000000000002","monitor":"0","epoch":2,"fps":60})");
  streamField(value, "width", sourceWidth); streamField(value, "height", sourceHeight);
  std::get<Json::Object>(value.value)["decoderLimits"] = limits(boxWidth, boxHeight);
  return value;
}
Json metadata(int encodedWidth, int encodedHeight, int sourceWidth, int sourceHeight, int fps = 60) {
  auto value = JsonParser::parse("{}");
  streamField(value, "width", encodedWidth); streamField(value, "height", encodedHeight);
  streamField(value, "sourceWidth", sourceWidth); streamField(value, "sourceHeight", sourceHeight);
  streamField(value, "fpsTarget", fps);
  return value;
}
void candidateSizes() {
  struct Case { int oldWidth, oldHeight, sourceWidth, sourceHeight, boxWidth, boxHeight, encodedWidth, encodedHeight; bool oldAccepts; };
  const Case cases[] = {
    {1430,1080,1080,1616,1080,1616,1080,1616,false}, // reported landscape -> portrait boundary
    {1080,1616,1430,1080,1430,1080,1430,1080,false}, // portrait -> landscape
    {800,600,1920,1080,1920,1080,1920,1080,false},
    {1920,1080,800,600,800,600,800,600,true},
    {1920,1080,5120,2880,5120,2880,5120,2880,false},
    {1430,1080,7680,4320,3840,2160,3840,2160,false}, // source remains larger than encoded picture
    {1920,1080,1280,720,640,360,640,360,true},
  };
  for (const auto& item : cases) {
    const std::optional<ViewportEncoderLimits> current = ViewportEncoderLimits{item.oldWidth, item.oldHeight};
    const auto command = request(item.sourceWidth, item.sourceHeight, item.boxWidth, item.boxHeight);
    const auto parsed = streamResizeRequest(command);
    const auto info = metadata(item.encodedWidth, item.encodedHeight, item.sourceWidth, item.sourceHeight);
    require(current->matches(info, parsed) == item.oldAccepts, "Old box no longer demonstrates the expected geometry boundary");
    const auto candidate = resizeViewportEncoderLimits(current, command, true);
    require(candidate && candidate->width == item.boxWidth && candidate->height == item.boxHeight, "Fresh exact decoder box was not selected");
    require(candidate->matches(info, parsed), "Supported new metadata rejected by its own candidate box");
    require(current->width == item.oldWidth && current->height == item.oldHeight, "Candidate eagerly committed or mutated the active box");
    require(parsed.width == item.sourceWidth && parsed.height == item.sourceHeight, "Encoded limits clipped physical source geometry");
  }
}
void retainedPolicy() {
  const std::optional<ViewportEncoderLimits> current = ViewportEncoderLimits{1430,1080};
  auto absent = request(1080,1616,1080,1616);
  std::get<Json::Object>(absent.value).erase("decoderLimits");
  for (const bool v2 : {false,true}) {
    const auto same = resizeViewportEncoderLimits(current, absent, v2);
    require(same && same->width == 1430 && same->height == 1080, "Absent optional limits changed active/legacy policy");
    require(!resizeViewportEncoderLimits({}, absent, v2), "Absent separate-v1 policy was manufactured");
  }
  const auto fresh = request(1080,1616,1080,1616);
  require(!resizeViewportEncoderLimits({}, fresh, true), "Decoder report manufactured separate-v1 policy for a session without one");
  refused([&] { (void)resizeViewportEncoderLimits(current, fresh, false); }, "Unnegotiated decoderLimits were accepted");
  auto malformed = fresh;
  std::get<Json::Object>(malformed.value)["decoderLimits"] = JsonParser::parse(R"({"maxWidth":1080,"maxHeight":1616,"maxPixels":1745280,"maxPixelRate":0,"extra":1})");
  refused([&] { (void)resizeViewportEncoderLimits(current, malformed, true); }, "Extra decoder capability field was accepted");
  std::get<Json::Object>(malformed.value)["decoderLimits"] = JsonParser::parse(R"({"maxWidth":1080,"maxHeight":1616,"maxPixels":1745280})");
  refused([&] { (void)resizeViewportEncoderLimits(current, malformed, true); }, "Missing decoder capability field was accepted");
  for (const auto& bad : {std::string(R"({"maxWidth":0,"maxHeight":1616,"maxPixels":1745280,"maxPixelRate":0})"),
                          std::string(R"({"maxWidth":1080,"maxHeight":1616.5,"maxPixels":1745280,"maxPixelRate":0})"),
                          std::string(R"({"maxWidth":1080,"maxHeight":1616,"maxPixels":0,"maxPixelRate":0})"),
                          std::string(R"({"maxWidth":1080,"maxHeight":1616,"maxPixels":1745280,"maxPixelRate":-1})")}) {
    std::get<Json::Object>(malformed.value)["decoderLimits"] = JsonParser::parse(bad);
    refused([&] { (void)resizeViewportEncoderLimits(current, malformed, true); }, "Malformed decoder capability was accepted");
  }
  require(current->width == 1430 && current->height == 1080, "Refused new limits changed the retained box");
}
void metadataStillVerified() {
  const auto command = request(1080,1616,1080,1616);
  const auto parsed = streamResizeRequest(command);
  const auto candidate = resizeViewportEncoderLimits(std::optional<ViewportEncoderLimits>{ViewportEncoderLimits{1430,1080}}, command, true);
  require(candidate.has_value(), "Missing candidate in metadata verification fixture");
  const auto good = metadata(1080,1616,1080,1616);
  require(candidate->matches(good, parsed), "Expected portrait metadata rejected");
  for (const auto& change : {std::pair<const char*,int>{"width",1082}, {"height",1618}, {"width",1079},
                             {"sourceWidth",1430}, {"sourceHeight",1080}, {"fpsTarget",30}}) {
    auto bad = good; streamField(bad, change.first, change.second);
    require(!candidate->matches(bad, parsed), "New box weakened encoded/source/fps metadata validation");
  }
  for (const auto* key : {"width","height","sourceWidth","sourceHeight","fpsTarget"}) {
    auto bad = good; std::get<Json::Object>(bad.value).erase(key);
    require(!candidate->matches(bad, parsed), "Missing required metadata field was accepted");
  }
  const ViewportEncoderLimits large{5120,2880};
  require(!large.matches(metadata(5120,2880,1080,1616), parsed), "Box allowed a picture larger than physical source");
}
}
int main() {
  try { candidateSizes(); retainedPolicy(); metadataStillVerified(); std::cout << checks << " checks, 0 failures\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
