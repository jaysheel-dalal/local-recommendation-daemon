I'm building a portfolio project in C++ to prep for an Apple Software Engineer, 
Apple Ads role (on-device ad delivery systems: daemons, SDKs, IPC, concurrency, 
privacy-by-architecture — not a UI/app role). My current resume is Python/FastAPI-
heavy and I need a project that demonstrates C++ systems programming: IPC, 
multi-threading with explicit locking, caching, and serialization.

ENVIRONMENT: I'm on Windows, developing inside WSL2 (Ubuntu). Before proposing 
anything, check that g++/clang, cmake, make, and pthread support are available 
in this environment, and tell me if anything needs installing (e.g. protobuf 
compiler/dev libraries) before we start.

PROJECT: On-Device Local Recommendation Engine (Daemon + Client SDK)

Goal: simulate the kind of on-device ad-candidate ranking/filtering system Apple 
Ads builds — a background daemon that ranks and filters items using local signals, 
enforces simple compliance rules, and serves results to client processes over IPC 
with low latency. Privacy-preserving by design (no raw activity leaves the device).

I want to build this in two phases so I always have a working, demoable system 
even if I run out of time before finishing phase 2.

PHASE 1 — Core caching daemon (build this first, get it fully working before touching phase 2):
- A background daemon process exposing a UNIX domain socket IPC interface
- Multiple client processes can connect and send requests concurrently
- Thread pool handles requests; explicit mutex/RWLock protects a shared in-memory 
  cache (no data races — I want to be able to explain the locking strategy clearly 
  in an interview)
- Requests/responses serialized with Protocol Buffers (or a simple custom binary 
  format if Protobuf setup is too heavy for v1 — your call, explain the tradeoff)
- LRU eviction policy on the cache
- A basic load-testing/benchmark client that measures throughput and p50/p90/p99 
  latency under concurrent load

PHASE 2 — Extend into the full recommendation engine (only after phase 1 is solid):
- Replace the generic cache with candidate "item" metadata (mock ad-like items: 
  id, category, score, metadata)
- Add a simple ranking/filtering function over candidates based on local "user 
  signal" input (doesn't need ML — a scoring heuristic is fine)
- Add compliance-style rules layered on the filter: max exposure cap per item, 
  frequency limit per time window
- Wrap the daemon's socket protocol in a thin client SDK/library that other 
  "apps" link against instead of talking to the raw socket — this needs to read 
  as a genuine SDK, not just a wrapper function
- Add one honest privacy mechanism: local noise/rounding added to any aggregated 
  metrics before they'd be logged/exported (I don't need full formal differential 
  privacy math — just a real, explainable mechanism, not a fake gesture at one)

CONSTRAINTS:
- C++ (not Rust) — I want this to double as C++ interview prep
- Runs on Linux (via WSL2/Ubuntu) — this maps closely enough to macOS/Apple's 
  Unix-like environment for the concepts to transfer directly, even though I'm 
  not building on macOS itself
- I'm comfortable with Python/FastAPI systems design but this is my first real 
  C++ systems project — I want you to explain non-obvious C++ choices as you 
  make them (smart pointers vs raw, RAII patterns, why a particular lock type), 
  not just write code silently
- Keep the repo structured so each phase is a clean, working milestone I can 
  commit and point to independently

BEFORE WRITING CODE: propose a concrete build plan for Phase 1 only — directory 
structure, the order you'll implement components in (e.g. socket server before 
thread pool before cache before serialization), and which libraries/build system 
you'd use (CMake + which Protobuf/threading libs). I want to review and approve 
the plan before you start implementing.