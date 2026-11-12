/* audio-opus-v1 fixture for the Android client's JVM check
 * (android/test/AndroidOpusAudioCheck.kt). Encodes a fixed synthetic signal
 * with the Host's exact encoder settings (apps/windows_host/src/opus_audio.hpp:
 * RESTRICTED_LOWDELAY, 128 kbps VBR, DTX and in-band FEC off, 10 ms frames,
 * 48 kHz stereo) with libopus 1.6.1, and decodes every packet with libopus.
 *
 * Fixture (little endian): "TVOPUSF1", u32 packet count, u32 lookahead; per
 * packet u16 length + the Opus packet; then per packet 1920 bytes of the
 * libopus decode (480 s16 stereo frames). stdout: bitrate and SNR over
 * `seconds` of the same signal.
 *
 * Usage: opus_host_fixture <fixture-out> <packets> [seconds]
 * opus-host-fixture.bin is `opus_host_fixture opus-host-fixture.bin 20 30`,
 * built with MSVC against the client's libopus 1.6.1 (native/third_party/opus):
 *   cl /O2 /MD /D_USE_MATH_DEFINES /I<opus>/include opus_host_fixture.c opus.lib
 * Its stdout then: opus_kbps=128.8 media_packet_kbps=140.0 (the media packets
 * with their 12-byte envelope and 2-byte sequence) against pcm_kbps=1545.6.
 */
#include <opus.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define FRAME 480

static uint32_t rng = 12345u;
static double noise(void) { rng = rng * 1664525u + 1013904223u; return ((double)(rng >> 8) / 16777216.0) * 2.0 - 1.0; }

/* Music-like: two chords (sine partials), a slow sweep, and noise. */
static void signal(long long n, int16_t* out) {
  const double t = (double)n / RATE;
  double l = 0.18 * sin(2 * M_PI * 220.0 * t) + 0.12 * sin(2 * M_PI * 277.18 * t) + 0.10 * sin(2 * M_PI * 329.63 * t);
  double r = 0.18 * sin(2 * M_PI * 246.94 * t) + 0.12 * sin(2 * M_PI * 311.13 * t) + 0.10 * sin(2 * M_PI * 369.99 * t);
  const double sweep = 0.08 * sin(2 * M_PI * (500.0 * t + 400.0 * t * t));
  l += sweep + 0.03 * noise();
  r += sweep + 0.03 * noise();
  out[0] = (int16_t)lrint(l * 32767.0);
  out[1] = (int16_t)lrint(r * 32767.0);
}

static OpusEncoder* host_encoder(void) {
  int error = OPUS_OK;
  OpusEncoder* e = opus_encoder_create(RATE, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
  if (error != OPUS_OK || !e) return NULL;
  if (opus_encoder_ctl(e, OPUS_SET_BITRATE(128000)) != OPUS_OK || opus_encoder_ctl(e, OPUS_SET_VBR(1)) != OPUS_OK ||
      opus_encoder_ctl(e, OPUS_SET_DTX(0)) != OPUS_OK || opus_encoder_ctl(e, OPUS_SET_INBAND_FEC(0)) != OPUS_OK) return NULL;
  return e;
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <fixture-out> <packets> [seconds]\n", argv[0]); return 2; }
  const int packets = atoi(argv[2]);
  const int seconds = argc > 3 ? atoi(argv[3]) : 10;
  OpusEncoder* enc = host_encoder();
  int error = OPUS_OK;
  OpusDecoder* dec = opus_decoder_create(RATE, 2, &error);
  if (!enc || !dec) { fprintf(stderr, "libopus refused\n"); return 1; }
  opus_int32 lookahead = 0;
  opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

  /* Bitrate and SNR over `seconds` (the first `packets` frames are the fixture). */
  const int frames = seconds * 100 > packets ? seconds * 100 : packets;
  unsigned char** pkt = calloc((size_t)packets, sizeof(*pkt));
  int* len = calloc((size_t)packets, sizeof(int));
  int16_t* pcm = malloc((size_t)packets * FRAME * 2 * sizeof(int16_t));
  int16_t* source = malloc((size_t)frames * FRAME * 2 * sizeof(int16_t));
  int16_t* decoded = malloc((size_t)frames * FRAME * 2 * sizeof(int16_t));
  long long bytes = 0; int smallest = 1 << 30, largest = 0;
  for (int f = 0; f < frames; ++f) {
    int16_t* in = source + (size_t)f * FRAME * 2;
    for (int i = 0; i < FRAME; ++i) signal((long long)f * FRAME + i, in + 2 * i);
    unsigned char out[1275];
    const int n = opus_encode(enc, in, FRAME, out, (opus_int32)sizeof(out));
    if (n <= 0) { fprintf(stderr, "encode failed at %d: %d\n", f, n); return 1; }
    if (opus_packet_get_nb_samples(out, n, RATE) != FRAME) { fprintf(stderr, "not 10 ms\n"); return 1; }
    bytes += n; if (n < smallest) smallest = n; if (n > largest) largest = n;
    int16_t* o = decoded + (size_t)f * FRAME * 2;
    if (opus_decode(dec, out, n, o, FRAME, 0) != FRAME) { fprintf(stderr, "decode failed\n"); return 1; }
    if (f < packets) {
      pkt[f] = malloc((size_t)n); memcpy(pkt[f], out, (size_t)n); len[f] = n;
      memcpy(pcm + (size_t)f * FRAME * 2, o, FRAME * 2 * sizeof(int16_t));
    }
  }
  /* SNR of the decode against the source, aligned by the encoder's lookahead. */
  double sig = 0, err = 0;
  const long long total = (long long)frames * FRAME;
  for (long long i = RATE / 10; i + lookahead < total; ++i) {
    for (int c = 0; c < 2; ++c) {
      const double s = source[i * 2 + c], d = decoded[(i + lookahead) * 2 + c];
      sig += s * s; err += (s - d) * (s - d);
    }
  }
  const double opusKbps = bytes * 8.0 / frames / 10.0;            /* per 10 ms frame -> kbps */
  const double wireKbps = (bytes + 14.0 * frames) * 8.0 / frames / 10.0; /* + media envelope 12 + sequence 2 */
  printf("frames=%d seconds=%.2f lookahead=%d opus_bytes_min=%d max=%d avg=%.1f\n", frames, frames / 100.0, lookahead, smallest, largest, (double)bytes / frames);
  printf("opus_kbps=%.1f media_packet_kbps=%.1f pcm_kbps=%.1f snr_db=%.1f\n", opusKbps, wireKbps, (1920.0 + 12.0) * 8 * 100 / 1000, 10 * log10(sig / err));

  FILE* file = fopen(argv[1], "wb");
  if (!file) return 1;
  fwrite("TVOPUSF1", 1, 8, file);
  uint32_t count = (uint32_t)packets, look = (uint32_t)lookahead;
  fwrite(&count, 4, 1, file); fwrite(&look, 4, 1, file);
  for (int f = 0; f < packets; ++f) { uint16_t l = (uint16_t)len[f]; fwrite(&l, 2, 1, file); fwrite(pkt[f], 1, (size_t)len[f], file); }
  fwrite(pcm, sizeof(int16_t), (size_t)packets * FRAME * 2, file);
  fclose(file);
  opus_encoder_destroy(enc); opus_decoder_destroy(dec);
  return 0;
}
