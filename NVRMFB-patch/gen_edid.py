#!/usr/bin/env python3
import sys

BASE = bytes.fromhex(
    '00ffffffffffff003aac010000000000'
    '14200104a52213780eee91a3544c9926'
    '0f505400000001010101010101010101'
    '010101010101d53e8050703825400820'
    '780458c11000001a000000fc00507269'
    '6d654272696467650a20000000fd0030'
    '901ea01e000a20202020202000000010'
    '000a20202020202020202020202000fe')


def timing(w, h, hz):
    vbi = max(int(460 / ((1e6 / hz - 460) / h)) + 1, 15)
    return round(hz * (h + vbi) * (w + 80) / 1e4), vbi


def pick_hz(w, h, panel_hz):
    return next((hz for hz in (panel_hz / k for k in range(1, 13)) if hz <= 75 and timing(w, h, hz)[0] <= 34000), None)


def edid(w, h, hz, mm):
    pclk, vbi = timing(w, h, hz)
    vfp = min(vbi - 14, 63)
    assert w < 4096 and h < 4096 and vbi < 4096 and pclk <= 34000
    e = bytearray(BASE)
    e[21], e[22] = round(mm[0] / 10), round(mm[1] / 10)
    e[54:72] = [pclk & 255, pclk >> 8, w & 255, 80, (w >> 8) << 4, h & 255, vbi & 255, (h >> 8) << 4 | vbi >> 8,
                8, 32, (vfp & 15) << 4 | 8, (vfp >> 4) << 2, mm[0] & 255, mm[1] & 255, (mm[0] >> 8) << 4 | mm[1] >> 8,
                0, 0, 0x1a]
    e[95] = min(e[95], int(hz))
    e[98] = max(e[98], -(-pclk * 10 // (w + 80)))
    e[99] = max(e[99], -(-pclk // 1000))
    e[126] = int(pclk > 16500)
    e[127] = -sum(e[:127]) & 255
    if e[126]:
        c = bytearray(128)
        c[:12] = [2, 3, 12, 0, 0x67, 3, 0x0c, 0, 0x10, 0, 0, 68]
        c[127] = -sum(c[:127]) & 255
        e += c
    return bytes(e)


if __name__ == '__main__':
    w, h = map(int, sys.argv[1].split('x'))
    hz = pick_hz(w, h, float(sys.argv[2]))
    if not hz:
        sys.exit(f'gen_edid: no refresh rate gives a {w}x{h} mode under 340 MHz')
    k = 15.6 * 25.4 / (w * w + h * h) ** 0.5
    b = edid(w, h, hz, (round(w * k), round(h * k)))
    print(f'forced mode {w}x{h} at {hz:g} Hz, pixel clock {timing(w, h, hz)[0] / 100:g} MHz', file=sys.stderr)
    print(',\n'.join(', '.join(f'0x{x:02x}' for x in b[i:i + 16]) for i in range(0, len(b), 16)))
