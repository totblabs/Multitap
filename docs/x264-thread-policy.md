# Optional Host x264 thread policy

The administrator-protected `%ProgramData%\Multitap\Host\encoder.json` may contain
`"x264Threads": 2` beside its encoder/adapter selection. Only integer values
from 1 through 8 are applied. A missing, untrusted (writable by anyone other
than SYSTEM and Administrators), unreadable, malformed or larger-than-4096-byte
file keeps the automatic choice: one slice thread per affinity-visible
processor, capped at 8. Invalid field types or values also keep the automatic
choice. Reading the policy writes nothing, and saving the encoder choice in
Host settings preserves this key. Code: `apps/windows_host/src/x264_policy.hpp`
and `configuredX264Settings()` in `x264_encoder.cpp`.

The policy is read whenever a production x264 encoder is constructed: initial
CPU selection, fallback after a GPU encoder fails to start, resize/topology
recreation and runtime GPU-to-CPU fallback. It is not reloaded on every frame.
Explicit `Encoder(X264Settings)` benchmark settings stay independent. x264 may
use fewer threads for small pictures; the value is an upper bound for that
encoder, not a CPU budget for capture, conversion, input, audio, Windows
applications or several simultaneous output encoders. The Microsoft MFT
fallback is outside this x264-only limit. The requested frame rate is
unchanged.

`StreamPriority` is unchanged: the streaming worker raises its process
priority to ABOVE_NORMAL and opts out of EcoQoS while a stream runs, and
restores both when the last stream ends. Input shares that process, so
lowering its priority is not part of this limit.

`MultitapHostX264PolicyTest` (a target of the Host's CMake project in an
x264 build) checks the protected-reader rejection and the bounds, then encodes
synthetic pixels with caps 1, 2 and 8 and decodes them with the client's
decoder. An injected GPU failure verifies that the replacement x264 encoder
starts and follows the same policy without touching a GPU or machine settings.
