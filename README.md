# Timox-Pwar

Standalone source snapshot of the PWAR transport, Windows ASIO client and Linux
PipeWire backends. See [MIGRATION.md](MIGRATION.md) for provenance and build commands,
and [client/CONTROL.md](client/CONTROL.md) for the unchanged version 1 control protocol.

The observed audio round trip is discontinuous and is **not qualified as stable**.
No binary, installation or driver registration is included in the source import.

PWAR is GPL-3.0; upstream notices remain in source files and `LICENSE`.
ASIO SDK notices/licenses are under `deps/asiosdk`; nlohmann JSON is MIT licensed
under `deps/nlohmann/LICENSE.MIT`. Exact dependency contents are pinned in
`import-manifest.json`; builds do not download dependencies.
