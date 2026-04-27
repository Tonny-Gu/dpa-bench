# DPA ISA Notes

## Summary

This repository's DPA device code is built with NVIDIA's `dpacc` / `dpa-clang` toolchain targeting `riscv64-unknown-unknown-elf`.

The configured DPA CPU targets in this repo are:

- `nv-dpa-bf3`
- `nv-dpa-cx7`
- `nv-dpa-cx8`

These targets are configured in:

- `p2p_rtt/doca/meson.build:19`
- `thread_comm/meson.build:19`
- `timer/meson.build:19`

The same default appears in DOCA's app meson options:

- `/opt/mellanox/doca/applications/meson_options.txt:56`

## Supported DPA CPU Targets

From `dpa-clang -print-supported-cpus` on this machine:

- `generic`
- `generic-rv32`
- `generic-rv64`
- `nv-dpa-bf3`
- `nv-dpa-cx7`
- `nv-dpa-cx8`
- `nv-dpa-generic`

## Default Extensions By Target

The most useful distinction is between:

- extensions the compiler knows about
- extensions enabled by default for a specific DPA `-mcpu`

The second list is what matters for the DPA targets used in this repo.

### `nv-dpa-bf3`

Default standard extensions observed via `dpa-clang -### -mcpu=nv-dpa-bf3` and `-dM -E`:

- `i`
- `m`
- `a`
- `c`
- `zbb`
- `zbp`
- `zbr`
- `zbs`

Default vendor/custom extensions:

- `xfenceheap`
- `xnvcc`
- `xrpfxp`

Observed predefined macros:

```c
#define __riscv_xfenceheap 1000000
#define __riscv_xnvcc 1000000
#define __riscv_xrpfxp 1000000
#define __riscv_zbb 1000000
#define __riscv_zbp 93000
#define __riscv_zbr 93000
#define __riscv_zbs 1000000
```

### `nv-dpa-cx7`

`nv-dpa-cx7` matches the same default extension set observed for `nv-dpa-bf3`:

- standard: `i m a c zbb zbp zbr zbs`
- custom: `xfenceheap xnvcc xrpfxp`

Observed predefined macros are the same as `nv-dpa-bf3` for these extensions.

### `nv-dpa-cx8`

`nv-dpa-cx8` enables everything listed above plus additional defaults:

- added standard extensions: `zba`, `zihintpause`
- added custom extensions: `xblkop`, `xcas`, `xflush`

Observed predefined macros:

```c
#define __riscv_xblkop 1000000
#define __riscv_xcas 1000000
#define __riscv_xflush 1000000
#define __riscv_zba 1000000
#define __riscv_zihintpause 2000000
```

## Meaning Of The Custom DPA Extensions

The most important custom extensions seen in the `bf3` and `cx7` targets are:

- `xfenceheap`
- `xnvcc`
- `xrpfxp`

These are not generic RISC-V standard extensions. They are DPA-specific extensions exposed by NVIDIA's DPA toolchain.

### `xfenceheap`

Compiler description from `dpa-clang -print-supported-extensions`:

- `xfenceheap`: `mellanox heap memory fence`

This extension provides a heap-specific fence instruction for the DPA heap memory space.

DOCA exposes it through the generic DPA fence API:

```c
__dpa_thread_fence(__DPA_HEAP, __DPA_RW, __DPA_RW)
```

On this machine, that compiles to:

```asm
fence.heap rw, rw
```

Practical meaning: use it when ordering or visibility requirements apply specifically to DPA heap memory rather than to the broader system or MMIO spaces.

### `xnvcc`

Compiler description from `dpa-clang -print-supported-extensions`:

- `xnvcc`: `Cache Invalidate Instruction`

DOCA exposes this through:

```c
__dpa_data_ignore(addr)
```

On this machine, that compiles to:

```asm
cinva a0
```

Practical meaning: this is an address-based cache invalidation primitive. In the DPA intrinsics header it is exposed as `data_ignore`, and in assembly it lowers to `cinva`.

### `xrpfxp`

Compiler description from `dpa-clang -print-supported-extensions`:

- `xrpfxp`: `Fixed Point Arithmetic Instructions`

DOCA exposes three fixed-point helpers when this extension is present:

- `__dpa_fxp_rcp(x)`
- `__dpa_fxp_pow2(x)`
- `__dpa_fxp_log2(x)`

