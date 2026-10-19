"""Authored SF2 saw wave, 128 transposed programs and a percussion preset."""
import struct
from pathlib import Path

def chunk(tag,data):
    return tag+struct.pack('<I',len(data))+data+b'\0'*(len(data)&1)

def soundfont():
    presets=[(i,0) for i in range(128)]+[(0,128)]
    phdr=b'';pbag=b'';pgen=b''
    for i,(program,bank) in enumerate(presets):
        phdr+=struct.pack('<20sHHHIII',f'Authored {bank}:{program}'.encode(),program,bank,i,0,0,0)
        pbag+=struct.pack('<HH',2*i,0)
        pgen+=struct.pack('<HHHH',51,program%12,41,0)
    phdr+=struct.pack('<20sHHHIII',b'EOP',0,0,len(presets),0,0,0)
    pbag+=struct.pack('<HH',2*len(presets),0)
    pgen+=b'\0'*4
    sample=struct.pack('<'+'h'*4142,*([((i%128)-64)*256 for i in range(4096)]+[0]*46))
    shdr=struct.pack('<20sIIIIIBbHH',b'Authored saw',0,4096,128,3968,44100,60,0,0,1)
    shdr+=struct.pack('<20sIIIIIBbHH',b'EOS',4096,4096,4096,4096,44100,0,0,0,1)
    tables=[(b'phdr',phdr),(b'pbag',pbag),(b'pmod',b'\0'*10),(b'pgen',pgen),
            (b'inst',struct.pack('<20sH20sH',b'Authored',0,b'EOI',1)),
            (b'ibag',struct.pack('<HHHH',0,0,2,0)),(b'imod',b'\0'*10),
            (b'igen',struct.pack('<HHHHHH',54,1,53,0,0,0)),(b'shdr',shdr)]
    return chunk(b'RIFF',b'sfbk'+chunk(b'LIST',b'INFO'+chunk(b'ifil',struct.pack('<HH',2,1)))+
                 chunk(b'LIST',b'sdta'+chunk(b'smpl',sample))+
                 chunk(b'LIST',b'pdta'+b''.join(chunk(*p) for p in tables)))
if __name__=='__main__':
    import sys
    Path(sys.argv[1]).write_bytes(soundfont())
