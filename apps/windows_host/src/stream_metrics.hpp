#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <ostream>

namespace host {
struct FrameStageTiming {
  double topologyMs = 0, captureMs = 0, convertMs = 0, encodeMs = 0;
};
class StreamMetricsWindow {
 public:
  void observe(FrameStageTiming timing) {
    const std::array<double,4> values{timing.topologyMs,timing.captureMs,timing.convertMs,timing.encodeMs};
    if (count_ == work_.size() || std::any_of(values.begin(),values.end(),[](double n){return !std::isfinite(n)||n<0||n>60000;})) {
      invalid_ = true; return;
    }
    double work = 0;
    for (double value : values) work += value;
    if (work > 60000) { invalid_=true; return; }
    for (std::size_t i=0;i<values.size();++i) { sums_[i]+=values[i]; maxima_[i]=std::max(maxima_[i],values[i]); }
    work_[count_++] = work;
  }
  void writeJson(std::ostream& out) const {
    if (!count_ || invalid_) return;
    auto sorted = work_;
    std::sort(sorted.begin(),sorted.begin()+count_);
    const double total=sums_[0]+sums_[1]+sums_[2]+sums_[3];
    out << ",\"pipeline\":{\"sampleCount\":" << count_
        << ",\"topologyAvgMs\":" << sums_[0]/count_ << ",\"captureAvgMs\":" << sums_[1]/count_
        << ",\"convertAvgMs\":" << sums_[2]/count_ << ",\"encodeAvgMs\":" << sums_[3]/count_
        << ",\"workAvgMs\":" << total/count_ << ",\"workP95Ms\":" << sorted[(count_*95+99)/100-1]
        << ",\"workMaxMs\":" << sorted[count_-1]
        // Which stage a slow frame spent its time in (a p95 spike alone does not say).
        << ",\"topologyMaxMs\":" << maxima_[0] << ",\"captureMaxMs\":" << maxima_[1]
        << ",\"convertMaxMs\":" << maxima_[2] << ",\"encodeMaxMs\":" << maxima_[3] << '}';
  }
  void reset() { count_=0; sums_={}; maxima_={}; invalid_=false; }
 private:
  std::array<double,256> work_{};
  std::array<double,4> sums_{}, maxima_{};
  std::size_t count_ = 0;
  bool invalid_ = false;
};
}
