// The frame rate a narrow path still carries a picture at (path_frame_rate.hpp). Pure rules.
#include "path_frame_rate.hpp"
#include "bitrate_controller.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
long long checks = 0;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); ++checks; }
constexpr uint64_t second = 1000000;
}

int main() {
  try {
    using host::PathFrameRate;
    const int width = 2084, height = 1340; const double pixels = double(width) * height;
    const auto at = [&](double bpp, int rate) { return int(bpp * pixels * rate); };  // the target that gives a frame at `rate` this share
    {  // A target above the old floor of the stream's own rate changes nothing, however long.
      PathFrameRate p(144);
      for (uint64_t t = 0; t < 60 * second; t += 100000) require(p.decide(t, at(0.041, 144), width, height) == 144 && p.decide(t, 100000000, width, height) == 144, "A stream above its floor lost frames");
      require(p.lowest() == 30 && host::pathFrameLowest(20) == 20 && host::pathFrameLowest(240) == 30, "The lowest rate is not 30, or above the stream's own");
    }
    {  // Under the floor: at once to the highest rate whose frames get 0.05, not one rung at a time.
      PathFrameRate p(144);
      require(p.decide(0, 8000000, width, height) == 45, "8 Mbps at 2084x1340 is not paced at 45 (0.064 bit per pixel; 60 would get 0.048)");
      PathFrameRate q(144); require(q.decide(0, at(0.039, 144), width, height) == 90, "Just under the floor at 144 did not go to 90 (120 would get 0.047)");
      PathFrameRate r(144); require(r.decide(0, 2000000, width, height) == 30 && r.decide(1, 500000, width, height) == 30, "The lowest rate was passed");
      PathFrameRate s(60); require(s.decide(0, at(0.03, 60), 1920, 1080) == 60 && s.decide(0, int(0.03 * 1920 * 1080 * 60), 1920, 1080) == 30, "A 60 fps desktop under its floor did not go to 30 (45 would get 0.04)");
    }
    {  // Back up: only once the next rate's frames would have got 0.055 for two seconds, then to the highest rate whose frames do.
      PathFrameRate p(144); p.decide(0, 8000000, width, height);
      uint64_t t = second;
      for (; t < 20 * second; t += 100000) require(p.decide(t, at(0.054, 60), width, height) == 45, "Went up while the next rate's frames would get less than 0.055");
      const uint64_t began = t; int rate = 45;
      for (; t < began + 2 * second; t += 100000) rate = p.decide(t, at(0.056, 60), width, height);
      require(rate == 45, "Went up before two seconds");
      require(p.decide(t, at(0.056, 60), width, height) == 60, "Did not go up after two seconds, or went past the rate its frames get 0.055 at");
      // A dip in between starts the two seconds again.
      for (uint64_t k = 0; k < second; k += 100000) p.decide(t + k, at(0.056, 75), width, height);
      p.decide(t + second, at(0.05, 75), width, height);
      for (uint64_t k = second + 100000; k < 3 * second; k += 100000) require(p.decide(t + k, at(0.056, 75), width, height) == 60, "A dip did not start the wait again");
      require(p.decide(t + 3 * second + 100000, at(0.056, 75), width, height) == 75, "Did not go up two seconds after the dip");
      // A wide path again: the stream's own rate at once after the two seconds, not rung by rung.
      uint64_t u = t + 4 * second; int steps = 0, last = 75;
      for (; u < t + 10 * second; u += 100000) { const int now = p.decide(u, 60000000, width, height); if (now != last) { ++steps; last = now; } }
      require(last == 144 && steps == 1, "The stream's own rate did not come back in one step");
      // Enough for 90 but not for 120: 90.
      PathFrameRate q(144); q.decide(0, 4000000, width, height); require(q.fps() == 30, "4 Mbps is not paced at 30");
      for (uint64_t k = second; k <= 4 * second; k += 100000) q.decide(k, at(0.056, 90), width, height);
      require(q.fps() == 90, "Did not go to the highest rate whose frames get 0.055");
    }
    {  // No flapping: a target that swings 30 % around a threshold does not take the rate up and down with it.
      PathFrameRate p(144); int changes = 0, last = 144; uint64_t lastChange = 0, shortest = ~0ull;
      for (uint64_t t = 0; t < 120 * second; t += 100000) {
        const double swing = 1 + 0.3 * ((t / 700000) % 2 ? 1 : -1);
        const int now = p.decide(t, int(at(0.045, 144) * swing), width, height);
        if (now != last) { if (now > last && changes) shortest = std::min(shortest, t - lastChange); ++changes; last = now; lastChange = t; }
        // What the paced frames get never stays under the floor: either the rate is the lowest, or the next call fixes it.
        require(now == 30 || double(at(0.045, 144)) * swing / (pixels * now) >= host::pathFrameFloorBpp - 1e-9, "A paced frame got less than the floor");
      }
      require(shortest >= 2 * second - 100000, "The rate went up sooner than two seconds after a change");
    }
    {  // A stream asked for at an odd rate keeps it as its top, with the ladder's rates below.
      PathFrameRate p(100); require(p.decide(0, at(0.03, 100), width, height) == 60, "100 fps under its floor did not go to 60 (0.05; 75 and 90 would get less)");
      for (uint64_t t = second; t < 20 * second; t += 100000) p.decide(t, 60000000, width, height);
      require(p.fps() == 100, "The stream's own odd rate did not come back");
      PathFrameRate low(24); require(low.decide(0, 100000, width, height) == 24 && low.lowest() == 24, "A stream under 30 fps was paced lower");
    }
    {  // The highest rate the stream can be paced at changes under it (another encoder, the Host's load).
      PathFrameRate p(144);
      require(p.decide(0, 60000000, width, height, 60) == 60 && p.decide(1, 60000000, width, height, 60) == 60, "A lower top was not followed");
      require(p.decide(2, 60000000, width, height, 144) == 144, "A stream at its top did not follow the top back up");
      // Lowered by the path, a higher top does not raise it: the two seconds do.
      require(p.decide(3, 8000000, width, height, 144) == 45 && p.decide(4, 8000000, width, height, 60) == 45 && p.decide(5, 8000000, width, height, 144) == 45, "A change of the top raised a rate the path had lowered");
      require(p.decide(6, 8000000, width, height, 30) == 30, "A top under the paced rate was not followed");
      PathFrameRate q(144); q.decide(0, 8000000, width, height, 144);
      for (uint64_t t = second; t <= 4 * second; t += 100000) q.decide(t, 60000000, width, height, 90);
      require(q.fps() == 90 && q.lowest() == 30, "The way back went past the top, or the lowest rate moved with it");
    }
    {  // The floor of the automatic rate with the ladder is the lowest rate's (bitrate_controller.hpp).
      host::BitrateController c(40000000, 100000000, 144); c.geometry(width, height, 144);
      require(c.floor() == host::adaptiveBitrateFloor(width, height, 144) && c.floor() > 16000000, "The floor at 144 fps is not the old one");
      c.geometry(width, height, 144, 30);
      require(c.floor() == host::adaptiveBitrateFloor(width, height, 30) && c.floor() < 3400000, "The floor with the ladder is not the lowest rate's");
      c.geometry(width, height, 144);
      require(c.floor() > 16000000, "A new stream kept the floor of the last one's ladder");
      c.geometry(width, height, 20, 30);
      require(c.floor() == host::adaptiveBitrateFloor(width, height, 20), "A lowest rate above the stream's own lowered nothing, yet the floor moved");
    }
    std::printf("{\"kind\":\"path-frame-rate\",\"checks\":%lld,\"passed\":true}\n", checks);
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "path frame rate check failed: %s\n", error.what()); return 1; }
}
