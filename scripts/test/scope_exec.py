#!/usr/bin/env python3
"""Wait inside the scope until the parent verifies caps; then exec QEMU."""
import os
import resource
from pathlib import Path
import sys
import time

if __name__ == '__main__':
    gate=Path(sys.argv[1]);deadline=time.monotonic()+10
    while not gate.exists():
        if time.monotonic()>=deadline: sys.exit('scope verification gate timed out')
        time.sleep(.02)
    if gate.read_text()!='verified': sys.exit('invalid verification gate')
    limit=int((gate.parent/'file-limit').read_text())
    resource.setrlimit(resource.RLIMIT_FSIZE,(limit,limit))
    os.execvp(sys.argv[2],sys.argv[2:])
