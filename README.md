# RDMA Write Ping-Pong

Minimal 8KB `RDMA_WRITE_WITH_IMM` latency tests using either plain ibverbs or DOCA RDMA/DPA.

## Build

Build the original ibverbs version:

```bash
make
```

Build the DOCA/DPA version:

```bash
make dpa
```

## Original Ibverbs Usage

On server machine:

```bash
./server <ib_hca> <ib_port> <gid_index>
# Example: ./server mlx5_0 1 3
```

On client machine:

```bash
./client <ib_hca> <ib_port> <gid_index> <server_ip>
# Example: ./client mlx5_0 1 3 192.168.1.100
```

Parameters:

- `ib_hca`: RDMA device name (for example `mlx5_0`)
- `ib_port`: device port number (typically `1`)
- `gid_index`: GID table index for RoCE routing
- `server_ip`: server IP address (client only)

## DOCA/DPA Usage

The `dpa` directory builds two binaries:

- `./dpa/build/latency_server`
- `./dpa/build/latency_client`

The DOCA client is always a CPU host process. The server can run either on CPU host or on DPA.

Run a host-mode server:

```bash
./dpa/build/latency_server --mode host --device <ib_hca> --gid-index <gid_index>
# Example: ./dpa/build/latency_server --mode host --device mlx5_0 --gid-index 3
```

Run a DPA-mode server:

```bash
./dpa/build/latency_server --mode dpa --device <pf_ib_hca> --rdma-device <rdma_ib_hca> --gid-index <gid_index>
# Host example: ./dpa/build/latency_server --mode dpa --device mlx5_0 --gid-index 3
# BF3 Arm example: ./dpa/build/latency_server --mode dpa --pf-device mlx5_0 --rdma-device mlx5_2 --gid-index 0
```

Run the client:

```bash
./dpa/build/latency_client --device <ib_hca> --gid-index <gid_index> <server_ip>
# Example: ./dpa/build/latency_client --device mlx5_4 --gid-index 3 10.200.2.27
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
- `--rdma-device` selects the SF/PF device that hosts the RDMA context
- all RDMA-related DPA resources are created on the RDMA device's extended DPA context

## Finding Parameters with show_gids

The easiest way to find the correct parameters is using the `show_gids` command:

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

**Choosing the right GID index:**

- Entries with **IPv4 addresses** (e.g., `10.200.3.1`) are for RoCEv1/v2 over IPv4
- Entries starting with **fe80:** are link-local IPv6 addresses
- **v1** = RoCEv1 (runs directly over Ethernet)
- **v2** = RoCEv2 (runs over UDP/IP, more commonly used)

For most setups, choose a GID index with an IPv4 address and **v2** (RoCEv2). In the example above, that would be **index 3**.

```bash
# Server (IP: 10.200.3.1)
./server mlx5_0 1 3

# Client
./client mlx5_0 1 3 10.200.3.1

# DOCA host server
./dpa/build/latency_server --mode host --device mlx5_0 --gid-index 3

# DOCA client
./dpa/build/latency_client --device mlx5_0 --gid-index 3 10.200.3.1
```

## Output

```
Device: mlx5_0, Port: 1, GID Index: 3
PING server_ip: 8192 bytes
seq=1 time=12.34 us
seq=2 time=11.89 us
...
```

The DOCA client prints the same `seq=<n> time=<us>` format.

## Requirements

- libibverbs-dev
- RDMA-capable NIC (InfiniBand or RoCE)
- DOCA 3.x and DPA-capable hardware for `dpa` mode
