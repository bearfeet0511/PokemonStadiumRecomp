"""Prints the parts of a Gen 1 (Red/Blue) .sav that Pokemon Stadium checks.

Offsets follow pret/pokered's SRAM layout. Usage:
    python -I tools/gen1_sav_info.py <save.sav>
"""
import sys

MAIN_START, MAIN_SUM = 0x2598, 0x3523
PLAYER_NAME, PLAYER_ID = 0x2598, 0x2605
DEX_OWNED = 0x25A3
CUR_MAP = 0x260A
CUR_BOX_NUM = 0x284C
PARTY = 0x2F2C
CUR_BOX = 0x30C0
BANK_SIZE, BOX_SIZE = 0x2000, 0x462
ALL_BOXES_SUM = 6 * BOX_SIZE

CHARS = {0x50: "", 0x7F: " "}
CHARS.update({0x80 + i: chr(ord("A") + i) for i in range(26)})
CHARS.update({0xA0 + i: chr(ord("a") + i) for i in range(26)})
CHARS.update({0xF6 + i: str(i) for i in range(10)})


def checksum(data):
    return (~sum(data)) & 0xFF


def text(raw):
    out = []
    for b in raw:
        if b == 0x50:
            break
        out.append(CHARS.get(b, "?"))
    return "".join(out)


def main(path):
    sav = open(path, "rb").read()
    assert len(sav) == 0x8000, f"unexpected size {len(sav)}"
    ok = sav[MAIN_SUM] == checksum(sav[MAIN_START:MAIN_SUM])
    print(f"main checksum: {'ok' if ok else 'BAD'} (0x{sav[MAIN_SUM]:02X})")
    print(f"player: {text(sav[PLAYER_NAME:PLAYER_NAME + 11])} "
          f"id {int.from_bytes(sav[PLAYER_ID:PLAYER_ID + 2], 'big')}")
    print(f"map: 0x{sav[CUR_MAP]:02X}  current box: {sav[CUR_BOX_NUM] & 0x7F}"
          f" (boxes initialized: {bool(sav[CUR_BOX_NUM] & 0x80)})")
    owned = sum(bin(b).count("1") for b in sav[DEX_OWNED:DEX_OWNED + 19])
    print(f"pokedex owned: {owned}")

    count = sav[PARTY]
    nicks = PARTY + 8 + 6 * 44 + 6 * 11
    print(f"party ({count}):")
    for i in range(count):
        mon = sav[PARTY + 8 + 44 * i:PARTY + 8 + 44 * (i + 1)]
        nick = text(sav[nicks + 11 * i:nicks + 11 * (i + 1)])
        hp = int.from_bytes(mon[1:3], "big")
        print(f"  species 0x{mon[0]:02X} L{mon[33]} hp {hp} '{nick}'")
    print(f"current box count: {sav[CUR_BOX]}")

    for bank in (2, 3):
        base = bank * BANK_SIZE
        ok = sav[base + ALL_BOXES_SUM] == checksum(sav[base:base + ALL_BOXES_SUM])
        print(f"bank {bank} box checksum: {'ok' if ok else 'BAD'}")


if __name__ == "__main__":
    main(sys.argv[1])
