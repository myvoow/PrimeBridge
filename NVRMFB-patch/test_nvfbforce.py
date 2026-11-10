#!/usr/bin/env python3
import importlib.util
import os
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1])
fb = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
ac = (root / 'kexts/NVRM/accel/nvrm-accel.cpp').read_text()
spec = importlib.util.spec_from_file_location('gen_edid', pathlib.Path(__file__).with_name('gen_edid.py'))
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
fails = 0


def check(name, ok):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name)
    fails += 0 if ok else 1


def section(text, start, end):
    s = text[text.index(start):]
    return s[:s.index(end)]


check('the generator gives back the EDID tested on the Nitro V15 (1920x1080 at 72 Hz)', g.edid(1920, 1080, 72, (344, 193)) == g.BASE)
check('a panel gets an even divisor of its refresh: 72 Hz for 144, 55 for 165, 60 for 60 and 240, 30 for 4K at 60',
      [g.pick_hz(1920, 1080, 144), g.pick_hz(1920, 1080, 165), g.pick_hz(1920, 1080, 60), g.pick_hz(2560, 1440, 240),
       g.pick_hz(3840, 2160, 60)] == [72, 55, 60, 60, 30])
check('a measured 143.78 Hz panel gets its exact half: 71.89 Hz, 160.60 MHz',
      abs(g.pick_hz(1920, 1080, 143.78) - 71.89) < 1e-9 and g.timing(1920, 1080, 71.89)[0] == 16060)
check('the forced EDID is the generated nvfbforce_edid.inc', '#include "nvfbforce_edid.inc"' in fb)
inc = root / 'kexts/NVRMFB/fb/nvfbforce_edid.inc'
edid = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', inc.read_text())) if inc.exists() else b''
check('every block of it has a valid checksum', len(edid) in (128, 256) and all(sum(edid[i:i + 128]) % 256 == 0 for i in range(0, len(edid), 128)))
if edid:
    pclk = edid[54] | edid[55] << 8
    hact, hblank = edid[56] | (edid[58] >> 4) << 8, edid[57] | (edid[58] & 15) << 8
    vact, vblank = edid[59] | (edid[61] >> 4) << 8, edid[60] | (edid[61] & 15) << 8
    hz = pclk * 10000 / ((hact + hblank) * (vact + vblank))
    hdmi = len(edid) == 256 and edid[132:136] == bytes([0x67, 3, 0x0c, 0]) and edid[139] * 500 >= pclk
    check('its timing paces evenly (24 to 75 Hz) and fits HDMI: 165 MHz, or the clock its HDMI block declares',
          24 <= hz < 75.1 and (pclk <= 16500 and len(edid) == 128 or hdmi))
    want = os.environ.get('PANEL')
    if want:
        w, h = map(int, want.split('x'))
        check(f'it is the panel\'s {want}, at the rate picked for {os.environ.get("PANELHZ", 60)} Hz',
              (hact, vact) == (w, h) and abs(hz - g.pick_hz(w, h, float(os.environ.get('PANELHZ', 60)))) < 0.1)
queries = [m.start() for m in re.finditer(r'getDynamicDisplayInfo\(', fb)]
check('every dynamic-display query is preceded by nvrmForce on the same line',
      len(queries) == 3 and all('nvrmForce(dd);' in fb[fb.rindex('\n', 0, q) - 200:q] for q in queries))
check('the force pass needs the boot-arg', 'PE_parse_boot_argn("nvfbforce", &force' in fb and 'pass < (force ? 2 : 1)' in fb)
check('only an HDMI connector is forced (a DP link cannot train without a sink)', '!= 0x800) continue;' in fb)
check('a force that no connector took is dropped, after its reason is kept',
      fb.index('const char *why = !force ? "off" : !gNDpy ? (gForcedDpy ? "refused" : "no-hdmi")') < fb.index('if (!gNDpy) gForcedDpy = 0;'))
pub = 'p->setProperty("nvfbforce-display", (unsigned long long)gForcedDpy, 32); p->setProperty("nvfbforce", why);'
check('NVRM publishes the forced display (0 = none) and why, after the drop',
      pub in fb and fb.index('if (!gNDpy) gForcedDpy = 0;') < fb.index(pub))
scan = section(fb, 'isEqualTo(NVRM_VRAM_FN_SCANOUT)', 'return kIOReturnSuccess;')
check('nvScanoutInfo reports the scanout allocation (fMemBytes) in mappedTotal', 'r->mappedTotal = fMemBytes;' in scan)
check('nvScanoutInfo flags a redirected scanout with the physical address of its target',
      'if (fRedirPhys) { r->flags |= NVRM_SCANOUT_FLAG_SYSPHYS; r->size = fRedirPhys; }' in scan)
red = section(fb, 'isEqualTo(NVRM_VRAM_FN_REDIRECT)', 'return kIOReturnSuccess;')
check("nvScanoutRedirect keeps NVRMFB's own buffer and puts it back for a NULL target",
      all(t in red for t in ('if (!fKvaOwn) fKvaOwn = fKva;', 'fKva = param1 ? param1 : fKvaOwn;', 'if (!param1) fKvaOwn = nullptr;')))
pt = section(ac, 'IOReturn NVDisplayPipe::performTransaction', '\n}\n')
check('a frame goes to the panel by the copy engine after its GPU work is done, before the CPU copy path',
      pt.index('nvSurfWaitReady(s->getSurfaceID());') < pt.index('if (nvAccelIopToPanel(a, fb, head, s)) return kIOReturnSuccess;')
      < pt.index('if (!nvAccelSurfCopyReady(s, head))'))
ce = section(ac, 'static bool nvAccelIopToPanel(NVAccel *a, IOService *fb, unsigned head, IOSurface *s)\n{', '\n}\n')
check('the copy engine runs only for a redirected scanout, a same-geometry surface and VRAM pixels newer than system memory',
      all(t in ce for t in ('NVRM_SCANOUT_FLAG_SYSPHYS', 's->getBytesPerRow() != q.pitch', '!nvSVPend(id, 2)', '!(valid[0] & 1u)',
                            'nvAccelResPluginWrote(res)', 'vm->fBytes >= len')))
check('a frame the copy engine did not take is never copied from stale system memory: the panel keeps its picture',
      'return !nvAccelSurfCopyReady(s, head) || nvSVPend(id, 2);' in ce and ce.count('return keep(') == 3)
check('a copy engine frame is counted like a CPU one', 'OSAddAtomic64(1, &gCntIopOk)' in ce)
sys.exit(1 if fails else 0)
