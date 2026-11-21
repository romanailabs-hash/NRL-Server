# NRL-Server v1 — Custom Linux Micro-Container & NRL-Gated Execution Daemon
## Build Specification for DeepSeek Flash V4 (Fresh Linux Build)

### 1. Objective & Design Philosophy
Design and build **NRL-Server** from scratch on Linux (C11 + x86-64 AVX2/FMA, with optional Python/CLI client tooling) as a **custom lightweight container/sandbox runtime and event-driven execution server**—**no Docker daemon, no containerd, no runc**.

Key goals:
- **Sub-millisecond / ultra-low-overhead container spawn & teardown** using native Linux kernel isolation primitives (`unshare`/`clone` namespaces, `pivot_root`, `cgroups v2`, `seccomp-bpf`).
- **NRL-native state & admission/routing gate**: Use NRL (`braincore_int4` / Omega-ZPM sparse sublattice state & telemetry) to score incoming workloads, track lattice state deltas (`learn_steps`, active sublattices, skip ratios), and gate/route jobs before or during execution.
- **Web & HTTP/JSON control plane**: Expose a fast non-blocking HTTP/1.1 + JSON API (with optional Unix socket + TCP listener) for health, telemetry, job submission, sandbox lifecycle, and benchmark metrics—designed to sit safely behind a reverse proxy (Caddy/Nginx) or private mesh (Tailscale).
- **Zero-bloat single-binary core (`nrl-serverd` + `nrlctl`)**: Clean, deterministic, copy-pasteable single-shot build scripts (`build.sh`) with zero Windows/PowerShell dependencies.

---

### 2. Core Architecture (`NRL-Server`)
Split the system into **five decoupled modules** inside `~/Desktop/Code/NRL-Server`:

1. **`nrl_core` (NRL State & Sparse Compute Engine)**
   - Packed INT4 potential lattice (`uint8_t` packing two 4-bit states per byte) with AVX2/FMA fast path and scalar fallback.
   - Sublattice routing & ZPM-style skip logic (skipping inactive/zero-delta sublattice windows to maximize throughput and keep CPU/RAM usage minimal).
   - Persistent binary state snapshot (`.nrlstate`) + atomic checkpointing and telemetry struct (`steps`, `virtual_steps`, `active_sublattices`, `total_sublattices`, `skip_ratio`, `state_hash`, `last_delta_score`).

2. **`nrl_gate` (Admission, Priority & Placement Policy)**
   - Evaluates each incoming workload request (command, image/rootfs ID, CPU/MEM quota, timeout, payload hash) against current NRL lattice state and host load (`/proc/loadavg`, `/proc/meminfo`).
   - Outputs a deterministic decision: `ALLOW_FAST`, `ALLOW_THROTTLED`, `DEFER`, or `REJECT`, plus a `lane` / `priority` score.

3. **`nrl_sandbox` (Custom Linux Container / Micro-Jail Runtime — No Docker)**
   - Uses Linux namespaces (`CLONE_NEWPID`, `CLONE_NEWNS`, `CLONE_NEWUTS`, `CLONE_NEWIPC`, `CLONE_NEWNET` optional/configurable) and `pivot_root` into a minimal rootfs directory (`rootfs/<name>`).
   - Sets hostname, mounts minimal `/proc`, `/sys` (read-only), `/dev/null`, `/dev/zero`, `/dev/urandom`, `/tmp` (`tmpfs`), and drops unnecessary capabilities (`capset` / bounding set).
   - Enforces resource limits via **cgroups v2** (`memory.max`, `cpu.max`, `pids.max`) under a dedicated `/sys/fs/cgroup/nrl-server/<job_id>` slice, cleaning up cgroup directories on exit.
   - Captures `stdout`/`stderr` into bounded ring/log files, enforces execution timeouts, and reaps child PIDs cleanly (`waitpid` / `SIGKILL` on timeout).

4. **`nrl_net` (Fast Non-Blocking HTTP/JSON & Socket Server)**
   - Event-driven C HTTP/1.1 + JSON server using `epoll` and non-blocking sockets (`SO_REUSEADDR`, `TCP_NODELAY`).
   - Supports concurrent health/status polls, job execution requests, log streaming/tailing, and benchmark endpoints with low latency and clean error handling (proper `Content-Length`, status codes, request size limits, and bearer token / bind-address controls).

5. **`nrl_cli` (`nrlctl` Command-Line Client & Benchmark Harness)**
   - Talks to `nrl-serverd` over HTTP or Unix socket.
   - Provides commands for `status`, `tick` (run NRL steps), `run` (execute a sandboxed command/workload in a custom rootfs), `ps`/`logs`/`stop`, and `bench` (measures spawn latency, HTTP throughput, and NRL steps/sec).

---

### 3. Directory & File Layout
Create this exact layout under `~/Desktop/Code/NRL-Server`:

