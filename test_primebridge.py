#!/usr/bin/env python3
import importlib.util
import pathlib
import sys

spec = importlib.util.spec_from_file_location('primebridge', pathlib.Path(__file__).with_name('primebridge.py'))
pb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pb)
fails = 0


def check(name, ok):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name)
    fails += 0 if ok else 1


shipped = 'NVK_DIAG=0\n# NVRM_EVENT_WAIT=0\nNVMTL_X=1\n'
mine = f'{shipped}{pb.MINE[0]}\nNVMTL_NO_SURFACE_PAGEOFF=1\n{pb.MINE[1]}\n'
added = pb.conf_mine(shipped, pb.EVENT_WAIT, True)
check('a commented line does not set a knob, and the last line for a key wins',
      pb.conf_value(shipped, 'NVRM_EVENT_WAIT') is None and pb.conf_value('A=0\n  A = 1 \n', 'A') == '1')
check('the event wait goes in a new block of your own settings at the end, which NullMoth keeps on update',
      added == f'{shipped}{pb.MINE[0]}\n{pb.EVENT_WAIT}\n{pb.MINE[1]}\n' and pb.conf_value(added, 'NVRM_EVENT_WAIT') == '1')
check('an existing block of your own settings gets the line before its end marker, once',
      pb.conf_mine(pb.conf_mine(mine, pb.EVENT_WAIT, True), pb.EVENT_WAIT, True)
      == f'{shipped}{pb.MINE[0]}\nNVMTL_NO_SURFACE_PAGEOFF=1\n{pb.EVENT_WAIT}\n{pb.MINE[1]}\n')
check('uninstall removes the line and gives back the file as it was, an emptied block included',
      pb.conf_mine(added, pb.EVENT_WAIT, False) == shipped
      and pb.conf_mine(pb.conf_mine(mine, pb.EVENT_WAIT, True), pb.EVENT_WAIT, False) == mine
      and pb.conf_mine(shipped, pb.EVENT_WAIT, False) == shipped)
sys.exit(1 if fails else 0)
