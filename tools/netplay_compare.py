"""Compares netplay-probe state hash logs (PSR_STATE_HASH) from several runs.

Each line: <poll> <whole-RDRAM hash> <128 per-64KiB-block hashes>. Reports how
many polls match across all runs, the first poll where they differ, and which
RDRAM blocks differ there (and how often each block differs overall).
Usage: python -I tools/netplay_compare.py <hash.txt> <hash.txt> [...]
"""
import sys
from collections import Counter

BLOCK = 64 * 1024


def load(path):
    rows = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            rows[int(parts[0])] = (parts[1], parts[2:])
    return rows


def main(paths):
    runs = [load(p) for p in paths]
    for p, r in zip(paths, runs):
        print(f"{p}: {len(r)} polls")
    common = sorted(set.intersection(*(set(r) for r in runs)))
    if not common:
        print("no common polls")
        return

    first = None
    differing_polls = 0
    block_counts = Counter()
    for poll in common:
        wholes = {r[poll][0] for r in runs}
        if len(wholes) == 1:
            continue
        differing_polls += 1
        blocks = list(zip(*(r[poll][1] for r in runs)))
        diff = [i for i, values in enumerate(blocks) if len(set(values)) > 1]
        block_counts.update(diff)
        if first is None:
            first = (poll, diff)

    print(f"compared {len(common)} polls; {len(common) - differing_polls} identical, "
          f"{differing_polls} differ")
    if first is None:
        print("all runs identical at every common poll")
        return
    poll, diff = first
    print(f"first difference at poll {poll}, in {len(diff)} block(s):")
    for i in diff[:16]:
        print(f"  0x{0x80000000 + i * BLOCK:08X}..0x{0x80000000 + (i + 1) * BLOCK - 1:08X}")
    print("blocks that differ most often:")
    for i, n in block_counts.most_common(12):
        print(f"  0x{0x80000000 + i * BLOCK:08X}: {n} polls")


if __name__ == "__main__":
    main(sys.argv[1:])
