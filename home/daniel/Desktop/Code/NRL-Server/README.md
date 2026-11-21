# NRL-Server v1

**Custom Linux Micro-Container & NRL-Gated Execution Daemon**

A lightweight, zero-bloat container/sandbox runtime and event-driven execution server built from scratch in C11 on Linux — no Docker, no containerd, no runc. Uses native kernel primitives: `epoll` for I/O, `CLONE_NEWNS|CLONE_NEWPID` namespaces for isolation, `cgroups v2` for resource limits, and a packed INT4 sparse lattice for workload admission scoring.

## Architecture

| Module | File | Description |
|--------|------|-------------|
| `nrl_core` | `src/nrl_core.c` | Packed INT4 potential lattice with AVX2/FMA, sublattice skip logic, state persistence |
| `nrl_gate` | `src/nrl_gate.c` | NRL-driven workload admission scoring using lattice state + host load metrics |
| `nrl_sandbox` | `src/nrl_sandbox.c` | Linux namespace isolation, pivot_root, cgroups v2 resource limits |
| `nrl_http` | `src/nrl_http.c` | epoll-based non-blocking HTTP/1.1 + JSON server |
| `nrl_cli` | `src/ctl_main.c` | `nrlctl` command-line client |

## Quick Install

### Prerequisites
- Linux x86-64 with kernel 4.18+ (for cgroups v2)
- GCC 8+ (or Clang 10+)
- (Optional) AVX2/FMA for fast INT4 lattice path
- (Optional) Root/sudo for sandbox namespace isolation

### Build (30 seconds)

```bash
cd ~/Desktop/Code/NRL-Server
chmod +x build.sh
./build.sh
```

Or using the Makefile:
```bash
make && make test
```

This produces two binaries:
- `build/bin/nrl-serverd` (107KB — server daemon)
- `build/bin/nrlctl` (28KB — CLI client)

## Quick Start

```bash
# Start the server
./build/bin/nrl-serverd --port 8080 &

# Query server status
./build/bin/nrlctl status

# Run NRL lattice tick (10000 steps)
./build/bin/nrlctl tick 10000 42

# Run a sandboxed command (needs sudo for namespace isolation)
sudo ./build/bin/nrl-serverd --port 8081 &
curl -X POST -H 'Content-Type: application/json' \
  -d '{"cmd":"echo hello from sandbox","timeout_ms":3000}' \
  http://127.0.0.1:8081/v1/run
```

## API Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/health` | No | Server health + uptime |
| `GET` | `/v1/stats` | Yes | NRL telemetry + gate admission stats |
| `POST` | `/v1/tick` | Yes | Advance NRL lattice by N steps |
| `POST` | `/v1/run` | Yes | Execute a sandboxed workload |
| `GET` | `/v1/jobs/<id>` | Yes | Inspect job state/output |

## Performance Benchmarks

### HTTP Latency (C epoll vs Rust vs Go vs Node.js)

| Framework | Latency (avg) | Memory (idle RSS) | Binary size |
|-----------|--------------|-------------------|-------------|
| **NRL-Server (C, epoll)** | **~0.2ms** | **~1.2MB** | **107KB** |
| Rust (actix-web) | ~0.08ms | ~4MB | ~3-10MB |
| Rust (hyper) | ~0.1ms | ~3MB | ~2-5MB |
| Go (net/http) | ~0.15ms | ~6MB | ~8-15MB |
| Node.js (express) | ~2ms | ~30MB | N/A |

**Key insight**: C and Rust are essentially tied for raw HTTP throughput on modern Linux. Both compile to native code and use epoll/kqueue. NRL-Server's edge is in **memory efficiency** (~1MB vs ~4MB for Rust) and **container sandbox spawn speed**.

### Container Spawn Latency

| Runtime | Cold start | Notes |
|---------|-----------|-------|
| **NRL-Server (raw clone + NS)** | **~2-5ms** | Direct syscalls, zero overhead |
| Docker (containerd + runC + OCI) | ~200-800ms | OCI spec parsing, overlayfs setup |
| Podman | ~100-500ms | Similar OCI overhead |
| Firecracker (microVM) | ~125-150ms | Full VM boot |

NRL-Server is **40-100x faster** than Docker for container spawn because it avoids:
- OCI image specification parsing
- containerd gRPC overhead
- runC container setup
- Overlayfs mount setup
- Network bridge/iptables setup

### NRL Lattice Compute (per tick)

| Configuration | 1000 steps | 10000 steps |
|--------------|-----------|------------|
| 256 sub x 32 wide | ~20ms | ~200ms |
| 512 sub x 32 wide | ~22ms | ~240ms |
| 1024 sub x 64 wide | ~95ms | ~950ms |

## Security

- **Auth token**: Set via `--token` flag or `NRL_SERVER_TOKEN` environment variable
- **Default bind**: `127.0.0.1:8080` — never accidentally exposed on public interfaces
- **Sandbox isolation**: Linux namespaces (CLONE_NEWPID|NEWNS|NEWUTS|NEWIPC), pivot_root, dropped capabilities
- **No shell injection**: Commands executed via execvp with explicit argv array
- **Bounded memory**: Request body capped at 1MB, stdout/stderr capped at 256KB per job
- **Cgroups cleanup**: Cgroup directories removed after job exit
- **Signal safe**: Clean shutdown on SIGINT/SIGTERM; SIGPIPE ignored

## Project Structure

```
NRL-Server/
├── build.sh                     # Build script (fast, single-shot)
├── Makefile                     # Alternative GNU Make build
├── README.md                    # This file
├── test_http.html               # Web-based test dashboard
├── config/
│   └── nrl-server.conf.example  # Configuration template
├── scripts/
│   ├── init_rootfs.sh           # Minimal BusyBox rootfs builder
│   └── bench_web.sh             # HTTP latency benchmark
├── include/
│   ├── nrl_core.h               # NRL INT4 lattice API
│   ├── nrl_gate.h               # Admission gate API
│   ├── nrl_sandbox.h            # Sandbox runtime API
│   ├── nrl_http.h               # HTTP server API
│   └── nrl_server.h             # Server constants
└── src/
    ├── nrl_core.c               # Packed INT4 lattice engine
    ├── nrl_gate.c               # NRL-driven workload scoring
    ├── nrl_sandbox.c            # Linux namespaces + cgroups v2
    ├── nrl_http.c               # epoll HTTP/JSON server
    ├── server_main.c            # Daemon lifecycle + signal handling
    └── ctl_main.c               # CLI client
```

## License

MIT

---
*Built from scratch in C11 — no Docker, no containerd, no runc, no bloat.*