# D450BT macOS Driver

Native CUPS driver for the D450BT thermal label printer (Aimo/Quyin family,
203 dpi, TSPL protocol). Replaces the vendor's Intel-only `rastertolabeltspl`
filter with a universal (arm64 + x86_64) binary. Output is verified
**byte-identical** to the vendor filter for its GrayScale and B&W modes on
identical raster input.

## Layout

- `src/rastertod450bt.c` — CUPS raster → TSPL filter
- `ppd/D450BT.ppd` — printer description (sizes, tracking, darkness, speed, rendering)
- `install.sh` — build, install filter + PPD, create the `D450BT` queue
- `tools/d450_print.py` — direct-to-serial test tool (bypasses CUPS)
- `Makefile`

## Install

```sh
./install.sh
```

Creates a queue named `D450BT` on the CUPS bluetooth backend (the printer
pairs as `D450BT-Z`; it does not enumerate as a USB device on this Mac even
when cabled — both USB ports on the unit appear to be power/charge only).

## Print

```sh
lp -d D450BT -o media=w288h432 file.pdf        # 4x6"
lp -d D450BT -o media=w144h90 file.pdf         # 2x1.25"
lp -d D450BT -o media=Custom.283x430 file.pdf  # custom, points
```

Options:
- `-o ColorOption=GrayScale|None|FloydSteinberg` — rendering (see below);
  default GrayScale
- `-o DegreeOfGrayRecognition=1..7` — GrayScale black floor (default 4 = 128)
- `-o BlackLevel=1..8` — B&W threshold (default 2 = 160)
- `-o Darkness=1..15|PrinterDefault` (default 8), `-o zePrintRate=2..6`
- `-o zeMediaTracking=Web|Mark|Continuous` (vendor spellings Gap/BLine ok),
  `-o GapHeight=2..6`, `-o GapOffset=0..3`
- `-o Rotate=180` — rotate output 180°
- `-o WaitForCompletion=0|15|60|180` — poll printer until :DONE after PRINT
  (default 60 s; 0 disables)

### Rendering modes

| Mode | Algorithm | 4x6 stream size | Use for |
|---|---|---|---|
| `GrayScale` (default) | Bayer 8×8 ordered dither (vendor-exact) | ~3-15 KB | everything, incl. photos |
| `None` | global threshold at 160 (vendor-exact) | ~2 KB | pure text/barcodes |
| `FloydSteinberg` | error diffusion | **~30-100 KB** | small labels only |

The Bluetooth path chokes on large compressed streams (see below), so
GrayScale's compressibility is not just cosmetic — it is what makes 4×6
labels print reliably. FS noise can even make an LZO block *expand* past
the 4096-byte chunk size (observed 4116 B), another suspected firmware
killer.

## Direct protocol testing (no CUPS)

```sh
tools/d450_print.py --test                      # geometric test pattern
tools/d450_print.py --text "HELLO"              # TSPL built-in font
tools/d450_print.py photo.png --width 100 --height 150
```

Writes TSPL straight to `/dev/cu.D450BT-Z` in 512-byte chunks (BT SPP drops
data on large writes).

## Protocol notes

Reverse-engineered from the vendor filter
(`/usr/libexec/cups/filter/rastertolabeltspl`, x86_64, unstripped; macOS
build v?, Linux build v2.10 has DWARF symbols) plus the community spec at
<https://github.com/thermal-label/labelife> (`docs/protocol/tspl.md`).

Per page (vendor command order, byte-exact):

```
SIZE <w> mm,<h> mm          # integer mm, rounded
REFERENCE 0,0
DIRECTION 0,0
GAP <g>.0 mm,<o> mm         # or BLINE <g> mm,<o> mm | GAP 0 mm,0 mm
SET TEAR ON
OFFSET 0 mm
DENSITY <1-15>              # only when Darkness is set
SPEED <ips>
CLS
BITMAP 0,0,<rowBytes>,<rows>,4,<stream>
PRINT 1,<copies>
```

Line terminator `\r\n`. Raster input is 8-bit gray (`cupsColorSpace 0`,
255 = white) at 203 dpi.

### Rendering (vendor-exact, from disassembly)

