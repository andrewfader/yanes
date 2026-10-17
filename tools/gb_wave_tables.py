#!/usr/bin/env python3
"""List the CH3 wave-RAM tables a Game Boy game actually played.

Reads an APU register log (tools/yanes_gb_wave_harvest.c or yanes-gb-oracle;
columns: sample index at 48 kHz, IO register low byte (FF1A is 1a), value) and tracks
wave RAM (FF30-FF3F) and the CH3 controls. Every stretch where CH3 sounds (APU
on, DAC on, NR32 volume not muted, triggered) is credited to the table that was
in wave RAM, so the report ranks tables by how long they were heard. A driver
that streams samples through wave RAM shows up as many tables heard for a few
milliseconds each; --min-seconds drops those.

usage: gb_wave_tables.py registers.log [--min-seconds 0.5] [--json]
"""
import argparse
import json
import sys

RATE = 48000
NR30, NR32, NR34, NR52 = 0x1A, 0x1C, 0x1E, 0x26
WAVE_START, WAVE_END = 0x30, 0x3F


def samples(table):
    """The 32 four-bit samples, high nibble first, as the hardware plays them."""
    out = []
    for byte in table:
        out += [byte >> 4, byte & 0x0F]
    return out


def sketch(levels):
    return "".join(" .:-=+*#%@ABCDEF"[v] for v in levels)


def harvest(path):
    wave = [0] * 16
    dac = power = False
    volume = 0
    playing = False
    since = 0
    heard = {}

    def credit(now):
        if playing and power and dac and volume:
            key = bytes(wave)
            entry = heard.setdefault(key, {"seconds": 0.0, "triggers": 0, "volumes": {}})
            entry["seconds"] += (now - since) / RATE
            entry["volumes"][volume] = entry["volumes"].get(volume, 0.0) + (now - since) / RATE

    with open(path) as log:
        for line in log:
            if line.startswith("#"):
                continue
            t, reg, value = line.split()
            t, reg, value = int(t), int(reg, 16), int(value, 16)
            touches_ch3 = reg in (NR30, NR32, NR34, NR52) or WAVE_START <= reg <= WAVE_END
            if not touches_ch3:
                continue
            credit(t)
            since = t
            if WAVE_START <= reg <= WAVE_END:
                wave[reg - WAVE_START] = value
            elif reg == NR30:
                dac = bool(value & 0x80)
                if not dac:
                    playing = False
            elif reg == NR32:
                volume = (value >> 5) & 3
            elif reg == NR52:
                power = bool(value & 0x80)
            elif reg == NR34 and value & 0x80:
                playing = True
                if dac:
                    heard.setdefault(bytes(wave), {"seconds": 0.0, "triggers": 0, "volumes": {}})["triggers"] += 1
    return heard


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log")
    parser.add_argument("--min-seconds", type=float, default=0.5)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    heard = harvest(args.log)
    rows = sorted(((k, v) for k, v in heard.items() if v["seconds"] >= args.min_seconds),
                  key=lambda kv: -kv[1]["seconds"])
    if args.json:
        json.dump([{"table": k.hex(), "samples": samples(k), "seconds": round(v["seconds"], 3),
                    "triggers": v["triggers"],
                    "volume_codes": {str(c): round(s, 3) for c, s in sorted(v["volumes"].items())}}
                   for k, v in rows], sys.stdout, indent=1)
        print()
        return
    print(f"{len(rows)} tables heard for {args.min_seconds}s or more ({len(heard)} distinct in all)")
    for k, v in rows:
        vols = " ".join(f"v{c}:{s:.0f}s" for c, s in sorted(v["volumes"].items()))
        print(f"{v['seconds']:8.1f}s {v['triggers']:6d}x  {k.hex()}  |{sketch(samples(k))}|  {vols}")


if __name__ == "__main__":
    main()
