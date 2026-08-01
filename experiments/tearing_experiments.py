#!/usr/bin/env python3
"""
Standalone tearing-mitigation experiment harness for the Thermaltake round LCD
(264a:233c). Talks straight to the interface-1 hidraw node using the validated
live-stream protocol (SOI-last chunk reordering, 1ms/chunk, EP4 ack drain,
~20fps pacing) -- independent of OpenRGB, so free the device first:

    pkill -x openrgb

Then e.g.:

    python3 tearing_experiments.py shuppet.gif --mode checker
    python3 tearing_experiments.py nyan.gif    --mode double
    python3 tearing_experiments.py nyan.gif    --mode blend
    python3 tearing_experiments.py nyan.gif    --mode normal   # baseline

Modes (all attack frame-to-frame CHANGE, which is what the panel tears on):
  normal   - stream frames as-is (baseline for comparison)
  double   - send every frame twice (A,A,B,B): 2nd identical write finishes any
             interrupted raster overwrite with zero visible change
  blend    - insert a 50% blended frame between each pair (A, (A+B)/2, B): halves
             the magnitude of every transition
  checker  - 16x16-block temporal checkerboard: each frame only updates half the
             blocks (alternating parity), holding the rest at the previous
             frame's pixels, so only ~half the panel can tear per frame

If the endpoint wedges (writes start timing out), physically replug the panel.
Ctrl-C to stop; the panel reverts to its own idle graphic shortly after.
"""
import os, sys, time, glob, select, argparse, io
import numpy as np
from PIL import Image, ImageSequence

VID, PID = 0x264a, 0x233c
PANEL = 480
CHUNK_PAYLOAD = 1020          # 1024-byte packet - 4-byte header
BLOCK = 16                    # MCU-aligned block size for the checkerboard

# ----------------------------------------------------------------------------- device
def find_hidraw():
    """interface-1 node: HID_ID matches VID/PID, HID_PHYS ends in 'input1'."""
    for uevent in glob.glob("/sys/class/hidraw/hidraw*/device/uevent"):
        txt = open(uevent).read()
        fields = dict(l.split("=",1) for l in txt.splitlines() if "=" in l)
        hid_id = fields.get("HID_ID","")
        phys   = fields.get("HID_PHYS","")
        # HID_ID looks like "0003:0000264A:0000233C"
        parts = hid_id.split(":")
        if len(parts) == 3:
            try:
                v = int(parts[1],16); p = int(parts[2],16)
            except ValueError:
                continue
            if v == VID and p == PID and phys.endswith("input1"):
                node = "/dev/" + os.path.basename(os.path.dirname(os.path.dirname(uevent)))
                return node
    return None

def chunk_frame(jpeg: bytes, soi_last=True):
    """Split into 1020-byte content chunks, add 4-byte headers, choose wire order.
    Each content chunk keeps the SAME header in both modes:
      content[0]=SOI -> idx=N, flag=0x80 ;  content[k>=1] -> idx=k, flag=0x00.
    soi_last=True  (validated protocol): wire = content[1..N-1] then content[0]
                   -- the flagged SOI chunk is transmitted LAST.
    soi_last=False (--no-soi-last probe): wire = content[0..N-1] in natural order
                   -- the flagged SOI chunk is transmitted FIRST. Byte-for-byte
    identical packets, only the transmission order differs: isolates *when* the
    SOI/top-of-image data reaches the panel relative to its scan-out."""
    content = [jpeg[i:i+CHUNK_PAYLOAD] for i in range(0, len(jpeg), CHUNK_PAYLOAD)]
    content[-1] = content[-1].ljust(CHUNK_PAYLOAD, b"\x00")
    n = len(content)
    def pkt(ci):
        idx, flag = (n, 0x80) if ci == 0 else (ci, 0x00)
        return bytes([0x08, idx & 0xff, 0x00, flag]) + content[ci]
    packets = [pkt(ci) for ci in range(n)]     # in content order
    return packets[1:] + packets[:1] if soi_last else packets

def send_frame(fd, jpeg, chunk_delay_us, soi_last=True):
    for pkt in chunk_frame(jpeg, soi_last):
        os.write(fd, pkt)
        if chunk_delay_us > 0:
            time.sleep(chunk_delay_us / 1e6)

