# Multitap mark and icons

The mark is a power strip ("멀티탭"): a filled rounded bar with three round
holes and a cord that leaves its left end and bends down. It was drawn for this
project on 2026-11-11 and is AGPL-3.0-only like the rest of the project.

## Sources (`src/`)

- `multitap-mark.svg`: the bare mark, large drawing, for 40 px and above. A
  120-unit canvas: bar x 24..112, y 38..84 (radius 23), holes of radius 7.5 at
  x 46, 68, 90, cord 10 wide.
- `multitap-mark-small.svg`: the small drawing for 32 px and below, with larger
  holes (radius 9) and a thicker cord (13), so the three holes stay apart.
- Both are one path filled non-zero: the bar, the holes wound the other way,
  and the cord as an outline (no stroke). The same path data works in SVG, an
  Android VectorDrawable and a Flutter `Path`. The file colour is the amber
  `#F5A623`; a page recolours it with a CSS mask.
- `windows/tile-<16|20|24|32|40|48|256>.svg`: the app icon. Amber tile
  `#F5A623` (corner radius 22.5 %), dark mark `#1E1503`. Each size up to 48 is
  its own drawing on whole pixels (holes of 2, 3, 3, 4, 5 and 6 px with at least
  1 px between them). 64, 128 and 256 come from the 256 master, which places
  the large drawing so that its weight sits on the centre of the tile.
- `windows/tray-<16|20|24|32|40|48>.svg`: the bare mark for the Host's
  notification-area icon, amber. The build also writes it in `#1E1503` for a
  light taskbar.
- `android/`: `background` (amber) and `foreground` (dark mark inside the safe
  area) of the adaptive icon, `monochrome` (themed icon), `symbol` (amber mark
  for the charcoal splash and launch background), `notification` (status bar,
  small drawing in a 24 dp box), `tv-banner` (320x180) and `legacy` (the tile,
  for the mipmap PNGs).

The four Windows products (client, Host, client setup, Host setup) share one
tile. `Brand` and
`ProductMark` in `apps/windows_client/lib/ui/components.dart` build the same
two paths for Flutter. Keep them together when the master changes.

## Where the mark has a tile

- With the amber tile: app icons (`.ico`, Android launcher and TV banner),
  favicons, the "클라이언트 정보" dialog.
- Bare, in one colour: next to the wordmark in the app header and sign-in, the
  admin console and browser Console headers, the Host tray, the Android status
  bar and themed icon. On a dark surface it is amber; on a light one it takes
  the deep amber ink of that surface.
- Wordmark: "Multitap" in Pretendard 700, letter-spacing -0.04em, one colour,
  with "for Proxmox VE" under it ("Proxmox VE" in amber).

## Build

Run from the repository root with the project Node runtime:

```powershell
& .tools/node-v24.22.0-win-x64/node.exe tools/icons/build-icons.mjs --frames .artifacts/icon-exports
```

It uses local headless Edge or Chrome and no npm packages. It writes the four
nine-size Windows ICOs, the two six-size tray ICOs, the Android mipmaps and
VectorDrawables, and byte copies of the SVGs: `multitap-mark.svg` to
`apps/windows_client/assets/brand/multitap.svg` and
`website/static/assets/mark.svg`, `multitap-mark-small.svg` to
`server/admin/mark.svg`, and `windows/tile-16.svg` to `server/admin/favicon.svg`
and `website/static/assets/favicon.svg`. `--frames` also writes every rasterised
frame and a manifest with each output hash. `--products` limits the output to
`client,host,client-setup,host-setup,tray,android,mark`.

Review the native 16/20/24/32 px frames as well as the large ones, and the
Android circular and squircle masks. Do not judge an icon only at an enlarged
preview. New artwork does not authorize replacing a published installer:
applications get it through their normal versioned build, verification and
publication.
