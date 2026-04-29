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

Run one instance per remote device. This is the tested setup with both servers on `mcnode26`:

```bash
ssh mcnode26 \
  'numactl --cpunodebind=2 --membind=2 \
     /home/gut0a/proj/dpa-bench/qp_post/build_verify_64/qp_post_server \
       --device mlx5_0 \
       --gid-index 3 \
       --port 18645'

ssh mcnode26 \
  'numactl --cpunodebind=3 --membind=3 \
     /home/gut0a/proj/dpa-bench/qp_post/build_verify_64/qp_post_server \
       --device mlx5_3 \
       --gid-index 3 \
       --port 18646'
```

Here `mlx5_0` is `mcnode26` CX7 on NUMA node 2 (`10.200.2.26`), and `mlx5_3` is `mcnode26` BF3 on NUMA node 3 (`10.200.2.126`).

The server is host-only and exports `64` QPs with `1KB` MR per QP.

## Client

Host client example:

```bash
numactl --cpunodebind=3 --membind=3 \
  ./build_verify_64/qp_post_client \
  --mode host \
  --device mlx5_5 \
  --gid-index 3 \
  --server-a-ip 10.200.2.26 \
  --server-b-ip 10.200.2.126 \
  --server-a-port 18645 \
  --server-b-port 18646 \
  --sq-depth 128 \
  --payload-size 1 \
  --duration 10
```

DPA client on local `mcnode27` BF3 (`mlx5_5`, NUMA node 3):

```bash
numactl --cpunodebind=3 --membind=3 \
  ./build_verify_64/qp_post_client \
  --mode dpa \
  --device mlx5_5 \
  --gid-index 3 \
  --server-a-ip 10.200.2.26 \
  --server-b-ip 10.200.2.126 \
  --server-a-port 18645 \
  --server-b-port 18646 \
  --sq-depth 128 \
  --cq-depth 256 \
  --payload-size 1 \
  --duration 10
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
