# podlogs

Ships the logs of every podman container on a host to [Grafana Loki](https://grafana.com/oss/loki/),
so you can search, filter and live-tail hundreds of microservices from one place — a
self-hosted Papertrail.

It is an external observer: nothing about how your containers are started changes.
The collector discovers containers through the podman API, follows their stdout/stderr
(`podman logs -f` semantics, but over the socket, one process for all containers),
normalises JSON / plain-text / unstructured lines, joins stack traces back together,
labels every line with the image name, image id, container name and more, and pushes
batches to Loki. Grafana is the UI.

```
 podman host
 ┌────────────────────────────────────────────────────────────────┐
 │  200+ containers ── stdout/stderr ──▶ podman                    │
 │                                          │ unix socket          │
 │  podlogs (one static C++ binary)         ▼                      │
 │   discover: containers/json + events stream + periodic resync   │
 │   follow:   /containers/{id}/logs?follow=true  (one per ctr)    │
 │   parse:    JSON → level/logger · text → level · multiline join │
 │   label:    image_name image_tag image_id container_name        │
 │             stream pod host compose_project …                   │
 │   ship:     batches → POST /loki/api/v1/push (ack → checkpoint) │
 └───────────────────────────┬────────────────────────────────────┘
                             ▼
                    Loki  ◀──  Grafana (Explore / Logs Drilldown / live tail)
```

## Quick start

Requirements: podman ≥ 4.x with its API socket enabled (below), Loki ≥ 3.0, Grafana ≥ 11.

**1. Enable the podman socket** (one time, on the host; this is the only host change):

```sh
# rootful podman (containers started by root / a system service)
sudo systemctl enable --now podman.socket          # → /run/podman/podman.sock

# rootless podman (containers started by a user) — run as THAT user
systemctl --user enable --now podman.socket        # → /run/user/<uid>/podman/podman.sock
loginctl enable-linger "$USER"                     # keep it alive without a login session
```

**2. Start Loki + Grafana + podlogs** with the bundled compose stack:

```sh
cd deploy
# rootless: point the stack at your user's socket
# export PODMAN_SOCKET="$XDG_RUNTIME_DIR/podman/podman.sock"
export PODLOGS_HOSTNAME="$(hostname)"
podman compose up -d          # or: docker compose up -d
```

Open <http://localhost:3000> (admin / admin), go to **Explore** or **Drilldown → Logs**,
pick `image_name` and start filtering. Loki stores data in a volume with 14-day retention
(`deploy/loki/loki-config.yaml`).

**3. Check what would be tailed** (works with the bare binary too):

```sh
podman exec podlogs podlogs --check
```

```
podman socket : /run/podman/podman.sock (ok)
loki          : http://loki:3100 (HTTP 200 ready)
checkpoint    : /var/lib/podlogs/checkpoint.json
  + 3f2a9c1d0b7e orders-1        image_name=registry.example.com/shop/orders image_tag=1.4.2 image_id=8d1e4f...
  + 91bb02ee7a10 payments-1      image_name=registry.example.com/shop/payments image_tag=2.0.0 image_id=...
  - a0c4d9e1f2b3 podlogs         image_name=ghcr.io/you/podlogs image_tag=latest ...  (excluded)
212 of 213 running containers would be tailed
```

## Running the collector

Pick one. All of them read the same config file (`deploy/podlogs.json`, every key optional,
comments allowed) and honour `PODLOGS_*` environment overrides (`podlogs --help`).

| How | When | Files |
|---|---|---|
| **compose** (above) | Loki/Grafana on the same host | `deploy/compose.yaml` |
| **quadlet** container under systemd | Loki elsewhere, you like images | `deploy/quadlet/podlogs.container` |
| **bare binary + systemd** (rootful) | no container runtime for the agent, RHEL 8 boxes | `deploy/systemd/podlogs.service` |
| **bare binary + systemd --user** (rootless) | containers run by a regular user | `deploy/systemd/podlogs-user.service` |

Bare-binary install on a RHEL 8 host, for example:

```sh
tar xzf podlogs-v1.0.0-rhel8-x86_64.tar.gz && cd podlogs-v1.0.0-rhel8-x86_64
sudo install -m 0755 podlogs /usr/local/bin/podlogs
sudo install -d /etc/podlogs && sudo install -m 0644 podlogs.json /etc/podlogs/podlogs.json
sudo install -m 0644 podlogs.service /etc/systemd/system/podlogs.service
sudo sed -i 's#http://127.0.0.1:3100#http://loki.internal:3100#' /etc/podlogs/podlogs.json
sudo systemctl enable --now podman.socket
sudo systemctl daemon-reload && sudo systemctl enable --now podlogs
sudo podlogs --check           # connectivity + list of containers
journalctl -u podlogs -f       # the collector's own log
```

Release assets (see [Releasing](#releasing)):

| Asset | Runs on |
|---|---|
| `podlogs-<ver>-linux-amd64.tar.gz` | any x86_64 Linux — fully static (musl), no dependencies |
| `podlogs-<ver>-linux-arm64.tar.gz` | any aarch64 Linux — fully static (musl) |
| `podlogs-<ver>-rhel8-x86_64.tar.gz` | RHEL 8 / Rocky 8 / Alma 8 — built with gcc-toolset-13 against glibc 2.28, libstdc++ static |
| `ghcr.io/OWNER/podlogs:<ver>` | container image, linux/amd64 + linux/arm64 |

The static amd64 binary also runs on RHEL 8; the rhel8 asset exists for shops that want
a binary built with the platform toolchain.

## Searching

Every line reaches Loki with these **stream labels** (indexed, use them in `{}`):

`job=podlogs` · `host` · `container_name` · `image_name` · `image_tag` · `image_id` (12-char) ·
`stream` (stdout/stderr) · `pod` (if in a pod) · `compose_project` / `compose_service` (if labelled)

and this **structured metadata** (not indexed, filter with `| key="value"`):

`container_id` (12-char) · `detected_level` (trace/debug/info/warn/error/fatal) · `logger` (from JSON)

LogQL cheat sheet:

```logql
{image_name="registry.example.com/shop/orders"}                    # one service, all versions
{image_id="8d1e4f0a2b3c"}                                          # one exact image build
{image_name=~".*/shop/.*", stream="stderr"}                         # a team's stderr
{job="podlogs"} |= "timeout" != "healthz"                           # grep across everything
{job="podlogs"} | detected_level="error"                            # errors from every container
{image_name=~".*orders"} | json | order_id="A-1234"                 # JSON logs: filter on any field
{image_name=~".*orders"} | json | duration_ms > 500                 # numeric comparisons
{container_name="orders-1"} | container_id="3f2a9c1d0b7e"           # a specific container instance
{job="podlogs"} |~ "(?i)nullpointer"                                # regex (stack traces are one entry)
sum by (image_name) (count_over_time({job="podlogs"} | detected_level="error" [5m]))   # error rate per service
```

In Grafana, **Explore → Live** gives the Papertrail-style tail; **Drilldown → Logs** gives
per-service volume, level breakdowns and click-to-filter without writing LogQL.

## How it works

- **Discovery.** On start it lists running containers, then follows the podman events stream
  and reconciles every `resync_interval` (15 s) as a safety net. Containers that appear, restart
  or disappear are picked up automatically. Pod infra containers and anything matching
  `podman.exclude` (default `^podlogs`) are skipped; `podman.include` narrows further.
- **Tailing.** One HTTP connection per container to `/libpod/containers/{id}/logs?follow=true&timestamps=true`.
  Podman's multiplexed frames are demuxed into stdout/stderr lines; the podman timestamp is the
  entry timestamp (so out-of-order application clocks cannot break ingestion).
- **Parsing.** A line that parses as a JSON object gets `detected_level` from `level`/`severity`/
  `log.level`/… (string or pino/python numeric) and `logger` from `logger`/`component`/…; the raw
  JSON is what gets stored, so `| json` works on every field. Plain text gets its level from the
  first `INFO`/`WARN`/`ERROR`/… token in the first 256 bytes. Lines that start with whitespace or
  look like stack-frame lines (`at …`, `Caused by:`, `java.lang.FooException: …`, `Traceback …`,
  `goroutine N [`, …) are appended to the previous entry, so a stack trace is one searchable entry.
- **At-least-once delivery.** Entries are batched (1 MiB / 5000 entries / 1 s) and pushed. Loki
  outages (connection refused, 5xx, 429) are retried with backoff forever; the in-memory queue
  fills, tailers pause, and podman keeps buffering on disk — nothing is dropped. The per-container
  checkpoint (`/var/lib/podlogs/checkpoint.json`) only advances after Loki acknowledges a batch,
  so a restart resumes from the newest acknowledged timestamp. Replayed duplicates (same
  timestamp + same line) are discarded by Loki. Only `4xx` responses drop a batch (e.g. entries
  older than Loki's `reject_old_samples_max_age`).
- **First run.** Containers with no checkpoint start from `podman.initial_lookback` (default
  `1h`; `"all"` replays full history — mind Loki's 7-day old-sample limit).
- **Cardinality.** Labels are per container/image, not per line: ~200 services ⇒ a few hundred
  Loki streams. `level` is metadata by default; set `labels.level_as_label` to make
  `{level="error"}` work at ~6× the stream count. Per-restart values (`container_id`) are metadata
  so restarts do not create new streams.

Metrics (Prometheus format) at `http://127.0.0.1:9151/metrics`, health at `/healthz`:
`podlogs_lines_read_total`, `podlogs_entries_shipped_total`, `podlogs_batches_dropped_total`,
`podlogs_push_errors_total`, `podlogs_queue_size`, `podlogs_containers_tailing`, `podlogs_loki_up`, …

## Configuration

See [`deploy/podlogs.json`](deploy/podlogs.json) — it lists every key with its default and a
comment. The most common changes:

```jsonc
{
  "loki":   { "url": "http://loki.internal:3100", "tenant": "", "basic_auth_user": "", "basic_auth_pass": "" },
  "podman": { "socket": "", "exclude": ["^podlogs", "^loki", "^grafana"], "initial_lookback": "1h" },
  "labels": { "static": { "job": "podlogs", "env": "prod", "dc": "ams1" } }
}
```

Environment overrides: `PODLOGS_CONFIG`, `PODLOGS_PODMAN_SOCKET`, `PODLOGS_LOKI_URL`,
`PODLOGS_LOKI_TENANT`, `PODLOGS_LOKI_BASIC_AUTH` (`user:pass`), `PODLOGS_CHECKPOINT_PATH`,
`PODLOGS_LOG_LEVEL`, `PODLOGS_METRICS_LISTEN`, `PODLOGS_HOSTNAME`, `PODLOGS_INITIAL_LOOKBACK`.

## Building

Everything is C++20 with no external dependencies beyond a vendored
[nlohmann/json](third_party/nlohmann) header, so the build containers need only a compiler and
CMake. The unit + end-to-end tests (a fake podman socket and a fake Loki) run inside every build.

```sh
# in the Docker build containers, exactly like CI
docker buildx build -f docker/Dockerfile.static --platform linux/amd64 --target artifact --output type=local,dest=out/amd64 .
docker buildx build -f docker/Dockerfile.static --platform linux/arm64 --target artifact --output type=local,dest=out/arm64 .
docker buildx build -f docker/Dockerfile.rhel8  --platform linux/amd64 --target artifact --output type=local,dest=out/rhel8 .
docker buildx build -f docker/Dockerfile.static --target runtime -t podlogs:local --load .   # container image

# natively (Linux or macOS, any C++20 compiler)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/podlogs_tests
```

`docker/Dockerfile.static` builds on Alpine with `-static` (musl). `docker/Dockerfile.rhel8`
builds on Rocky Linux 8 with `gcc-toolset-13`, links libstdc++/libgcc statically and asserts the
result needs no glibc symbol newer than 2.28.

## Releasing

**First time only** — put the project on GitHub and point the deploy files at it:

```sh
git init && git add -A && git commit -m "podlogs"
gh repo create your-org/podlogs --private --source . --push     # or create it in the UI and git push
sed -i 's/OWNER/your-org/g' deploy/compose.yaml deploy/quadlet/podlogs.container deploy/systemd/podlogs.service
git commit -am "point deploy files at your-org" && git push
```

Nothing else to configure: the workflows use the default `GITHUB_TOKEN` (`contents: write` for
the release, `packages: write` for GHCR are set in the workflow file). If the repo is private,
the GHCR image is private too — `podman login ghcr.io` with a token that has `read:packages`
on the hosts that pull it, or use the tarballs.

**Every release** — `.github/workflows/ci.yml` builds and tests every push and PR
(static amd64 + arm64, rhel8); `.github/workflows/release.yml` runs on a `v*` tag:

```sh
git tag v1.0.0
git push origin v1.0.0
```

It builds all three targets in their containers, packages each as
`podlogs-v1.0.0-<target>.tar.gz` (binary, `podlogs.json`, systemd units, quadlet, README),
writes `SHA256SUMS`, creates the GitHub release with auto-generated notes and the assets attached,
and pushes `ghcr.io/your-org/podlogs:v1.0.0`, `:1.0`, `:latest` (multi-arch). The release
appears at `https://github.com/your-org/podlogs/releases/tag/v1.0.0` about 15 minutes after the
push (the arm64 build runs under QEMU; switch its matrix entry to an `ubuntu-24.04-arm` runner if
your plan has one and you want it faster).

## Limitations

- Loki is reached over plain HTTP. For TLS, put nginx/Caddy/stunnel in front of Loki or
  next to podlogs (`basic_auth_*`/`tenant` are supported for gateways such as Grafana Enterprise
  or a reverse proxy).
- The podman API socket is required (podman ≥ 4.0 API). There is deliberately no
  `podman logs -f` subprocess fallback: 200 podman processes would cost several GB of RAM.
- Log lines are stored exactly as emitted (no field renaming), which is what keeps `| json`
  working for every service regardless of its logging library.
