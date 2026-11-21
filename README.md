# NRL-Server v1

**Custom Linux Micro-Container & NRL-Gated Execution Daemon**

A lightweight, zero-bloat container/sandbox runtime and event-driven execution server built from scratch on Linux — no Docker, no containerd, no runc.

## Architecture

| Module | Description |
|--------|-------------|
| `nrl_core` | Packed INT4 potential lattice with AVX2/FMA, sublattice skip logic, state persistence |
| `nrl_gate` | NRL-driven workload admission scoring using lattice state + host load metrics |
| `nrl_sandbox` | Linux namespace isolation, pivot_root, cgroups v2 resource limits |
| `nrl_http` | epoll-based non-blocking HTTP/1.1 + JSON server |
| `nrl_cli` | `nrlctl` command-line client |

## Build

```bash
cd ~/Desktop/Code/NRL-Server
chmod +x build.sh
./build.sh
```

## Quick Start

```bash
# Start the server (background)
./build/bin/nrl-serverd --port 8080 &

# Query server status
./build/bin/nrlctl status

# Run NRL lattice tick
./build/bin/nrlctl tick 10000 42

# Run a sandboxed command
./build/bin/nrlctl run "/bin/echo hello world"
```

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Server health (no auth required) |
| `GET` | `/v1/stats` | NRL telemetry + gate stats |
| `POST` | `/v1/tick` | Advance NRL lattice |
| `POST` | `/v1/run` | Execute sandboxed workload |
| `GET` | `/v1/jobs/<id>` | Inspect job state |

## Security

- Set auth token via `--token` flag or `NRL_SERVER_TOKEN` environment variable
- Server binds to `127.0.0.1` by default — not exposed on public interfaces
- Sandbox uses Linux namespaces, pivot_root, dropped capabilities
- Commands executed via `execve` with explicit argv (no shell injection)

## Benchmarking

```bash
# Run web benchmarks
./scripts/bench_web.sh
```

## License

MIT
