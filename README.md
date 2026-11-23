# RetroPLC Runtime

RetroPLC Runtime is the manifest repository for a Zephyr T2 workspace. The
workspace is an IDE-managed, versioned platform installation shared by all PLC
projects. PLC projects are build inputs; they are not Zephyr applications and
do not contain west workspaces or copies of Zephyr.

## Workspace layout

```text
retroplc-platform/
├── .west/
├── .venv/
├── retroplc-runtime/          # this manifest repository
│   ├── west.yml
│   └── app/                   # central Zephyr application
├── zephyr/
├── modules/
└── bootloader/
```

A PLC project remains outside that workspace:

```text
MyMachine/
├── ProjectFiles/              # Structured Text source
├── hardware/
│   └── target.overlay
└── .retroplc/
    ├── generated/             # STruCpp .cpp/.hpp output
    └── build/                 # project-specific Zephyr output
```

## Initialize the platform

Install `west` in the platform's private Python environment, then initialize
the workspace from this local manifest repository:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install west
.venv/bin/west init -l retroplc-runtime
.venv/bin/west update
.venv/bin/west packages pip --install
```

The workspace directory containing `.west/` must not itself be a Git
repository. `retroplc-runtime/` is the manifest Git repository.

## Build a PLC project

Run west with the platform workspace as its working directory. Source and
build paths should be absolute when invoked by the IDE:

```sh
.venv/bin/west build -p always --sysbuild \
  -b arduino_opta/stm32h747xx/m7 \
  -s "$PWD/retroplc-runtime/app" \
  -d "/path/to/MyMachine/.retroplc/build" \
  -- \
  -DRETROPLC_GENERATED_DIR="/path/to/MyMachine/.retroplc/generated" \
  -DEXTRA_DTC_OVERLAY_FILE="/path/to/MyMachine/hardware/target.overlay"
```

`RETROPLC_GENERATED_DIR` is optional for a platform-only smoke build. When it
is supplied, all `.cpp`, `.cc`, and `.cxx` files immediately inside it are
compiled into the firmware, and the directory is added to the include path.

`EXTRA_DTC_OVERLAY_FILE` is a standard Zephyr CMake setting. It adds the
project-specific hardware overlay without making the PLC project part of the
west workspace.

## Manage an Opta over USB

The runtime enables MCUmgr's serial transport on the Opta M7 USB CDC-ACM
device. Connect the Opta's USB port, identify its serial device (for example,
`/dev/ttyACM0` on Linux), and query it with an MCUmgr client:

```sh
./mcumgrctl-linux -s /dev/ttyACM0 os echo hello
```

## Long-poll PLC scan snapshots

The runtime registers MCUmgr group `64`, command `0`. A read without an
`after` value waits for the next completed PLC scan and returns the located
variables captured at that scan boundary:

```sh
./mcumgrctl-linux -s /dev/ttyACM0 --json -t 6000 \
  raw read 64 0 '{"timeout_ms":5000}'
```

To wait for a generation newer than one already received, include it as
`after`:

```sh
./mcumgrctl-linux -s /dev/ttyACM0 --json -t 6000 \
  raw read 64 0 '{"after":42,"timeout_ms":5000}'
```

The response contains the scan generation and canonical IEC addresses. Bit
locations use boolean `value`; wider locations use unsigned `raw` because an
address width alone does not identify the signed IEC type:

```json
{
  "scan": 43,
  "timed_out": false,
  "total": 2,
  "count": 2,
  "truncated": false,
  "vars": [
    {"address": "QX0.0", "value": true},
    {"address": "MW10", "raw": 1234}
  ]
}
```

`CONFIG_RETROPLC_MGMT_MAX_LOCATED_VARS` controls the snapshot capacity and
defaults to 64. The client timeout (`-t`) must be longer than `timeout_ms`.

The same USB serial device remains available as the Zephyr console; MCUmgr
packets are distinguished by their SMP-over-console framing. The runtime
exposes OS management commands and MCUboot image management.

The Opta build uses MCUboot's swap-using-offset mode without image-signing
keys. The M7 primary and secondary slots are both 512 KiB and use the same
128 KiB erase-sector layout. Offset swap uses the first secondary sector as
working space, leaving approximately 384 KiB for an M7 firmware image. The
final 768 KiB of internal flash remains unpartitioned for the future M4 image
pair; M4 image management is not enabled yet.

The first installation must program both the application and MCUboot using a
wired SWD debug probe. For example, with STM32CubeProgrammer support, flash the
application first and the bootloader last so the final reset enters MCUboot:

```sh
.venv/bin/west flash -d /path/to/build --domain app \
  --runner stm32cubeprogrammer
.venv/bin/west flash -d /path/to/build --domain mcuboot \
  --runner stm32cubeprogrammer
```

This replaces the Arduino bootloader. Keep an SWD probe available for recovery.
Subsequent updates can be installed through the running application:

```sh
./mcumgrctl-linux -s /dev/ttyACM0 firmware update \
  /path/to/build/app/zephyr/zephyr.signed.bin
```

Keyless images contain an MCUboot header and SHA-256 integrity hash but no
authenticity protection. Anyone with access to the MCUmgr serial endpoint can
install firmware; use signing keys before deploying the runtime outside a
controlled development environment.

## Read symbolic PLC variables

Read MCUmgr group `64`, command `1` with an unsigned `offset` and `limit`.
The defaults are offset 0 and up to 8 entries. The limit must be positive and
at most `CONFIG_RETROPLC_MGMT_MAX_DEBUG_ENTRIES` (default 16). Offsets from 0
through `total` are valid, including the empty page at `offset == total`.

The reply contains `scan`, `total`, `offset`, `count`, `next` and `vars`.
Each record contains `id` (the element index in array 0), `type` (the
STruC++ numeric type tag), and `data` (a CBOR byte string in the upstream
debug format, little-endian for Opta scalar values). If the value cannot be
read, the record carries a numeric `status` in place of `data`.

| Status | Meaning |
| --- | --- |
| 1 | Invalid entry pointer or type tag |
| 2 | Unsupported storage type or zero-size/unavailable value |
| 3 | Value exceeds `CONFIG_RETROPLC_MGMT_MAX_DEBUG_VALUE_SIZE` (default 253 bytes) |

Use `next` as the following request's offset until `next == total`.
The runtime may return fewer entries than requested to fit the SMP response.
Resolve each entry using `(0, id)` in the project's `debug_map.json`.

The PLC captures a requested page after program execution at a scan boundary.
Each page is internally coherent, and separate requests can return different
`scan` generations. The read times out after five seconds if no scan can
service it. Firmware built without `generated_debug.cpp` returns MCUmgr
`ENOENT` for command 1. STruC++ v0.6.3 emits some wrappers (such as enum and
subrange storage and custom-capacity strings) under the same tags as its
elementary storage types. Those entries are enumerated with status 2 when
the upstream dispatcher cannot safely read their C++ storage.

## Platform-owned STruCpp runtime

The headers under `app/src/strucpp-runtime/` are compiled against every
generated PLC program. They must remain pinned to the STruCpp compiler version
distributed with the IDE. `COPYING` and `COPYING.RUNTIME` record their upstream
license and runtime exception.

The Zephyr application supplies the embedded `iec_runtime_fault()` hook. The
PLC scheduler, generated configuration registration, and board-specific I/O
image adapter are separate runtime layers and must be implemented before a
generated program controls physical outputs.
