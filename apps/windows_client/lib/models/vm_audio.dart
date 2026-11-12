/// The broker's VM sound-device contract, named in a desktop's
/// `vmAudioProtocol`. The client only reads the device's state from the
/// desktop list (the tile's 소리 없음 / 다시 시작 필요 badge); it adds none
/// (2026-11-10: the administrator adds an audio device in Proxmox).
const vmAudioProtocol = 'vm-audio-v1';
