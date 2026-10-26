# PWAR source extraction — 2026-10-26

Canonical transport sources were copied byte-for-byte from the local
`TimoxVasio/build_pwar_reconciled_20261025` snapshot. Client and qualification
sources came from `TimoxVasio/patches/pwar`. `import-manifest.json` records source
paths, source SHA-256, final SHA-256 and generated files. The source Git commit
does not capture these local uncommitted changes; file hashes are authoritative.
Only CMake wiring changed; no protocol, queue, client or backend implementation
was edited. The standalone source tree builds without the original clone.

## Reconciliation

Upstream base: `ed5b07c44c7635d006422d646d56368600504b3f`. The historical inputs
stopped at 0007 and are retained under `docs/historical`. The new root inputs
list 0001–0013, Windows burst capacity, experimental stream backend, 0014,
0015, then `0016-consolidated-stream-snapshot.patch` in application order.
The measured-stream-backend patch is an alternate older branch, preserved for
provenance but deliberately excluded from this sequence.

Local reverse/replay established that 0001–0015 and the two branch patches
reproduce all canonical Linux, protocol and Windows files except the stream
relay. New 0016 captures its existing quantum bounds checks and interval
telemetry exactly; it introduces no new behavior relative to the consolidated
snapshot. Applying 0016 restored normalized-text equality for all 38 files.
The reverse/replay base was derived locally: the historical upstream ZIP is
absent, so full replay from a verified upstream archive remains unverified.
Builds use the checked-in canonical snapshot directly, not that unavailable ZIP.

## Local build

From the standalone checkout, first inspect `git rev-parse --show-toplevel`
and `git status --short --branch`.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The Windows client uses the minimal local ASIO common headers, the router,
Threads and Windows system libraries. The optional legacy DLL additionally
uses the included SDK COM implementation files. JSON and SDK files/licenses
are copied from the original local dependencies and individually hash-pinned;
there is no FetchContent, implicit SDK path or download. The legacy DLL option
is OFF and never registers a driver or copies it to an external path.

The default Windows suite is offline: control protocol, frame ring, router,
packet and receive-buffer cases, plus client help/invalid arguments. Real-UDP
fake-driver tests and historical hardware/integration tests are excluded.
`integration/README.md` describes their separate engine ownership/dependencies.

## Linux commands — unverified in this extraction

```sh
cmake -S . -B build -DPWAR_BUILD_LINUX=ON
cmake --build build --target pwar_cli pwar_stream_cli
cmake --build build --target PwarAudioQueueTests PwarLinuxCallbackTests
ctest --test-dir build --output-on-failure
```

Requires a real Linux compiler, pkg-config and PipeWire development headers;
Qt5 is optional for the historical GUI. For pure protocol/queue tests without
PipeWire, configure `-DPWAR_BUILD_LINUX=OFF` and build all default targets.
No Linux execution, remote access, installation or network observation was
performed here. Stub callback tests are not a Linux ABI/build validation.

## Qualification remains failed

The preceding coordinated observation measured 70,871 zero frames out of
480,000 (14.8%), and Linux rejected 116,992 frames. Its complete causal analysis
and fix remain open. Keep stereo float32 little-endian, 48 kHz, fixed port 8321,
v1 stdio control and null unmeasured `latency_ms`/`drift_ppm`. UDP peer filtering
does not authenticate audio. Offline green tests do not qualify continuity,
drift compensation, network recovery or DSP round trips.

The old bundle script, prebuilt binaries, historical packaging/CI files and
runtime observations were not imported as runnable release tooling. There is
no claim of a releasable or stable audio product from this source migration.


## Results from this extraction

- Windows configure/build: PASS with MSVC 19.51.36260, VS 18 2026, SDK 10.0.26100.
  The initial sandbox attempt failed with MSBuild FileTracker E_ACCESSDENIED;
  the approved execution outside the sandbox succeeded.
- `ctest --test-dir build -C Release --output-on-failure`: 21/21 PASS.
- Client artifact: `build/Release/PwarAsioClient.exe`, UTC timestamp
  `2026-10-26T18:40:42.3855373Z`, SHA-256
  `29D5E3208E10AE5FEBBB7FD3914790A87EAF12D32BE6BEF923F20D7E33566FD3`.
  This identifies this build only; PE bytes are not claimed reproducible across builds.
- Existing C4244 warning converting the driver name from wchar_t to char remains.
- Linux library build wiring now explicitly links Threads::Threads for its existing
  pthread calls; Linux builds, C11 queue tests and callbacks remain unverified.
- No audio, UDP, remote host, installation, registration or publication performed.

Git attributes disable automatic text normalization for this snapshot. This preserves the exact imported bytes and SHA-256 across clones; verify the committed blobs as well as the working tree before updating these pins.
