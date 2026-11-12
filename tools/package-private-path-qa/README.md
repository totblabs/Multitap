# Package path admission regression

These tests call the real package path predicates. They do not package, stage,
compress, extract, install, read private runtime files or exercise a service.

Broker update packages (`server/update/package.mjs`):

```powershell
node --test --test-concurrency=1 server/test/package-path-policy.test.mjs
```

To compare with another version, set `MULTITAP_PACKAGE_PATH_SOURCE` to an
unmodified `server/update/package.mjs`; the loader only adds an export alias to
its private `allowed` function, and both of its call sites are inspected.

Windows installers (`tools/build-windows-installer.ps1` and its helper
`tools/windows-payload-path-policy.ps1`):

```powershell
& ./tools/package-private-path-qa/windows_path_test.ps1 -SourceRoot $PWD
```

The test loads only the helper function's AST, checks that the installer
build's single payload guard calls it, and never runs the installer build.
With `-Baseline`, pass `-SourceRoot` of an older tree whose installer build
still had an inline `$relative -match` guard; that expression is evaluated
instead. Where
running script files is restricted, the test can be passed as inline
PowerShell command text; no execution-policy change is needed.

The policies reserve exact VCS and runtime directory components, credential
basenames and `.env` variants. Only `.env.example`, `.env.sample` and
`.env.template` are allowed environment names. `.bin`, `.well-known`, tracked
`.gitignore`/`.gitattributes`, licences and ordinary JSON stay valid. Windows
payloads also keep public CA `.pem` support; the broker's `.pem`/`.key`
extension rejection is unchanged. A renamed secret, or a secret placed in an
allowed example file, is outside path-only classification.

A pass proves path admission and source wiring only, not archive-content
secrecy or complete packaging.
