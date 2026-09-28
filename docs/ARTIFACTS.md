# Launcher build artifacts

`VERSION` identifies the launcher component, not the assembled Holder product.
Windows file properties contain that version and the source commit; startup logs
on both platforms include the source commit. Modified working trees are marked
`-dirty`. Builds without Git information are marked `unknown-dirty`.

## Build and package

Use a clean Git checkout, Python 3 and a `RelWithDebInfo` build. Run tests before
packaging; the packaging target itself does not claim tests have passed.

```sh
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
cmake --build build --config RelWithDebInfo --target stage_artifact
python scripts/artifact.py verify build/artifacts/RelWithDebInfo
```

For single-configuration generators such as Ninja, configure with
`-DCMAKE_BUILD_TYPE=RelWithDebInfo`. The packaging target is available when CMake
finds Python; building the launcher alone does not require Python.

Output directories must be empty. Existing artifacts are never overwritten.
For local inspection of uncommitted work, invoke `scripts/artifact.py create`
directly with `--allow-dirty`, `--binary`, `--build-record`, `--symbols` and
`--output`. The build record is `build/provenance/RelWithDebInfo/built.json`.
Dirty or unidentified builds are rejected by default and are not release inputs.

## Contents and verification

- `Holder.exe` or `Holder`: the exact linked executable.
- `Holder.pdb` on MSVC, or `Holder.dSYM/` on macOS: debug symbols for support.
- `build-info.json`: launcher version, actual source commit/dirty state, target
  architecture, compiler/version, configuration, generator and CMake version;
  executable/payload SHA-256 hashes and public GitHub run identifiers.
- `SHA256SUMS`: checksums for every file except itself, including metadata.
- `manifest.txt`: complete relative file inventory, retaining the original
  executable filename for downstream staging consumers.

Source identity is refreshed before each build. The build record binds that
identity and toolchain configuration to the executable hash after linking.
Packaging rejects a binary changed since linking; verification rejects missing,
extra or modified files. The actual checked-out commit is recorded, which can be
a synthetic merge commit on a pull-request run.

Checksums detect changes against a trusted artifact record; they are not signed
attestations or a substitute for trusting the producing workflow. CI runs tests
before packaging and retains artifacts for 90 days. Release tooling must archive
the selected candidate inputs beyond that retention window.

## Release handoff

Select artifacts from a specific successful run/commit, verify them, and preserve
their metadata with the assembled candidate. Retain symbols for support; staging
decides whether to exclude them from the installer payload. Signing or modifying
the executable changes its hash: keep the input record and record new hashes for
the signed output and final package. Promote the tested candidate without silently
substituting a rebuild. Package signing and release archival remain owned by
staging/release.
