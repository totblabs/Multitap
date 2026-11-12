#ifndef MULTITAP_MEDIA_PUSH_H_
#define MULTITAP_MEDIA_PUSH_H_
#include <windows.h>
namespace multitap {
// Why a streaming engine did or did not take one relay media packet.
// `full` is transient backpressure: the decoder worker frees space as it
// decodes, so the caller offers the same packet again a little later (a burst
// of packets that waited in the network during a short stall arrives faster
// than they decode). `stopped` (the decoder ended) and `invalid` end a stream.
enum class MediaPushResult { accepted, full, stopped, invalid };

// HRESULTs of RemoteMediaSession::Push; the runner's push channel reports them
// as media_queue_full / decoder_failed / invalid_stream_message.
inline constexpr HRESULT kMediaQueueFull = E_PENDING;
inline constexpr HRESULT kMediaDecoderStopped = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, ERROR_INVALID_STATE);
inline constexpr HRESULT kMediaPacketInvalid = E_INVALIDARG;

inline HRESULT MediaPushHresult(MediaPushResult result) {
  switch (result) {
    case MediaPushResult::accepted: return S_OK;
    case MediaPushResult::full: return kMediaQueueFull;
    case MediaPushResult::stopped: return kMediaDecoderStopped;
    default: return kMediaPacketInvalid;
  }
}
}
#endif
