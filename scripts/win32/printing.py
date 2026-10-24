"""G31 winspool forwards to the original GDI shared-queue producer."""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parents[2]
NAMES = re.findall(r'\bW32API\s+(\w+)\(', (ROOT/'sdk/win32/include/w32/printing.h').read_text())
FORWARDERS = {name: 'gdi32.'+name for name in NAMES if name not in (
    'StartDocA', 'StartDocW', 'StartPage', 'EndPage', 'EndDoc', 'AbortDoc')}
