import { HttpError } from './errors.mjs';
import { ServerUpdateClient } from './server-update.mjs';
import { compareVersion, decodeFeed, feedUrl } from '../update/protocol.mjs';

// docker-release-check-v1. A Docker Compose installation has no root updater:
// the broker only reads the project's public, signed server feed and tells the
// administrator the exact commands. It never pulls, restarts or applies
// anything itself. The native install keeps its own updater (server-update.mjs).
//
// Feed: the existing server update envelope {release, signature}
// (update/protocol.mjs), platform linux-x64-docker, signed with the server
// update key pinned below (the same key the native updater is configured with).
// Only the metadata is read; the release's artifact is never downloaded here.

// The project's Ed25519 server-update public key (public half only).
export const serverUpdatePublicKey = "-----BEGIN PUBLIC KEY-----\nMCowBQYDK2VwAyEAh9QWZRImt31KX/fWrhKV2FpSol+RjYPjAp1IO0JI2yY=\n-----END PUBLIC KEY-----\n";
// PUBLICATION CONSTANT: the public GitHub repository ("owner/name"). The Docker
// update feed (its Releases) and the published image name follow it; the image
// name is repeated in compose.yaml, compose.direct-https.yaml and .env.example
// (a test keeps them equal). A fork changes this line and those files.
export const publicRepository = 'totblabs/Multitap';
export const defaultDockerUpdateFeed = `https://github.com/${publicRepository}/releases/latest/download/server-updates-docker.json`;
// GHCR names are lower case (.github/workflows/broker-image.yml pushes here).
export const publicImageRepository = `ghcr.io/${publicRepository.split('/')[0].toLowerCase()}/multitap-broker`;
export const dockerUpdatePlatform = 'linux-x64-docker';

const placeholder = feed => /[<>]|%3C|%3E/i.test(feed);
// MULTITAP_DOCKER_UPDATE_FEED: unset/empty = the project feed, `off` = no
// outbound check, otherwise an https URL without credentials, query or fragment.
export function dockerUpdateFeed(value) {
  if (value === undefined || value === '') return placeholder(defaultDockerUpdateFeed) ? null : defaultDockerUpdateFeed;
  if (value === 'off') return null;
  try { return feedUrl(value).href; } catch { throw Error('invalid_docker_update_feed'); }
}

const redirects = new Set([301, 302, 303, 307, 308]);
const failure = error => /^update_[a-z_]{1,60}$/.test(error?.message) ? error.message : 'update_download_failed';

// GET with at most `maxRedirects` https redirects (GitHub's latest/download
// link redirects twice), a 32 KiB body cap and one overall deadline. The
// signature, not the transport, decides whether the content is trusted.
export async function fetchFeed(url, { fetcher = fetch, timeoutMs = 8000, maxRedirects = 4, limit = 32768, userAgent = 'Multitap-Broker', allowInsecureHttp = false } = {}) {
  const controller = new AbortController(), timer = setTimeout(() => controller.abort(), timeoutMs); timer.unref?.();
  const allowed = target => (target.protocol === 'https:' || (allowInsecureHttp === true && target.protocol === 'http:')) && !target.username && !target.password;
  try {
    let target = new URL(url);
    if (!allowed(target)) throw Error('update_feed_invalid');
    for (let hop = 0; ; hop++) {
      const response = await fetcher(target.href, { redirect: 'manual', signal: controller.signal, headers: { Accept: 'application/json', 'User-Agent': userAgent } });
      if (redirects.has(response.status)) {
        await response.body?.cancel?.().catch(() => {});
        const location = response.headers.get('location');
        if (hop >= maxRedirects || !location) throw Error('update_feed_redirect_invalid');
        let next;
        try { next = new URL(location, target); } catch { throw Error('update_feed_redirect_invalid'); }
        if (!allowed(next)) throw Error('update_feed_redirect_invalid');
        next.hash = ''; target = next; continue;
      }
      if (response.status !== 200 || !response.body) { await response.body?.cancel?.().catch(() => {}); throw Error('update_download_failed'); }
      const length = response.headers.get('content-length');
      if (length !== null && (!/^\d+$/.test(length) || Number(length) > limit)) throw Error('update_download_size');
      const chunks = []; let total = 0;
      for await (const chunk of response.body) {
        total += chunk.length;
        if (total > limit) { controller.abort(); throw Error('update_download_size'); }
        chunks.push(Buffer.from(chunk));
      }
      return Buffer.concat(chunks, total);
    }
  } catch (error) { throw Error(failure(error)); }
  finally { clearTimeout(timer); }
}

