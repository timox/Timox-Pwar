# Timox-Pwar repository guide

- Confirm the repository root with `git rev-parse --show-toplevel` and inspect
  `git status --short --branch` before builds or installations.
- Own the PWAR transport, Linux backends, standalone Windows client and v1 control
  contract. TimoxVAudioEngine owns the supervisor, API and integrated observations.
- Keep source and build dependencies local and explicitly pinned. Do not import
  binaries from old build folders or infer provenance from filenames.
- Do not operate shared audio hardware, UDP ports, engine routes, remote hosts or
  PipeWire graphs without coordination with the parent/integration operator.
- Unit tests do not establish audio continuity. Preserve the documented failure:
  70,871 zero frames out of 480,000; Linux rejected 116,992 frames.
- Keep protocol version 1 and null unmeasured latency/drift fields until separately
  authorized and tested changes establish a new contract.
