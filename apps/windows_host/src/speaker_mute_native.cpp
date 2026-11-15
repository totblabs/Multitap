// speaker-mute-v1: the Windows audio objects (speaker_mute_native.hpp).
#include "speaker_mute_native.hpp"
#include "display_scale.hpp"
#include <mmdeviceapi.h>
#include <endpointvolume.h>

namespace host {
SpeakerMuteAccess nativeSpeakerMuteAccess() {
  struct State { Com<IMMDeviceEnumerator> enumerator; HRESULT error = S_OK; std::filesystem::path path; };
  const auto state = std::make_shared<State>();
  state->path = speakerMuteRecordPath();
  const auto devices = [state]() -> IMMDeviceEnumerator* {
    if (!state->enumerator) state->error = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&state->enumerator));
    return state->enumerator.Get();
  };
  // The endpoint's volume control, only where the endpoint is there now (active).
  const auto volume = [state, devices](const std::wstring& id) {
    Com<IAudioEndpointVolume> control;
    auto* enumerator = devices();
    if (!enumerator) return control;
    Com<IMMDevice> device; state->error = enumerator->GetDevice(id.c_str(), &device);
    if (FAILED(state->error) || !device) return control;
    state->error = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(control.GetAddressOf()));
    if (FAILED(state->error)) control.Reset();
    return control;
  };
  SpeakerMuteAccess access;
  access.defaultEndpoint = [state, devices]() -> std::optional<std::wstring> {
    auto* enumerator = devices();
    if (!enumerator) return std::nullopt;
    Com<IMMDevice> device; state->error = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (state->error == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return std::wstring();  // Windows has no render endpoint
    if (FAILED(state->error) || !device) { state->enumerator.Reset(); return std::nullopt; }  // asked again with a new enumerator
    LPWSTR id = nullptr; state->error = device->GetId(&id);
    if (FAILED(state->error) || !id) return std::nullopt;
    std::wstring text(id); CoTaskMemFree(id);
    if (!speakerMuteEndpointId(text)) { state->error = E_UNEXPECTED; return std::nullopt; }
    return text;
  };
  access.muted = [state, volume](const std::wstring& id) -> std::optional<bool> {
    const auto control = volume(id);
    if (!control) return std::nullopt;
    BOOL muted = FALSE; state->error = control->GetMute(&muted);
    if (FAILED(state->error)) return std::nullopt;
    return muted != FALSE;
  };
  access.setMute = [state, volume](const std::wstring& id, bool mute) {
    const auto control = volume(id);
    if (!control) return false;
    state->error = control->SetMute(mute ? TRUE : FALSE, nullptr);
    return SUCCEEDED(state->error);
  };
  access.lastError = [state] { return int32_t(state->error); };
  access.load = [state]() -> std::optional<std::string> { try { return DisplayScaleRecordFile(state->path).load(); } catch (...) { return std::nullopt; } };
  access.save = [state](const std::string& text) { try { return DisplayScaleRecordFile(state->path).save(text); } catch (...) { return false; } };
  access.self = speakerMuteSelf;
  access.alive = speakerMuteOwnerAlive;
  return access;
}

std::vector<uint32_t> speakerMuteRecover() noexcept {
  try {
    const auto text = DisplayScaleRecordFile(speakerMuteRecordPath()).load();
    if (!text || text->empty()) return {};
    const auto held = speakerMuteLock();
    if (!held) return {};
    return SpeakerMuteKeeper(nativeSpeakerMuteAccess()).recover();
  } catch (...) { return {}; }
}
}  // namespace host
