# qp_post

`qp_post` is a DOCA RDMA write throughput benchmark with these fixed test assumptions:

- one client talks to two remote servers
- total `128` QPs
- `64` QPs per remote server
- each server exposes `1KB` writable MR per QP
- payload size is runtime-configurable in `[0, 1024]`
- outstanding writes per QP are runtime-configurable with `--sq-depth`

## Binaries

- `build/qp_post_server`
- `build/qp_post_client`

## Build

```bash
meson setup build . -Ddpa_thread_count=<1|2|4|...|128>
meson compile -C build
```

If omitted, `dpa_thread_count` defaults to `1`.

From the repository root you can also run:

```bash
make qp_post QP_POST_DPA_THREAD_COUNT=<1|2|4|...|128>
```

## Server

Run one instance on each remote server:

```bash
./build/qp_post_server --device <ib_hca> --gid-index <gid_index>
```

If both server instances run on the same host, start them with different `--port` values.

Example:

```bash
./build/qp_post_server --device mlx5_0 --gid-index 3
```

The server is host-only and exports `64` QPs with `1KB` MR per QP.

## Client

Host client:

```bash
./build/qp_post_client \
  --mode host \
  --device <ib_hca> \
  --gid-index <gid_index> \
  --server-a-ip <ip> \
  --server-b-ip <ip> \
  --server-a-port <port> \
  --server-b-port <port> \
  --sq-depth <count> \
  --payload-size <0..1024> \
  --duration <seconds>
```

DPA client on a host CX7 or BF3 PF:

```bash
./build/qp_post_client \
  --mode dpa \
  --device <ib_hca> \
  --gid-index <gid_index> \
  --server-a-ip <ip> \
  --server-b-ip <ip> \
  --server-a-port <port> \
  --server-b-port <port> \
  --sq-depth <count> \
  --cq-depth <count> \
  --payload-size <0..1024> \
  --duration <seconds>
```

For DPU split-device mode, use `--pf-device` and `--rdma-device`.
If both servers use the same port, you can omit `--server-a-port` and `--server-b-port` and just pass `--port`.
The DPA thread count is fixed at build time via `-Ddpa_thread_count`.
`--cq-depth` applies only to DPA mode and controls the per-QP DPA completion queue depth.

## Output

The client prints:

- mode
- payload size
- duration
- depth
- compile-time DPA thread count
- completed writes to server A
- completed writes to server B
- total writes
- writes per second
