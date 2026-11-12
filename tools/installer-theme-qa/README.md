# Product icon QA

`product-icon-qa.ps1` runs in an x64 MSVC/Windows SDK build environment. It
compiles the real Flutter runner resources in Host Settings and client modes,
checks their product and executable names, and draws the installer's shared
resource-based product icon helper (`apps/windows_host/src/setup_product_icon.hpp`)
into offscreen bitmaps at 100/150/200 % DPI. It checks the tile, bar, hole and
cord pixels, repeated-paint handle counts, and a missing-resource case. It
creates no windows and installs nothing. Results and PNGs go to
`.artifacts/host-brand-audit` (`-OutputDirectory` overrides it).
