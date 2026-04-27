# DPA Timer Example

This example launches a small DPA RPC that busy-waits for about 1 second on the DPA and then returns the measured time in microseconds.

## Files

- `main.c`: host process that creates the DPA context and calls the RPC
- `wait_one_second_dev.c`: DPA-side wait loop
- `meson.build`: build entry for this example

## Requirements

- DOCA 3.x with DPA support
- A DPA-capable device visible to the host
- `meson` and `ninja`

## Build

From the repository root:

```bash
meson setup timer/build timer
meson compile -C timer/build
```

This produces:

- `./timer/build/dpa_wait_1s`

## Run

Run with an explicit RDMA device name:

```bash
./timer/build/dpa_wait_1s --device mlx5_4
```

If `--device` is omitted, the program selects the first visible DPA-capable device.

To list device names on the machine:

```bash
ibv_devices
show_gids
```

On this host, the BlueField-3 ports are:

- `mlx5_4` (`bf3p0`)
- `mlx5_5` (`bf3p1`)

## Example Output

```text
Running 1-second DPA wait on mlx5_4
DPA kernel max run time: 12 s
DPA reported wait: 1000000 us
Host observed RPC time: 1013861 us
```

## Notes

- The DPA-side timer uses the device `rdtime` counter.
- The example waits for `1000000` microseconds on the DPA side.
- If startup fails with an unsupported-operation error, the selected device or environment likely does not support DPA execution for this sample.
