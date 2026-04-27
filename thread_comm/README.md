# DPA Thread Communication Example

This sample shows the smallest useful `DPA thread A -> DPA thread B` communication path in this repository.

Flow:

1. Host creates two DPA threads.
2. Host creates a notification completion for each thread.
3. Host wakes thread A with an RPC.
4. Thread A writes a shared DPA heap struct and notifies thread B.
5. Thread B reads the shared struct and signals the host with a `sync_event`.

This keeps the communication model explicit:

- data path: shared DPA heap memory
- wakeup path: `doca_dpa_dev_thread_notify()`
- host completion path: `doca_sync_event`

## Build

```bash
meson setup thread_comm/build thread_comm
meson compile -C thread_comm/build
```

## Run

```bash
./thread_comm/build/dpa_thread_comm --device <ib_hca>
```

Example:

```bash
./thread_comm/build/dpa_thread_comm --device mlx5_0
```

## Expected Result

The host should print that:

- thread A wrote the shared message
- thread B observed it
- thread B replied through the same shared struct
- the host sync event was raised