```text
NRL-Server/
├── build.sh                     # Single-shot Linux build & test script (LF endings)
├── Makefile                     # Optional clean build target
├── README.md                    # Architecture, endpoints, security & tuning guide
├── config/
│   └── nrl-server.conf.example  # Bind host/port, auth token, cgroup root, rootfs dir, state path
├── scripts/
│   ├── init_rootfs.sh           # Builds a minimal Alpine/BusyBox or custom rootfs for sandboxes
│   └── bench_web.sh             # Load & latency benchmark against /health, /v1/tick, /v1/run
├── include/
│   ├── nrl_core.h
│   ├── nrl_gate.h
│   ├── nrl_sandbox.h
│   ├── nrl_http.h
│   └── nrl_server.h
└── src/
    ├── nrl_core.c               # AVX2 INT4 lattice, sublattice skip, state persistence
    ├── nrl_gate.c               # NRL-driven workload scoring & admission gate
    ├── nrl_sandbox.c            # Linux namespaces, pivot_root, cgroups v2, process/log runner
    ├── nrl_http.c               # epoll non-blocking HTTP/1.1 + JSON parser/router
    ├── server_main.c            # Daemon lifecycle, signals (SIGINT/SIGTERM), worker/tick loop
    └── ctl_main.c               # `nrlctl` CLI client & benchmark runner

4. HTTP / JSON API Specification

Implement these core endpoints cleanly in src/nrl_http.c:

    GET /health

        Returns 200 OK with {"ok":true,"service":"nrl-server","uptime_sec":...} (fast path for uptime monitors/proxies).

    GET /v1/stats

        Returns current NRL engine telemetry (steps, virtual_steps, active_sublattices, total_sublattices, skip_ratio, state_hash), sandbox counts (running, completed, failed), and host memory/load summary.

    POST /v1/tick

        Body: {"steps":10000,"input_seed":12345}

        Advances the NRL lattice, updates state & telemetry, and returns execution time (us), virtual_steps, skip_ratio, and updated state_hash.

    POST /v1/run

        Body: {"rootfs":"base","cmd":["/bin/sh","-c","echo hello"],"timeout_ms":5000,"mem_mb":128,"cpu_quota_pct":50,"pids_max":64}

        Passes request through nrl_gate -> spawns isolated sandbox via nrl_sandbox -> returns {"job_id":"...","gate_decision":"ALLOW_FAST","exit_code":0,"duration_ms":...,"stdout":"...","stderr":"..."}.

    GET /v1/jobs/<id> & POST /v1/jobs/<id>/kill

        Inspect job state/output or terminate a running sandbox job.

Optional security hardening:

    Support NRL_SERVER_TOKEN (if set, require Authorization: Bearer <token> on /v1/* while keeping /health accessible or configurable).

    Default bind to 127.0.0.1:8080 so it is never accidentally exposed wide-open on a public interface without explicit config or reverse proxy/Tailscale.

5. Linux Sandbox Safety & Reality Checks (Must Be Handled Explicitly)

Make sure the implementation handles these Linux kernel details properly rather than faking them:

    cgroups v2 detection & fallback: Check /sys/fs/cgroup/cgroup.controllers. Create a sub-cgroup directory, write memory.max, cpu.max, pids.max (when writable), move the child PID into cgroup.procs, and clean up the directory after the child exits. If unprivileged without delegated cgroups v2, log a clear warning and degrade gracefully rather than crashing.

    pivot_root + MS_PRIVATE mount propagation: After entering CLONE_NEWNS, mark / as MS_REC | MS_PRIVATE so mount changes do not leak to the host, bind/prepare the new root, call pivot_root, chdir("/"), mount fresh /proc (MS_NOSUID|MS_NOEXEC|MS_NODEV), and unmount the old root (MNT_DETACH).

    No shell injection: Execute commands via execve with an explicit argv[] array—never concatenate untrusted strings into /bin/sh -c unless explicitly requested by the caller's cmd array.

    Bounded memory & buffers: Cap request body size (e.g., 1 MiB) and captured stdout/stderr per job (e.g., 256 KiB tail/cap) so a chatty job cannot exhaust host RAM.

    Strict C11 + POSIX compliance: Define #define _POSIX_C_SOURCE 200809L (and _GNU_SOURCE where clone/pivot_root/unshare are used) consistently, use LF (\n) line endings everywhere, and compile with -Wall -Wextra -Werror (or clean warning-free builds) under -O3 -mavx2 -mfma -mpopcnt with a portable fallback when AVX2 isn't available.

6. Build, Rootfs & Verification Instructions

Provide a copy-pasteable build.sh and scripts/init_rootfs.sh that:

    Compiles build/bin/nrl-serverd and build/bin/nrlctl cleanly on Linux.

    Runs built-in unit/integration checks (NRL lattice tick & state hash determinism, gate scoring, epoll HTTP roundtrip, and sandbox isolation test).

    Sets up a minimal test rootfs in build/rootfs/base (using BusyBox/Alpine minirootfs or a tiny static test binary if offline) and runs a smoke test executing a command inside the isolated namespace and verifying /proc isolation and cgroup/timeout cleanup.

7. Output Format Expected From DeepSeek Flash V4

Generate the complete project in clean, copy-pasteable file blocks in this order:

    build.sh and Makefile

    include/nrl_core.h, include/nrl_gate.h, include/nrl_sandbox.h, include/nrl_http.h, include/nrl_server.h

    src/nrl_core.c and src/nrl_gate.c

    src/nrl_sandbox.c

    src/nrl_http.c

    src/server_main.c and src/ctl_main.c

    scripts/init_rootfs.sh, scripts/bench_web.sh, config/nrl-server.conf.example, and a concise README.md with exact Linux run & benchmark commands.
