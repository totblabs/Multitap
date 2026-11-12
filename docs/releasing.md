# Releasing (maintainers)

This repository is a copy exported from the development repository at every release. One release is one GitHub Release `vX.Y.Z`, where `X.Y.Z` is the broker version (`server/package.json`). The client, the Host and Android each have their own version, and every release attaches the newest file of each.

GitHub Actions (`.github/workflows/broker-image.yml`) builds the Docker image and pushes it to `ghcr.io/totblabs/multitap-broker` with `GITHUB_TOKEN`. The maintainer's PC needs neither Docker nor a registry token. No signing key is kept in CI; only the Docker update feed is signed, on the maintainer's PC.

## Once

- The repository name is in `publicRepository` (`totblabs/Multitap`) in `server/src/docker-update-check.mjs`, in `REPOSITORY` and `IMAGE_DEFAULT` in `deploy/docker/install.sh` and `deploy/docker/multitap`, and in the image name in `compose.yaml`, `compose.direct-https.yaml` and `.env.example`. A fork changes these places and pins its own server update key.
- After the first workflow run, make the package public: GitHub → Packages → `multitap-broker` → Package settings → Public. While it is private, `docker compose pull` needs a sign-in.
- The one-line install fetches `install.sh` from `main` and `compose.yaml`, `.env.example` and `deploy/docker/multitap` from the release tag, so every tag must contain them.

## Steps

### 1. Export

In the development repository, export the release commit to an empty folder and scan it (`tools/oss/`, not in this repository). The result must be 0 high and 0 private.

In the public working folder, keep only `.git`, replace the contents with the exported files, commit and push. Public commits are always made with the project's public identity: `ToTB` and the GitHub noreply address of the `totblabs` account (`241687781+totblabs@users.noreply.github.com`, the numeric ID is in that account's email settings).

When the release carries pull requests from this repository, add one more `-m` to the commit with a `Co-authored-by: Name <address>` line for each contributor (the address of their pull request's commits), as [CONTRIBUTING.md](../CONTRIBUTING.md) promises.

The public working folder's Git must not convert line endings (`git -C <public working folder> config core.autocrlf false`): the commit has to hold the exported bytes, and some files are CRLF on purpose.

```powershell
robocopy <exported folder> <public working folder> /MIR /XD .git
git -C <public working folder> add -A
git -C <public working folder> -c user.name=ToTB -c user.email=241687781+totblabs@users.noreply.github.com commit -m "Multitap X.Y.Z"
git -C <public working folder> push origin main
```

### 2. Tag

```powershell
git -C <public working folder> -c user.name=ToTB -c user.email=241687781+totblabs@users.noreply.github.com tag -a vX.Y.Z -m "Multitap X.Y.Z"
git -C <public working folder> push origin vX.Y.Z
```

### 3. Create the release

From the download list of the broker that published the installers, take the sizes, SHA-256, changelogs and release signatures, and gather the attachments in one folder. The tool runs the same checks as the workflow beforehand (signature against the pinned key, bundle rules).

```powershell
curl.exe -fsSo catalog.json https://<that broker>/api/v1/downloads
node tools/release/github-release-bundle.mjs stage catalog.json <client version> <Host version> <Android version> <new folder> <installer folder>...
```

On GitHub, create a release for tag `vX.Y.Z`, attach the 11 files from the new folder, write the notes (below) and publish it. A finished release shows 14 files: these 11, the two the workflow adds in step 4 and the update feed from step 5.

| Attachment | Contents |
| --- | --- |
| `multitap-release-input.json` | The download list entries of the five installers (changelogs, Windows release signatures) |
| `Multitap-<client>-windows-x64.exe`, `.exe.json` | Windows client and its build receipt |
| `Multitap-Host-<Host>-windows-x64.exe`, `.exe.json` | Host and its build receipt (`tools/release/host-installer-receipt.ps1` writes it) |
| `Multitap-<Android>-android-arm64-v8a.apk`, `.apk.verification.json` | Android and its verification receipt |
| `Multitap-<Android>-android-armeabi-v7a.apk`, `.apk.verification.json` | 〃 |
| `Multitap-<Android>-android-x86_64.apk`, `.apk.verification.json` | 〃 |

#### Release notes

Write the notes in English first, then Korean, from this release's section of `CHANGELOG.md` and `CHANGELOG.ko.md`. Say only what changes for the people who install and use it, and name each contributor whose pull request the release carries. Start with this table, because the file list alone does not say which of the 14 files a person needs:

```markdown
## Which file do I need?

| You want | Download |
| --- | --- |
| The app for a Windows PC | `Multitap-X.Y.Z-windows-x64.exe` |
| The app for an Android phone or tablet | `Multitap-X.Y.Z-android-arm64-v8a.apk` (most phones). `armeabi-v7a` is for older 32-bit devices, `x86_64` for emulators and x86 devices |
| The Host, for each Windows desktop | `Multitap-Host-X.Y.Z-windows-x64.exe` |
| The server | Nothing from this page. Install it with the one-line command in the README, and update it with `sudo multitap update` |

The `.json` files and the `.tar.gz` are for the updater and the server image. You do not need them.

A server you already run offers the same apps and Host on its own download page (`/download`).
```

Use each product's own version in the file names when they differ from the release's.

### 4. Watch the workflow

Under Actions → **Broker image**, watch the run for this release. When it succeeds:

- The image `ghcr.io/totblabs/multitap-broker:X.Y.Z` is pushed, and `:latest` too when it is the highest full release.
- `multitap-release-bundle-X.Y.Z.tar.gz` and `multitap-broker-image-X.Y.Z.json` are attached to the release.

If it fails, fix the attachments and run it again with **Run workflow** and the tag. It can be rebuilt until step 5.

### 5. Sign and attach the Docker update feed

This signs the image digest the workflow pushed, so it comes after step 4. Run it on the maintainer's PC that holds the server update key.

```powershell
curl.exe -fsSLo image.json https://github.com/totblabs/Multitap/releases/download/vX.Y.Z/multitap-broker-image-X.Y.Z.json
node tools/release/docker-feed.mjs server <production dependency folder> <changelog.txt> <server-update-private.pem> image.json <new folder>
```

- `<production dependency folder>`: a folder with the `node_modules` made by `npm ci --omit=dev --omit=optional --ignore-scripts` from this `server/package-lock.json`.
- The tool first checks that the key matches the public key pinned in the broker. After signing, it checks the result the way a Docker install does.

Attach `<new folder>/server-updates-docker.json` to the release. The workflow does not rebuild a version that has this file.

From step 3 until this step, the update check of Docker installs cannot find the feed in the new release and shows "check failed" for a while. Nothing else changes.

### 6. Check `docker compose pull`

On a Docker machine that is not signed in, check with the public `compose.yaml` in a new folder.

```sh
MULTITAP_VERSION=X.Y.Z docker compose pull
docker image inspect ghcr.io/totblabs/multitap-broker:X.Y.Z --format '{{index .RepoDigests 0}}'
```

The digest must equal `image` in `multitap-broker-image-X.Y.Z.json`. When X.Y.Z appears under **Server update** in the admin console of a Docker install on the previous version, you are done.
