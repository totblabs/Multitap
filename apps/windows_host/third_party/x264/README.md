# x264 build inputs (Host production encoder)

The Host links x264 (GNU GPL v2 or later) statically as its streaming H.264
encoder, with automatic fallback to the Microsoft software MFT
(`docs/x264-encoder.md`). The x264 source is **not vendored** and is used
**unmodified**: the build takes a pinned upstream checkout and NASM from outside
the repository and refuses anything else.

| Input | Pinned value |
|---|---|
| x264 repository | `https://code.videolan.org/videolan/x264.git`, branch `stable` |
| x264 commit | `b35605ace3ddf7c1a5d67a2eb553f034aef41d55` (2025-06-08, `0.165.3222 b35605a`, API build 165) |
| NASM | 2.16.03 win64, `nasm.exe` SHA-256 `a93276636266516421cc9b422f47476c21f7a2949f1ae251556b2f1d33a3be04` (from `https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip`) |

Prepare once (outside the repository, or in the git-ignored `.tools/`):

```powershell
git clone --branch stable --single-branch https://code.videolan.org/videolan/x264.git .tools/x264
git -C .tools/x264 checkout b35605ace3ddf7c1a5d67a2eb553f034aef41d55
# Extract nasm-2.16.03-win64.zip so that .tools/nasm-2.16.03/nasm.exe exists.
```

Build: `tools/host-installer/build-native.ps1` (it passes the two `.tools` paths
above), or CMake with `-DMULTITAP_X264_SRC=<checkout> -DMULTITAP_NASM=<nasm.exe>`.
CMake checks that the checkout is exactly the pinned commit with no modified,
untracked or ignored files. Omitting both CMake paths builds an MFT-only Host.

Files here are Multitap build glue, not x264 files:

- `x264.cmake` builds libx264 with the object lists and flags of x264's own
  `Makefile`/`configure` for Windows x86_64, 8-bit, no CLI, no OpenCL.
- `config/config.h`, `config/x264_config.h` are hand-written equivalents of
  what `./configure --bit-depth=8 --chroma-format=all --disable-cli
  --disable-opencl --enable-static` writes for MSVC.

Licence obligations met by the Host package (flat in the installed `bin/`):
`THIRD-PARTY-NOTICES.txt` (version, commit, upstream, licence, how it is used),
`COPYING.x264.txt` (the GPL v2 text from the checkout) and
`host-native-corresponding-source-<version>.zip` (the Host sources with the
complete source of the pinned x264 commit and these build files). The
Host executables contain x264, used under GPL version 3 as its "v2 or later"
licence allows; Multitap's own code is AGPL-3.0, and section 13 of both
licences permits linking the two.
Whoever distributes a Host package must also offer the corresponding source of
the Multitap Host itself.