// The commands an operator runs in the folder of compose.yaml. The version is
// a validated x.y.z, so it is safe inside the shell line.
export function dockerUpdateCommands(version) {
  compareVersion(version, version);
  return {
    kind: 'docker-compose', version,
    commands: [`sed -i '/^MULTITAP_VERSION=/d' .env && echo 'MULTITAP_VERSION=${version}' >> .env`, 'docker compose pull && docker compose up -d'],
    sourceCommands: ['git pull', 'docker compose -f compose.yaml -f compose.build.yaml up -d --build'],
  };
}

export class DockerReleaseCheck {
  constructor({ feed, currentVersion, publicKey = serverUpdatePublicKey, fetcher = fetch, now = Date.now, ttlMs = 60 * 60 * 1000, retryMs = 60 * 1000, forcedMs = 10 * 1000, waitMs = 3000, timeoutMs = 8000, allowInsecureHttp = false }) {
    compareVersion(currentVersion, currentVersion);
    Object.assign(this, { feed: feedUrl(feed, { allowInsecureHttp }).href, currentVersion, publicKey, fetcher, now, ttlMs, retryMs, forcedMs, waitMs, timeoutMs, allowInsecureHttp });
    this.state = { phase: 'idle' }; this.inflight = null; this.finishedAt = 0;
  }
  status() {
    const s = this.state, result = { state: s.phase, feedConfigured: true, applySupported: false, channel: 'docker-release-feed', currentVersion: this.currentVersion };
    if (s.checkedAt) result.checkedAt = s.checkedAt;
    if (s.errorCode) result.errorCode = s.errorCode;
    if (s.release) {
      result.latest = { version: s.release.version, releasedAt: s.release.releasedAt, changelog: s.release.changelog };
      result.releaseId = s.releaseId;
      if (s.phase === 'available') result.manual = dockerUpdateCommands(s.release.version);
    }
    return result;
  }
  // One request at a time for every administrator session.
  start() {
    if (this.inflight) return this.inflight;
    this.state = { ...this.state, phase: 'checking' };
    this.inflight = (async () => {
      try {
        const bytes = await fetchFeed(this.feed, { fetcher: this.fetcher, timeoutMs: this.timeoutMs, userAgent: 'Multitap-Broker/' + this.currentVersion, allowInsecureHttp: this.allowInsecureHttp });
        const { release, releaseId } = decodeFeed(bytes, this.publicKey);
        if (release.platform !== dockerUpdatePlatform) throw Error('update_platform_unsupported');
        this.state = { phase: compareVersion(release.version, this.currentVersion) > 0 ? 'available' : 'current', release, releaseId, checkedAt: new Date(this.now()).toISOString() };
      } catch (error) {
        this.state = { phase: 'failed', errorCode: failure(error), checkedAt: new Date(this.now()).toISOString() };
      } finally { this.finishedAt = this.now(); this.inflight = null; }
    })();
    return this.inflight;
  }
  stale() {
    if (this.state.phase === 'idle') return true;
    if (this.inflight) return false;
    return this.now() - this.finishedAt >= (this.state.phase === 'failed' ? this.retryMs : this.ttlMs);
  }
  // Server information: a cached answer, or a fresh check awaited briefly. A
  // slow feed leaves state `checking`, and the console polls until it settles.
  async discover({ authorize = () => {} } = {}) {
    if (this.stale()) { authorize(); this.start(); }
    if (this.inflight) await Promise.race([this.inflight, new Promise(resolve => { const t = setTimeout(resolve, this.waitMs); t.unref?.(); })]);
    return this.status();
  }
  // The administrator's 업데이트 확인. Repeated presses within a few seconds reuse the last answer.
  check() {
    if (!this.inflight && this.now() - this.finishedAt >= this.forcedMs) this.start();
    return { accepted: true };
  }
  apply() { throw new HttpError(409, 'update_manual_docker'); }
}

// The broker's server update source. An updater socket (the native install, or
// a root Docker updater) keeps the full check/apply path unchanged; only a
// Docker Compose install without one gets the read-only signed feed check.
export function selectServerUpdate(config, currentVersion) {
  if (config.updateSocket) return new ServerUpdateClient(config.updateSocket);
  if (config.deploymentKind === 'docker' && config.dockerUpdateFeed) return new DockerReleaseCheck({ feed: config.dockerUpdateFeed, currentVersion });
  return null;
}
