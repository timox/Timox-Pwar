# Integration ownership

TimoxVAudioEngine owns `observe-roundtrip.cjs` and `timox_host_tests.cpp` integration.
The files in `reference/` are immutable historical references, not runnable targets.
They retain original repository-relative paths so their provenance remains exact.

The observer requires Node.js, an authenticated engine API, engine/pilot ASIO probe,
the PWAR client, matching virtual driver and a coordinated Linux PipeWire relay.
It changes routes and uses audio/network resources. The C++ host test additionally
requires the engine's `physical_asio_host.cpp`, `asio_sample_conversion.cpp`, engine
headers, full ASIO host SDK and the legacy PWAR DLL. `test-supervision.ps1` likewise
requires the old qualification binary, installed driver and permitted UDP activity.

None is configured by this project's CMake or run during source migration.
Client `asio_client_tests.cpp` is retained but excluded from the default offline
suite because its simulated driver still uses real UDP. Existing ASIO lifecycle
and channel tests are retained as source references for separately authorized runs.
