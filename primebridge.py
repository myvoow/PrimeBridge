#!/usr/bin/env python3
import argparse
import importlib.util
import json
import os
import platform
import plistlib
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = '/Library/PrimeBridge'
CACHE = os.path.expanduser('~/Library/Caches/PrimeBridge')
EXT = '/Library/Extensions'
NV = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
OC = '4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102'
ARGS = ['nvfb=1', 'nvaccel=1', 'nvfbforce=1', 'nvcursor=0', '-nvrmnoflip']
MARK = {'NVRMFB': b'nvfbforce', 'NVAccel': b'panel by the copy engine'}
AGENT = os.path.expanduser('~/Library/LaunchAgents/com.myvo.primebridge.guard.plist')
CONF = '/Library/GPUBundles/nvmtl/nvrm610.conf'
MINE = ('# --- my settings, kept on update ---', '# --- end my settings ---')
EVENT_WAIT = 'NVRM_EVENT_WAIT=1'
LOG = '/var/log/PrimeBridge.log'
WHY = {'no-hdmi': 'this NVIDIA GPU has no HDMI port to force: PrimeBridge cannot use it to light the panel',
       'refused': 'NVKMS refused the forced HDMI display (kernel log: NVRM-fb lines)',
       'off': 'boot-arg nvfbforce=1 is missing: run ./install.sh again'}