def drain_ack(fd, timeout_s=0.05):
    r,_,_ = select.select([fd], [], [], timeout_s)
    if r:
        try: os.read(fd, 64)
        except OSError: pass

# ----------------------------------------------------------------------------- frames
def prep_frames(path):
    out = []
    for fr in ImageSequence.Iterator(Image.open(path)):
        im = fr.convert("RGBA")
        bg = Image.new("RGBA", im.size, (0,0,0,255)); bg.alpha_composite(im)
        im = bg.convert("RGB")
        w,h = im.size; s = min(w,h)
        im = im.crop(((w-s)//2,(h-s)//2,(w-s)//2+s,(h-s)//2+s)).resize((PANEL,PANEL), Image.LANCZOS)
        out.append(np.asarray(im, dtype=np.uint8))
    return out

def gen_ramp(step, reverse=False):
    """Black -> white progressive raster fill: frame k has k*step pixels white,
    the rest black. Forward (default): the first k*step pixels in row-major order
    (left-to-right, top-to-bottom) -- edge sweeps across each row then down.
    reverse=True: the last k*step pixels instead -- edge sweeps right-to-left,
    bottom-to-top, i.e. against the raster direction and starting in the known
    bottom tear zone. The cleanest probe of raster-order overwrite and any
    horizontal stride/wrap artifact. step<480 gives partial rows (moving
    horizontal edge); step==480 fills exactly one row per frame."""
    P = PANEL * PANEL
    frames = []
    n = 0
    while True:
        flat = np.zeros(P, dtype=np.uint8)
        if n > 0:
            if reverse:
                flat[P - min(n, P):] = 255
            else:
                flat[:min(n, P)] = 255
        img = flat.reshape(PANEL, PANEL)
        frames.append(np.repeat(img[:, :, None], 3, axis=2))
        if n >= P:
            break
        n += step
    return frames

def gen_band(height, stride, reverse=False, start=0, end=None):
    """A single white horizontal band of `height` rows on a black panel, gliding
    down by `stride` rows per frame (reverse=True: up). Every frame's change is
    small and LOCALIZED to wherever the band currently is, and constant in
    magnitude at every position -- so the only variable is the band's vertical
    position. Directly isolates whether tearing tracks *where* on the panel the
    image is changing (the top-region hypothesis) rather than how much.
    start/end bound the band's TOP-edge row range, to zoom into a region at fine
    stride (e.g. start=0 end=96 step=1 walks the top 96 rows one pixel at a time)."""
    if end is None:
        end = PANEL - height
    end = min(end, PANEL - height)
    frames = []
    tops = list(range(start, end + 1, stride))
    if reverse:
        tops = tops[::-1]
    for top in tops:
        img = np.zeros((PANEL, PANEL), dtype=np.uint8)
        img[top:top + height, :] = 255
        frames.append(np.repeat(img[:, :, None], 3, axis=2))
    return frames

def encode(arr, quality=90):
    buf = io.BytesIO()
    Image.fromarray(arr, "RGB").save(buf, "JPEG", quality=quality)
    return buf.getvalue()

def checker_masks():
    """Two complementary 16x16-block checkerboard pixel masks."""
    by, bx = np.mgrid[0:PANEL, 0:PANEL]
    parity = ((bx // BLOCK) + (by // BLOCK)) % 2
    return parity == 0, parity == 1

# ----------------------------------------------------------------------------- build the send sequence per mode
def build_sequence(frames, mode):
    """Return a list of RGB arrays (already composited) to encode+stream in order."""
    if mode == "normal":
        return frames
    if mode == "double":
        seq = []
        for f in frames: seq += [f, f]
        return seq
    if mode == "blend":
        seq = []
        for i in range(len(frames)):
            a = frames[i].astype(np.int16)
            b = frames[(i+1) % len(frames)].astype(np.int16)
            seq.append(frames[i])
            seq.append(((a + b)//2).astype(np.uint8))
        return seq
    if mode == "checker":
        mask_a, mask_b = checker_masks()
        seq = []
        prev = frames[0].copy()
        seq.append(prev.copy())
        for i in range(1, len(frames)+1):
            src  = frames[i % len(frames)]
            mask = mask_a if (i % 2 == 0) else mask_b
            comp = prev.copy()
            comp[mask] = src[mask]          # update only this parity's blocks
            seq.append(comp.copy())
            prev = comp
        return seq
    raise SystemExit(f"unknown mode {mode}")

# ----------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gif", nargs="?", help="GIF path (omit if using --gen)")
    ap.add_argument("--gen", choices=["ramp","band"], help="synthetic source instead of a GIF")
    ap.add_argument("--band-height", type=int, default=96, help="band: white band height in rows")
    ap.add_argument("--band-start", type=int, default=0, help="band: first top-edge row (zoom window)")
    ap.add_argument("--band-end", type=int, default=None, help="band: last top-edge row (zoom window)")
    ap.add_argument("--step", type=int, default=480, help="ramp: white pixels added per frame (raster order). <480 = partial rows")
    ap.add_argument("--reverse", action="store_true", help="ramp: fill bottom-up (right-to-left, against raster) instead of top-down")
    ap.add_argument("--mode", default="normal", choices=["normal","double","blend","checker"])
    ap.add_argument("--fps", type=float, default=20.0)
    ap.add_argument("--chunk-delay-us", type=int, default=1000)
    ap.add_argument("--quality", type=int, default=90)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--loop", action="store_true", help="loop the sequence (default: play once then hold)")
    ap.add_argument("--no-ack", action="store_true", help="don't drain the EP4 ack")
    ap.add_argument("--no-soi-last", action="store_true", help="transmit the SOI/flagged chunk FIRST instead of last (probe)")
    args = ap.parse_args()

    node = find_hidraw()
    if not node:
        sys.exit("panel interface-1 hidraw node not found (is it plugged in? is openrgb holding it? run: pkill -x openrgb)")
    soi_last = not args.no_soi_last
    print(f"device: {node}   mode: {args.mode}   fps: {args.fps}   "
          f"chunk_delay: {args.chunk_delay_us}us   SOI: {'last' if soi_last else 'FIRST (probe)'}")

    if args.gen == "ramp":
        frames = gen_ramp(args.step, args.reverse)
        print(f"generated {len(frames)} ramp frames (step={args.step}px/frame, "
              f"{'bottom-up' if args.reverse else 'top-down'} fill)")
    elif args.gen == "band":
        frames = gen_band(args.band_height, args.step, args.reverse,
                          args.band_start, args.band_end)
        print(f"generated {len(frames)} band frames (h={args.band_height}px, "
              f"stride={args.step}px/frame, top-edge rows {args.band_start}.."
              f"{args.band_end if args.band_end is not None else PANEL-args.band_height}, "
              f"gliding {'up' if args.reverse else 'down'})")
    elif args.gif:
        frames = prep_frames(args.gif)
        print(f"loaded {len(frames)} source frames from {args.gif}")
    else:
        sys.exit("provide a GIF path or --gen ramp")
    seq_arrays = build_sequence(frames, args.mode)
    print(f"encoding {len(seq_arrays)} output frames ({args.mode})...")
    seq = [encode(a, args.quality) for a in seq_arrays]
    avg = sum(len(j) for j in seq)/len(seq)
    print(f"avg frame {avg/1024:.1f} KB, ~{avg/CHUNK_PAYLOAD:.1f} chunks/frame")

    interval = 1.0 / args.fps
    fd = os.open(node, os.O_RDWR)
    t_end = time.time() + args.seconds
    i = 0; sent = 0; t0 = time.time(); held = False
    try:
        while time.time() < t_end:
            cycle = time.time()
            if i < len(seq):
                idx = i
            elif args.loop:
                idx = i % len(seq)
            else:
                idx = len(seq) - 1          # sequence done: hold the last frame
                if not held:
                    print(f"  sequence complete at {sent} frames "
                          f"({sent/(time.time()-t0):.1f} fps avg); holding last frame")
                    held = True
            send_frame(fd, seq[idx], args.chunk_delay_us, soi_last)
            if not args.no_ack:
                drain_ack(fd)
            i += 1; sent += 1
            slack = interval - (time.time() - cycle)
            if slack > 0: time.sleep(slack)
            if sent % 100 == 0 and not held:
                print(f"  {sent} frames, {sent/(time.time()-t0):.1f} fps")
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        os.close(fd)
    print(f"done: {sent} frames in {time.time()-t0:.1f}s")

if __name__ == "__main__":
    main()
