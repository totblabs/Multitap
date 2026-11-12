# Build through the root compose.build.yaml or: docker build ./server
# Official node:24-bookworm-slim OCI index, verified from registry-1.docker.io.
FROM node:24-bookworm-slim@sha256:55fb5f5a527ea2c5c4a95f963e48ef90aa38c7b346054fdc8ee9957a2b55d205 AS bundle
# Deliberate release inputs, never the workspace's .artifacts/ directory. A
# missing bundle fails COPY. The bundle holds the Windows client/Host EXEs,
# the three Android APKs, their receipts, releases.json and bundle.json, all
# checked below (src/download-bundle.mjs builds and validates it).
# Read-only modes are set in this stage, so the image carries the files once.
COPY release-bundle/ /bundle/
RUN chmod 0555 /bundle && find /bundle -mindepth 1 -maxdepth 1 -type f -exec chmod 0444 {} +

FROM node:24-bookworm-slim@sha256:55fb5f5a527ea2c5c4a95f963e48ef90aa38c7b346054fdc8ee9957a2b55d205

ARG MULTITAP_VERSION=1.1.1
LABEL org.opencontainers.image.title="multitap-broker" \
      org.opencontainers.image.version="${MULTITAP_VERSION}" \
      org.opencontainers.image.licenses="AGPL-3.0-only"

ENV NODE_ENV=production \
    MULTITAP_HOST=0.0.0.0 \
    MULTITAP_PORT=18480 \
    MULTITAP_DATA_DIR=/var/lib/multitap

WORKDIR /app
# Install the locked relay dependency; optional native accelerators are not needed.
COPY package.json package-lock.json ./
RUN node -e 'const v=require("./package.json").version;if(v!==process.argv[1])throw Error("OCI version must match package.json")' "$MULTITAP_VERSION"
RUN npm ci --omit=dev --omit=optional --ignore-scripts --no-audit --no-fund \
    && npm cache clean --force
COPY src/ ./src/
COPY update/ ./update/
COPY admin/ ./admin/
COPY setup/proxmox.py ./setup/proxmox.py
COPY --from=bundle /bundle/ /opt/multitap-release-bundle/
ARG MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE=false
ENV MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE=${MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE}
RUN case "$MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE" in \
      false) node src/download-bundle.mjs validate /opt/multitap-release-bundle ;; \
      true) node src/download-bundle.mjs validate /opt/multitap-release-bundle --allow-empty ;; \
      *) echo 'Invalid empty-bundle build option' >&2; exit 1 ;; \
    esac
COPY docker-entrypoint.sh /usr/local/bin/multitap-entrypoint
RUN chmod 0555 /usr/local/bin/multitap-entrypoint \
    && mkdir -p /var/lib/multitap \
    && chown 1000:1000 /var/lib/multitap \
    && chmod 0700 /var/lib/multitap

USER 1000:1000
VOLUME ["/var/lib/multitap"]
EXPOSE 18480
# UDP media relay, only when MULTITAP_UDP_PORT selects it (compose.yaml).
EXPOSE 18481/udp
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
  CMD ["node", "update/server-access-health.mjs", "--container-health"]
ENTRYPOINT ["/usr/local/bin/multitap-entrypoint"]
CMD ["node", "src/index.mjs"]
