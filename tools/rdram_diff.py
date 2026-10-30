"""Diffs two RDRAM dumps (netplay probe, PSR_STATE_DUMP_AT) and attributes each
differing range to the object file that owns it, per pret's linker map.

Usage: python -I tools/rdram_diff.py <a.bin> <b.bin> <pokestadium-us.map>
"""
import re
import sys
from collections import defaultdict

BASE = 0x80000000
SECTION = re.compile(r"^ \.(\w+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+)")


def load_sections(map_path):
    sections = []
    with open(map_path, errors="replace") as f:
        for line in f:
            m = SECTION.match(line)
            if not m:
                continue
            start, size = int(m.group(2), 16), int(m.group(3), 16)
            if size and 0x80000000 <= start < 0x80800000:
                sections.append((start, start + size, f"{m.group(4)} (.{m.group(1)})"))
    sections.sort()
    return sections


def owner(sections, addr):
    lo, hi = 0, len(sections)
    while lo < hi:
        mid = (lo + hi) // 2
        if sections[mid][1] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo < len(sections) and sections[lo][0] <= addr < sections[lo][1]:
        return sections[lo][2]
    return "(heap / unmapped)"


def main(a_path, b_path, map_path):
    a = open(a_path, "rb").read()
    b = open(b_path, "rb").read()
    sections = load_sections(map_path)

    # Group differing bytes into ranges, merging gaps under 16 bytes.
    ranges = []
    start = prev = None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if start is None:
                start = prev = i
            elif i - prev > 16:
                ranges.append((start, prev + 1))
                start = i
            prev = i
    if start is not None:
        ranges.append((start, prev + 1))

    total = sum(e - s for s, e in ranges)
    print(f"{total} differing bytes in {len(ranges)} ranges")
    by_owner = defaultdict(lambda: [0, 0, []])
    for s, e in ranges:
        o = owner(sections, BASE + s)
        by_owner[o][0] += e - s
        by_owner[o][1] += 1
        by_owner[o][2].append(BASE + s)
    for o, (n, count, addrs) in sorted(by_owner.items(), key=lambda kv: -kv[1][0]):
        sample = ", ".join(f"0x{x:08X}" for x in addrs[:3])
        print(f"{n:9d} bytes {count:5d} ranges  {o}  [{sample}]")


if __name__ == "__main__":
    main(*sys.argv[1:4])
