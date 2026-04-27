# RDMA Write Benchmarks

Minimal RDMA write benchmarks using either plain ibverbs or DOCA RDMA/DPA.

## Layout

- `p2p_rtt/verbs`: plain ibverbs ping-pong client and server
- `p2p_rtt/doca`: DOCA RDMA/DPA client and server
- `qp_post`: 128-QP DOCA write-throughput benchmark with host and DPA client modes

## Build

Build the ibverbs version:

```bash
make
```

This produces:

- `./p2p_rtt/verbs/server`
- `./p2p_rtt/verbs/client`

Build the DOCA/DPA version:

```bash
make dpa
```

This produces:

- `./p2p_rtt/doca/build/latency_server`
- `./p2p_rtt/doca/build/latency_client`

Build the multi-QP throughput benchmark:

```bash
make qp_post
```

This produces:

- `./qp_post/build/qp_post_server`
- `./qp_post/build/qp_post_client`

## Ibverbs Usage

On the server machine:

```bash
./p2p_rtt/verbs/server <ib_hca> <ib_port> <gid_index>
# Example: ./p2p_rtt/verbs/server mlx5_0 1 3
```

On the client machine:

```bash
./p2p_rtt/verbs/client <ib_hca> <ib_port> <gid_index> <server_ip>
# Example: ./p2p_rtt/verbs/client mlx5_0 1 3 192.168.1.100
```

Parameters:

- `ib_hca`: RDMA device name, for example `mlx5_0`
- `ib_port`: device port number, typically `1`
- `gid_index`: GID table index for RoCE routing
- `server_ip`: server IP address, client only

## DOCA/DPA Usage

The DOCA client is always a CPU host process. The server can run either on CPU host or on DPA.

Run a host-mode server:

```bash
./p2p_rtt/doca/build/latency_server --mode host --device <ib_hca> --gid-index <gid_index>
# Example: ./p2p_rtt/doca/build/latency_server --mode host --device mlx5_0 --gid-index 3
```

Run a DPA-mode server:

```bash
./p2p_rtt/doca/build/latency_server --mode dpa --device <pf_ib_hca> --rdma-device <rdma_ib_hca> --gid-index <gid_index>
# Host example: ./p2p_rtt/doca/build/latency_server --mode dpa --device mlx5_0 --gid-index 3
# BF3 Arm example: ./p2p_rtt/doca/build/latency_server --mode dpa --pf-device mlx5_0 --rdma-device mlx5_2 --gid-index 0
```

Run the client:

```bash
./p2p_rtt/doca/build/latency_client --device <ib_hca> --gid-index <gid_index> <server_ip>
# Example: ./p2p_rtt/doca/build/latency_client --device mlx5_4 --gid-index 3 10.200.2.27
```

Useful DOCA options:

- `--device <ib_hca>`: RDMA device name
- `--pf-device <ib_hca>`: DPA PF device name for DPU mode
- `--rdma-device <ib_hca>`: RDMA device name for DPU mode
- `--port <tcp_port>`: TCP port used to exchange descriptors, default `18515`
- `--gid-index <gid_index>`: RoCE GID index
- `--iters <count>`: number of client RTT samples, `0` means run forever
- `--interval-us <delay>`: delay between client samples in microseconds
- `--mode <host|dpa>`: server datapath mode

When running the DPA server on a BlueField DPU, follow the official DOCA split-device model:

- `--pf-device` selects the PF that hosts the DPA context
- `--rdma-device` selects the SF or PF device that hosts the RDMA context
- all RDMA-related DPA resources are created on the RDMA device's extended DPA context

## Finding Parameters with show_gids

The easiest way to find the correct parameters is using `show_gids`:

```bash
$ show_gids
DEV     PORT    INDEX   GID                                     IPv4            VERDEV
---     ----    -----   ---                                     ------------    ------
mlx5_0  1       0       fe80:0000:0000:0000:5200:e6ff:fea9:4f38                 v1 fabric-06
mlx5_0  1       1       fe80:0000:0000:0000:5200:e6ff:fea9:4f38                 v2 fabric-06
mlx5_0  1       2       0000:0000:0000:0000:0000:ffff:0ac8:0301 10.200.3.1      v1 fabric-06
mlx5_0  1       3       0000:0000:0000:0000:0000:ffff:0ac8:0301 10.200.3.1      v2 fabric-06
```

The columns map directly to CLI arguments:

| Column | CLI Argument | Description |
|--------|--------------|-------------|
| DEV    | `ib_hca`     | RDMA device name |
| PORT   | `ib_port`    | Device port number |
| INDEX  | `gid_index`  | GID table index |

Choose a GID index with an IPv4 address and `v2` in most RoCE setups. In the example above, that is index `3`.

```bash
# Server (IP: 10.200.3.1)
./p2p_rtt/verbs/server mlx5_0 1 3

# Client
./p2p_rtt/verbs/client mlx5_0 1 3 10.200.3.1

# DOCA host server
./p2p_rtt/doca/build/latency_server --mode host --device mlx5_0 --gid-index 3

# DOCA client
./p2p_rtt/doca/build/latency_client --device mlx5_0 --gid-index 3 10.200.3.1
```

## Output

```text
Device: mlx5_0, Port: 1, GID Index: 3
PING server_ip: 8192 bytes
seq=1 time=12.34 us
seq=2 time=11.89 us
...
```

The DOCA client prints the same `seq=<n> time=<us>` format.

## Requirements

- `libibverbs-dev`
- RDMA-capable NIC, InfiniBand or RoCE
- DOCA 3.x and DPA-capable hardware for DPA mode