def out(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def sudo(*cmd):
    subprocess.run(('sudo',) + cmd, check=True)


def die(msg):
    sys.exit('PrimeBridge: ' + msg)


def read(path):
    try:
        with open(path, 'rb') as f:
            return f.read()
    except OSError:
        return b''


def version(kext):
    p = read(kext + '/Contents/Info.plist')
    return plistlib.loads(p).get('CFBundleVersion') if p else None


def patched(name):
    return MARK[name] in read(f'{EXT}/{name}.kext/Contents/MacOS/{name}')


def ioreg(cls):
    data = subprocess.run(['ioreg', '-a', '-r', '-c', cls, '-d', '1'], capture_output=True).stdout
    return plistlib.loads(data) if data.strip() else []


def gen_edid():
    for p in (f'{HERE}/NVRMFB-patch/gen_edid.py', f'{HERE}/gen_edid.py'):
        if os.path.exists(p):
            spec = importlib.util.spec_from_file_location('gen_edid', p)
            m = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(m)
            return m


def panel_hz(pb):
    logged = re.findall(rb'panel ([1-9][0-9]*\.[0-9]+) Hz', read(LOG))[-50:]
    return statistics.median(map(float, logged)) if logged else pb[0].get('PanelHz', 0) if pb else 0


def conf_value(text, key):
    v = None
    for line in text.splitlines():
        k, eq, val = line.strip().partition('=')
        if eq and k.strip() == key:
            v = val.strip()
    return v


def conf_mine(text, line, add):
    lines = text.splitlines()
    s = lines.index(MINE[0]) if MINE[0] in lines else -1
    e = lines.index(MINE[1], s) if s >= 0 and MINE[1] in lines[s:] else -1
    if e < 0:
        if not add:
            return text
        lines += list(MINE)
        s, e = len(lines) - 2, len(lines) - 1
    block = [x for x in lines[s + 1:e] if x != line] + ([line] if add else [])
    lines[s:e + 1] = [MINE[0]] + block + [MINE[1]] if block else []
    return '\n'.join(lines) + '\n'


def conf_text():
    return read(CONF).decode('utf-8', 'surrogateescape')


def write_conf(text):
    sudo('cp', CONF, f'{LIB}/nvrm610.conf.{time.strftime("%Y%m%d-%H%M%S")}')
    subprocess.run(['sudo', 'tee', CONF], input=text.encode('utf-8', 'surrogateescape'), check=True, stdout=subprocess.DEVNULL)


def le(b):
    return int.from_bytes(b[:4], 'little')


def display_pci():
    return [le(e['device-id']) << 16 | le(e['vendor-id']) for e in ioreg('IOPCIDevice')
            if le(e.get('class-code', b'\0\0\0\0')) >> 16 == 3 and 'vendor-id' in e and 'device-id' in e]


def detect(a):
    d = {}
    if platform.machine() != 'x86_64':
        die('this Mac is not an Intel/AMD (x86_64) machine')
    d['macos'] = int(platform.mac_ver()[0].split('.')[0])
    if d['macos'] < 15:
        die('NullMoth 2 runs on macOS 15 and 26')
    sdk = out('xcrun', '--show-sdk-path').strip()
    if not (shutil.which('git') and sdk and os.path.isdir(sdk + '/System/Library/Frameworks/Kernel.framework/Headers')):
        die('the Xcode Command Line Tools are missing: run  xcode-select --install  and try again')
    d['nullmoth'] = version(f'{EXT}/NVRM.kext')
    if not d['nullmoth'] or not re.fullmatch(r'\d+(\.\d+)+', d['nullmoth']):
        die('NullMoth is not installed (no /Library/Extensions/NVRM.kext)')
    if not ioreg('NVRM'):
        die("NullMoth's NVRM is not running: get NullMoth working first (a monitor on the NVIDIA GPU shows an accelerated desktop)")
    pci = display_pci()
    if not any(x & 0xffff == 0x10de for x in pci):
        die('no NVIDIA GPU found')
    igpu = [x for x in pci if x & 0xffff in (0x1002, 0x8086)]
    if not igpu:
        die('no AMD or Intel integrated GPU found: PrimeBridge is for laptops whose built-in panel is wired to the iGPU')
    ok = {int(x, 16) for x in plistlib.loads(read(f'{HERE}/PrimeBridge-source/Info.plist') or read(f'{LIB}/PrimeBridge.kext/Contents/Info.plist'))
          ['IOKitPersonalities']['PrimeBridge']['IOPCIPrimaryMatch'].split()}
    d['igpu'] = next((x for x in igpu if x in ok), None)
    if not d['igpu']:
        die('iGPU ' + ', '.join(f'{x & 0xffff:04x}:{x >> 16:04x}' for x in igpu) + ' is not supported yet')
    if 'nootedred' in out('kmutil', 'showloaded').lower():
        die('NootedRed is loaded and drives this iGPU, which PrimeBridge cannot share: disable NootedRed in OpenCore, restart, then run install.sh again')
    pb = ioreg('PrimeBridge')
    w = h = 0
    if a.panel:
        w, h = map(int, a.panel.lower().split('x'))
    elif pb:
        w, h = pb[0].get('GOPWidth', 0), pb[0].get('GOPHeight', 0)
    else:
        for gpu in json.loads(out('system_profiler', 'SPDisplaysDataType', '-json') or '{}').get('SPDisplaysDataType', []):
            if 'nvidia' not in gpu.get('spdisplays_vendor', '').lower():
                for disp in gpu.get('spdisplays_ndrvs', []):
                    m = re.match(r'(\d+) x (\d+)', disp.get('_spdisplays_pixels') or disp.get('_spdisplays_resolution', ''))
                    if m:
                        w, h = map(int, m.groups())
    if not w or not h:
        die('cannot find the panel resolution: pass it, e.g.  ./install.sh --panel 1920x1080')
    d['panel'] = f'{w}x{h}'
    d['panelhz'] = a.panel_hz or panel_hz(pb) or 60
    d['hz'] = gen_edid().pick_hz(w, h, d['panelhz'])
    if not d['hz']:
        die(f'no forced mode for a {w}x{h} panel fits HDMI')
    d['efi'] = find_efi(a)
    print(f"macOS {d['macos']}, NullMoth {d['nullmoth']}, iGPU {d['igpu'] & 0xffff:04x}:{d['igpu'] >> 16:04x}, "
          f"panel {d['panel']} at {d['panelhz']:g} Hz -> forced HDMI mode at {d['hz']:g} Hz, OpenCore on {d['efi']}")
    return d


def find_efi(a):
    m = re.search(r'HD\(\d+,GPT,([0-9A-Fa-f-]{36})', out('nvram', OC + ':boot-path'))
    r = subprocess.run(['diskutil', 'info', '-plist', m.group(1)], capture_output=True) if m and not a.efi else None
    dev = a.efi or (plistlib.loads(r.stdout).get('DeviceIdentifier') if r and r.returncode == 0 else None)
    if not dev:
        die("cannot find OpenCore's EFI partition: pass it, e.g.  --efi disk0s1  (see diskutil list)")
    return dev


def mount(dev):
    mnt = plistlib.loads(subprocess.run(['diskutil', 'info', '-plist', dev], capture_output=True).stdout).get('MountPoint')
    if mnt:
        return mnt, False
    sudo('diskutil', 'mount', dev)
    return plistlib.loads(subprocess.run(['diskutil', 'info', '-plist', dev], capture_output=True).stdout)['MountPoint'], True


def edit_config(dev, change):
    mnt, mounted = mount(dev)
    cfg = f'{mnt}/EFI/OC/config.plist'
    p = plistlib.loads(subprocess.run(['sudo', 'cat', cfg], capture_output=True, check=True).stdout)
    if p.get('Misc', {}).get('Security', {}).get('Vault', 'Optional') != 'Optional':
        die('OpenCore Vault is on: a changed config.plist would not boot. Turn Vault to Optional, or add PrimeBridge by hand')
    change(p, mnt)
    with tempfile.NamedTemporaryFile(suffix='.plist', delete=False) as f:
        plistlib.dump(p, f, sort_keys=False)
    subprocess.run(['plutil', '-lint', '-s', f.name], check=True)
    sudo('cp', cfg, f'{LIB}/config.plist.{time.strftime("%Y%m%d-%H%M%S")}')
    sudo('cp', f.name, cfg)
    os.unlink(f.name)
    if mounted:
        sudo('diskutil', 'unmount', dev)


def install(a):
    d = detect(a)
    if a.dry_run:
        return
    v, work = d['nullmoth'], f"{CACHE}/nullmoth-{d['nullmoth']}"
    os.makedirs(CACHE, exist_ok=True)
    stock = []
    for n in MARK:
        s = f'{EXT}/{n}.kext/Contents/MacOS/{n}' if not patched(n) else f'{LIB}/{n}-{v}-orig.kext/Contents/MacOS/{n}'
        stock.append(s if os.path.exists(s) else '')
    env = dict(os.environ, WORK=work, PANEL=d['panel'], PANELHZ=f"{d['panelhz']:g}", MACOS_MAJOR=str(d['macos']))
    subprocess.run(['sh', f'{HERE}/NVRMFB-patch/build-nvrmfb.sh', v] + stock, env=env, check=True)
    r = subprocess.run(['sh', f'{HERE}/PrimeBridge-source/build.sh'], capture_output=True, text=True)
    if r.returncode:
        die('PrimeBridge did not build:\n' + r.stdout + r.stderr)
    print('installing: sudo asks for your password')
    sudo('mkdir', '-p', LIB)
    for src in (f'{work}/patched/NVRMFB.kext', f'{work}/patched/NVAccel.kext', f'{HERE}/PrimeBridge-source/build/PrimeBridge.kext'):
        sudo('rm', '-rf', f'{LIB}/{os.path.basename(src)}')
        sudo('ditto', src, f'{LIB}/{os.path.basename(src)}')
    for src in (f'{HERE}/install-nvrmfb.sh', f'{HERE}/primebridge.py', f'{HERE}/NVRMFB-patch/gen_edid.py'):
        sudo('cp', src, LIB)
    sudo('chown', '-R', 'root:wheel', LIB)
    added, pats = [], []

    def add(p, mnt):
        kexts = p['Kernel']['Add']
        e = next((x for x in kexts if x.get('BundlePath') == 'PrimeBridge.kext'), None)
        if e:
            e['Enabled'] = True
        else:
            kexts.append({'Arch': 'x86_64', 'BundlePath': 'PrimeBridge.kext', 'Comment': 'PrimeBridge', 'Enabled': True,
                          'ExecutablePath': 'Contents/MacOS/PrimeBridge', 'MaxKernel': '', 'MinKernel': '19.0.0',
                          'PlistPath': 'Contents/Info.plist'})
        weg = any(x.get('Enabled') and x.get('BundlePath') == 'WhateverGreen.kext' for x in kexts)
        nv = p.setdefault('NVRAM', {}).setdefault('Add', {}).setdefault(NV, {})
        have = nv.get('boot-args', '').split()
        want = ARGS + (['gfxrst=4'] if weg else [])
        keys = {x.split('=')[0] for x in want}
        added[:] = [x for x in want if x not in have]
        nv['boot-args'] = ' '.join([x for x in have if x.split('=')[0] not in keys and x != '-primeoff'] + want)
        pats[:] = [x for x in p['Kernel'].get('Patch', []) if x.get('Enabled') and 'fix pat' in x.get('Comment', '').lower()]
        for x in pats:
            x['Enabled'] = False
            print(f"disabled the OpenCore patch {x['Comment']!r}: it makes writes to the iGPU about 20 times slower")
        delete = p['NVRAM'].setdefault('Delete', {}).setdefault(NV, [])
        if 'boot-args' not in delete:
            delete.append('boot-args')
        sudo('rm', '-rf', f'{mnt}/EFI/OC/Kexts/PrimeBridge.kext')
        sudo('cp', '-R', f'{LIB}/PrimeBridge.kext', f'{mnt}/EFI/OC/Kexts/')

    edit_config(d['efi'], add)
    sudo('sh', f'{LIB}/install-nvrmfb.sh')
    old = read(f'{LIB}/added-boot-args').decode().split()
    off = read(f'{LIB}/disabled-patches').decode().splitlines() + [x['Comment'] for x in pats]
    state = {'nullmoth-version': v, 'forced-mode': f"{d['panel']}@{d['hz']:g}", 'added-boot-args': ' '.join(sorted(set(old + added))),
             'disabled-patches': '\n'.join(dict.fromkeys(off))}
    for k, val in state.items():
        subprocess.run(['sudo', 'tee', f'{LIB}/{k}'], input=val + '\n', text=True, check=True, stdout=subprocess.DEVNULL)
    conf = conf_text()
    if conf and conf_value(conf, 'NVRM_EVENT_WAIT') is None:
        write_conf(conf_mine(conf, EVENT_WAIT, True))
        subprocess.run(['sudo', 'tee', f'{LIB}/event-wait'], input='1\n', text=True, check=True, stdout=subprocess.DEVNULL)
        print(f'added {EVENT_WAIT} to your settings in {CONF}: Metal apps wait for the GPU without spinning the CPU')
    os.makedirs(os.path.dirname(AGENT), exist_ok=True)
    with open(AGENT, 'wb') as f:
        plistlib.dump({'Label': 'com.myvo.primebridge.guard', 'RunAtLoad': True,
                       'ProgramArguments': ['/usr/bin/python3', '-I', f'{LIB}/primebridge.py', 'guard']}, f)
    print('done: approve the extensions in System Settings > Privacy & Security if asked, restart, then run ./doctor.sh')


def doctor(a):
    pb, nvrm = ioreg('PrimeBridge'), ioreg('NVRM')
    v, built = version(f'{EXT}/NVRM.kext'), read(f'{LIB}/nullmoth-version').decode().strip()
    args = out('nvram', 'boot-args').split()
    if a.report:
        p = pb[0] if pb else {}
        print('```text')
        print(f"macOS {out('sw_vers', '-productVersion').strip()} ({out('sw_vers', '-buildVersion').strip()}), "
              f"{out('sysctl', '-n', 'hw.model').strip()}, {out('sysctl', '-n', 'machdep.cpu.brand_string').strip()}")
        print('display PCI IDs ' + ' '.join(f'{x & 0xffff:04x}:{x >> 16:04x}' for x in display_pci())
              + f", OpenCore {out('nvram', OC + ':opencore-version').partition(chr(9))[2].strip() or '?'}")
        print('boot-args ' + ' '.join(args[1:]))
        print(f"built for NullMoth {built or '-'}, forced mode {read(f'{LIB}/forced-mode').decode().strip() or '-'}, "
              f"panel {p.get('GOPWidth', '?')}x{p.get('GOPHeight', '?')} at {p.get('PanelHz', '?')} Hz, "
              f"effective rate {p.get('EffectiveRateHz', '?')} Hz")
    bad = []
    if not nvrm:
        bad.append("NullMoth's NVRM is not running: fix NullMoth first")
    if not built:
        bad.append('PrimeBridge was never installed with install.sh')
    elif v != built:
        bad.append(f'NullMoth is now {v}, PrimeBridge was built for {built}: run ./install.sh again')
    bad += [f'{n} is not the PrimeBridge build (a NullMoth update replaced it?): run ./install.sh again' for n in MARK if not patched(n)]
    miss = [x for x in ARGS if x not in args]
    if miss:
        bad.append(f"boot-args lack {' '.join(miss)} (OpenCore's config.plist changed?): run ./install.sh again")
    why = nvrm[0].get('nvfbforce') if nvrm else None
    pat = re.findall(rb'igpu writes: PAT 0x[0-9a-f]+ \(PA6 ([^)]+)\)', read(LOG))
    if pat and pat[-1] != b'WC':
        bad.append("OpenCore's Fix PAT patch is on (iGPU writes are slow, no direct mode): run ./install.sh again")
    if why in WHY:
        bad.append(WHY[why])
    if not pb:
        bad.append('PrimeBridge is not loaded: its OpenCore entry is missing or disabled, or -primeoff is set: run ./install.sh again')
    else:
        p = pb[0]
        mode = p.get('PrimeMode', '?')
        ew = conf_value(conf_text(), 'NVRM_EVENT_WAIT')
        print(f"PrimeBridge {p.get('PrimeBridgeVersion')}, mode {mode}, NullMoth {v}, forced {why or '?'}, "
              f"event wait {'on' if ew == '1' else 'off'}")
        if mode == 'direct-vram-fallback':
            bad.append('NVRMFB has no nvScanoutRedirect (an older patch or a stock NVRMFB): run ./install.sh again')
        elif mode.startswith('staging'):
            print('  staging copy: normal for a few seconds after boot, then the direct mode starts (still here later: see /var/log/PrimeBridge.log)')
        elif mode.startswith('no-'):
            bad.append(f'{mode}: see /var/log/PrimeBridge.log')
        hz = panel_hz(pb)
        forced = read(f'{LIB}/forced-mode').decode().strip().partition('@')
        if hz and p.get('GOPWidth') and forced[2]:
            best = gen_edid().pick_hz(p['GOPWidth'], p['GOPHeight'], hz)
            if best and (forced[0] != f"{p['GOPWidth']}x{p['GOPHeight']}" or abs(float(forced[2]) - best) > 0.02):
                print(f"  the panel runs at {hz:g} Hz: run ./install.sh again to force {best:g} Hz instead of {''.join(forced)}")
    print('\n'.join('problem: ' + x for x in bad) if bad else 'all good')
    if a.report:
        print(f'--- last 60 lines of {LOG}')
        print('\n'.join(read(LOG).decode(errors='replace').splitlines()[-60:]))
        print('```')


def uninstall(a):
    dev = find_efi(a)
    v = version(f'{EXT}/NVRM.kext')
    origs = [f'{LIB}/{n}-{v}-orig.kext' for n in MARK if os.path.isdir(f'{LIB}/{n}-{v}-orig.kext')]
    if origs:
        sudo('sh', f'{LIB}/install-nvrmfb.sh', *origs)
    for n in MARK:
        if not os.path.isdir(f'{LIB}/{n}-{v}-orig.kext'):
            print(f'no original {n} {v} kept: reinstall NullMoth {v} to get it back')
    added = read(f'{LIB}/added-boot-args').decode().split()
    off = read(f'{LIB}/disabled-patches').decode().splitlines()

    def remove(p, mnt):
        p['Kernel']['Add'] = [x for x in p['Kernel']['Add'] if x.get('BundlePath') != 'PrimeBridge.kext']
        for x in p['Kernel'].get('Patch', []):
            if x.get('Comment') in off:
                x['Enabled'] = True
        nv = p.get('NVRAM', {}).get('Add', {}).get(NV, {})
        nv['boot-args'] = ' '.join(x for x in nv.get('boot-args', '').split() if x not in added)
        sudo('rm', '-rf', f'{mnt}/EFI/OC/Kexts/PrimeBridge.kext')

    edit_config(dev, remove)
    conf = conf_text()
    if read(f'{LIB}/event-wait').strip() == b'1' and conf and conf_mine(conf, EVENT_WAIT, False) != conf:
        write_conf(conf_mine(conf, EVENT_WAIT, False))
    sudo('rm', '-f', f'{LIB}/added-boot-args', f'{LIB}/nullmoth-version', f'{LIB}/forced-mode', f'{LIB}/disabled-patches',
         f'{LIB}/event-wait')
    if os.path.exists(AGENT):
        os.remove(AGENT)
    print(f'removed: restart to finish. Backups stay in {LIB}')


def guard(a):
    built = read(f'{LIB}/nullmoth-version').decode().strip()
    if built and (version(f'{EXT}/NVRM.kext') != built or not all(patched(n) for n in MARK)):
        subprocess.run(['osascript', '-e', 'display notification "NullMoth changed since PrimeBridge was installed: '
                        'run install.sh again, the panel is not accelerated until then" with title "PrimeBridge"'])


if __name__ == '__main__':
    ap = argparse.ArgumentParser(prog='primebridge')
    ap.add_argument('command', choices=['install', 'doctor', 'uninstall', 'guard'])
    ap.add_argument('--dry-run', action='store_true', help='detect only, change nothing')
    ap.add_argument('--report', action='store_true', help='doctor: also print the system, settings and log tail, ready to paste in an issue')
    ap.add_argument('--panel', help='panel resolution, e.g. 1920x1080 (detected otherwise)')
    ap.add_argument('--panel-hz', type=float, help='panel refresh rate (measured by PrimeBridge otherwise, 60 before)')
    ap.add_argument('--efi', help="OpenCore's EFI partition, e.g. disk0s1 (found from OpenCore's boot-path otherwise)")
    a = ap.parse_args()
    try:
        globals()[a.command](a)
    except subprocess.CalledProcessError as e:
        die('stopped, this failed: ' + ' '.join(map(str, e.cmd)))