- **GrayScale** (`gray==1` in vendor): pixel ≥ 246 → white; pixel <
  `graylevellist[DegreeOfGrayRecognition-1]` (192,160,144,**128**,96,64,16)
  → black; otherwise black iff pixel ≤ 4×Bayer8[y%8][x%8] (standard Bayer
  8×8 index matrix).
- **B&W**: black iff pixel ≤ `blacklevellist[BlackLevel-1]`
  (192,**160**,144,128,112,96,64,0).
- Vendor PPD claims `DefaultRotate: 1` (180°) but with no explicit option
  the filter's global stays 0 — effective default is unrotated.

### BITMAP mode 4 (required over Bluetooth)

The printer's Bluetooth link **silently discards large uncompressed
BITMAPs**. The vendor's Bluetooth PPDs (`cupsModelNumber` = model + 100,
e.g. 21 → 121) switch the filter to BITMAP mode 4:

- 1bpp bitmap (bit 0 = black), split into 4096-byte chunks
- each chunk compressed with **LZO1X-1** (the filter vendors minilzo;
  so do we, in `src/minilzo/`)
- each block framed as `<u32le compressed-length><compressed bytes>`
- stream terminated by a u32le `0`

Other modes (per labelife spec): 0/1/2 = stock TSPL II uncompressed
(vendor uses mode 1 for D450 over USB, 128-byte pacing); mode 3 =
whole-frame LZO with decimal-ASCII length.

### Pacing / flow control (vendor "SendNowAndRead")

The vendor filter writes the framed stream in **32-byte slices**; after
each slice it flushes stdout and issues
`cupsSideChannelDoRequest(CUPS_SC_CMD_DRAIN_OUTPUT, timeout=0.05)` (0.01
in the Linux build) — i.e. it asks the *backend* to confirm delivery to
the device before sending more. It is **not** a back-channel read (an
earlier version of this driver replicated the wrong call). macOS's
bluetooth backend responds to side-channel commands, so this is real
flow control when supported and degrades to 50 ms/slice pacing when not.

### Status channel (SPP)

- Per-job ACK: printer sends `7E 01 7E` (accepted) / `7E 00 7E`
  (rejected → vendor apps resend up to 3×).
- Text queries (`\r\n`-terminated): `SSSGETPRINTING` → `:DOING/:DONE/
  :PAUSE`, `SSSGETPAPER` → `:YES/:JAM/:EMPTY`, `SSSGETSN`,
  `SSSGETVERSION`, `SSSGETBMAPMODE`, `SSSCANCELPRINT`; `INITIALPRINTER`
  = soft reset. Binary query `1F 11 2F` → records `1A 1D 00` (free) /
  `1A 1D 01` (busy) (used by the vendor filter's `waitPrinter()` — dead
  code in the macOS build).
- Our filter polls `SSSGETPRINTING` after `PRINT` (option
  `WaitForCompletion`) and logs ACK/NACK frames to the job log.

### Large-job failure over Bluetooth (history)

Symptom: jobs transmit fully but never print. Known-good: vendor 4×6
(17.5 KB compressed, thresholded). Failing: our FS 4×6 (33 KB; also with
blocks capped ≤2 KB). Small FS labels (3 KB) fine. Candidate causes, in
likelihood order: total compressed-size ceiling (~20-24 KB input
buffer?), LZO chunk expansion >4096 B, missing drain-based flow control.
GrayScale rendering (~3 KB for a full 4×6) sidesteps all three; the
instrumentation (drain stats + ACK/NACK + SSSGETPRINTING) will pinpoint
the cause if it recurs.

## License

GPL-2.0 (see `LICENSE`) — required by the vendored
[miniLZO](https://www.oberhumer.com/opensource/lzo/) in `src/minilzo/`.

### Bluetooth serial caveat

`/dev/cu.D450BT-Z` open() can hang forever in the kernel driver
(uninterruptible `U` state — unkillable even with SIGKILL; the zombies
block every later open until reboot). Prefer the CUPS `bluetooth://`
backend, which uses RFCOMM sockets and reconnects reliably. If using the
serial port, connect the baseband first (`blueutil --connect
0a-74-00-13-82-9d`) and only then open the port.