The header documents them as Q16.16 operations:

- reciprocal `1/x`
- power of two `2^x`
- base-2 logarithm `log2(x)`

On this machine, they compile to:

```asm
fxprcp   a0, a0
fxppw2   a0, a0
fxplog2  a0, a0
```

Practical meaning: this extension gives the DPA fast fixed-point math support without enabling full floating-point extensions such as `f` or `d`.

## Meaning Of The `cx8`-Only Additions

Compared with `bf3` and `cx7`, `cx8` adds three important custom extensions:

- `xblkop`
- `xcas`
- `xflush`

### `xflush`

Compiler description from `dpa-clang -print-supported-extensions`:

- `xflush`: `Flush L1 instruction`

DOCA exposes this through:

```c
__dpa_thread_l1_flush()
```

On this machine, that compiles to:

```asm
flush.l1
```

Practical meaning: this is an explicit L1 flush primitive on `cx8`.

### `xcas`

Compiler description from `dpa-clang -print-supported-extensions`:

- `xcas`: `Atomic Compare and Swap`

DOCA exposes this through:

```c
__dpa_remote_atomic_compare_exchange(ptr, expected, desired,
    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
```

The DPA intrinsics header documents this as a compare-exchange operation that:

- loads `*PTR`
- compares it with `*EXPECTED`
- stores `*DESIRED` if equal

On this machine, it compiles to:

```asm
amocas.w
amocas.d
```

Practical meaning: `cx8` adds hardware compare-and-swap support for remote atomics. This is the clearest new synchronization primitive added beyond the `bf3` / `cx7` baseline.

### `xblkop`

Compiler description from `dpa-clang -print-supported-extensions`:

- `xblkop`: `Bulk Register Zeroing Instructions`

Current evidence in this environment is more limited than for `xcas` and `xflush`:

- the compiler advertises the extension by name
- the `cx8` target enables it by default
- there is no obvious public DOCA intrinsic for it in `dpaintrin.h`

Practical meaning: this appears to be a `cx8` extension intended for efficiently zeroing groups of registers, likely as a compiler-visible code generation feature rather than a commonly used application-facing intrinsic.

At the moment, the safest statement is:

- `xcas` and `xflush` have directly visible DOCA APIs and confirmed mnemonics
- `xblkop` is confirmed as a supported `cx8` extension, but its exact public programming surface is not exposed in the headers inspected here

## Not Enabled By Default

These DPA targets do not appear to enable the following by default:

- `f`
- `d`
- `v`

The macro dump also shows soft-float:

```c
#define __riscv_float_abi_soft 1
```

## Compiler-Known Extensions vs DPA Defaults

`dpa-clang -print-supported-extensions` prints a much larger RISC-V extension list, including standard, experimental, and vendor extensions.

That list should not be read as "all DPA hardware features are enabled". It only means the compiler understands those `-march` extension names. The DPA hardware defaults for this repo are the smaller per-`-mcpu` sets listed above.

## Commands Used

The findings above were derived from these local commands:

```bash
/opt/mellanox/doca/tools/dpacc --version
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang --version
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -print-supported-cpus
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -print-supported-extensions
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -### -mcpu=nv-dpa-bf3 -x c -c /dev/null
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -### -mcpu=nv-dpa-cx7 -x c -c /dev/null
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -### -mcpu=nv-dpa-cx8 -x c -c /dev/null
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -dM -E -mcpu=nv-dpa-bf3 -x c /dev/null
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -dM -E -mcpu=nv-dpa-cx7 -x c /dev/null
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -dM -E -mcpu=nv-dpa-cx8 -x c /dev/null
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -mcpu=nv-dpa-bf3 -S -O2 -x c - -o -
/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/bin/dpa-clang -mcpu=nv-dpa-cx8 -S -O2 -x c - -o -
```

## Practical Takeaway

For code in this repo, the safest shorthand is:

- `bf3/cx7`: `rv64imac` + `zbb zbp zbr zbs` + NVIDIA custom DPA extensions
- `cx8`: the same baseline plus `zba zihintpause xblkop xcas xflush`

If code starts depending on a specific custom instruction family, it is worth verifying whether the dependency is valid for both `bf3/cx7` and `cx8`, or only for `cx8`.
