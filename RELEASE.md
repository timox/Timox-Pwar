# Timox-Pwar 0.1.0-alpha.1

First independently versioned candidate after extraction from TimoxVasio.
cmake/PwarVersion.cmake owns the product version. The Windows client and both
Linux command-line backends print it with --version before audio/network startup.
Windows resources use numeric file version 0.1.0.1 and product 0.1.0-alpha.1.
The Linux shared library uses VERSION 0.1.0 / SOVERSION 0; this records the initial
library ABI line, not a new network or control protocol.

Control protocol **v1**, transport behavior and unknown latency/drift fields stay
unchanged. The previously observed 70,871 zero frames out of 480,000 and 116,992
Linux rejected frames remain unresolved. This version is not a stable audio claim.

## Build and provenance

Use the existing checkout; inspect git root/status and build from a clean commit.
Dependencies are the existing pinned SDK/JSON snapshot; no implicit downloads.

Windows (Visual Studio 2026 and Windows SDK):

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
node verify-import.cjs
.\build\Release\PwarAsioClient.exe --version
```

Linux (CMake, compiler, pkg-config and PipeWire development headers):

```sh
cmake -S . -B build -DPWAR_BUILD_LINUX=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON -DCMAKE_DISABLE_FIND_PACKAGE_Qt5=TRUE
cmake --build build --parallel 4 --target PwarLinuxBuildInfo PwarControlProtocolTests PwarClientFrameRingTests PwarRouterBoundsTests PwarLinuxCallbackTests PwarProtocolAudit PwarReceiveBufferAudit PwarAudioQueueTests
ctest --test-dir build --output-on-failure
node verify-import.cjs
```

The default Windows build emits pwar-build.json beside PwarAsioClient.exe. The
PwarLinuxBuildInfo target emits per-binary receipts for pwar_cli, pwar_stream_cli
and libpwar. Receipts record version, protocol, exact commit, dirty state, platform,
filename and SHA-256. Hardware, UDP peers and PipeWire graphs are not started by
these build/version tests. Workflow results and artifacts are available under
.github/workflows/pwar-build.yml. Linux archives retain executable permissions
and library symlinks; PipeWire runtime dependencies must still be installed.

## Integration

The rebuilt Windows client has a new SHA-256. TimoxVAudioEngine must update
cmake/PwarClient.cmake from this verified artifact and rebuild its supervisor
consumers; keep runtime hash verification enabled. TimoxVSSC must pin the same
client/version/revision and receipt in artifacts.lock.json, then use that freshly
rebuilt engine. Do not combine the new client with the old engine supervisor pin.
The suite's driver remains TimoxVasio 1.2.7 and engine API remains 1.1.
