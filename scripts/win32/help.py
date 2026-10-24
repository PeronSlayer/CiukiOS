"""G15B HHCTRL entry points forward to USER's native Help bridge."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
from pathlib import Path
import struct
import subprocess
from scripts.win32 import stubs
ROOT=Path(__file__).resolve().parents[2]
def build(out,base):
    out=Path(out); folder=out/'htmlhelp-objects'; folder.mkdir(parents=True,exist_ok=True)
    definition=folder/'hhctrl.def'
    definition.write_text('LIBRARY hhctrl.ocx\nEXPORTS\n HtmlHelpA=user32.CiukiHtmlHelpA @14\n HtmlHelpW=user32.CiukiHtmlHelpW @15\n')
    rc=folder/'version.rc'
    rc.write_text((ROOT/'sdk/win32/ntdll/version.rc').read_text().replace('ntdll core','HTML Help bridge'))
    resource=folder/'version.res'; subprocess.run(['llvm-rc','/fo',str(resource),str(rc)],check=True)
    image=out/'hhctrl.ocx'
    subprocess.run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no','/timestamp:0',
                    '/subsystem:windows,4.0','/osversion:4.0','/incremental:no','/fixed','/base:'+hex(base),
                    '/def:'+str(definition),'/out:'+str(image),str(resource)],check=True)
    raw=image.read_bytes(); pe=struct.unpack_from('<I',raw,60)[0]
    return {'name':'hhctrl.ocx','path':image.name,'base':base,'size':struct.unpack_from('<I',raw,pe+80)[0],
            'sha256':hashlib.sha256(raw).hexdigest(),'version':stubs.image_version(raw)}
