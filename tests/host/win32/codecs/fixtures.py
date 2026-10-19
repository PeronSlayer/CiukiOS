"""Authored fixture generators and independent scalar ADPCM reference decodes."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import struct

STEPS = (7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767)
COEFS = ((256,0),(512,-256),(0,0),(192,64),(240,0),(460,-208),(392,-232))


def audio(tag, channels, rate, alignment, truncate=0):
    header = (4 if tag == 17 else 7)*channels
    samples = (alignment-header)*2//channels+(1 if tag == 17 else 2)
    extra = struct.pack('<H',samples)
    if tag == 2:
        extra += struct.pack('<H',7)+b''.join(struct.pack('<hh',*c) for c in COEFS)
    wave = struct.pack('<HHIIHHH',tag,channels,rate,rate*alignment//samples,alignment,4,len(extra))+extra
    if tag == 17:
        block = b''.join(struct.pack('<hBB',-1700+3400*c,12+c*7,0) for c in range(channels))
    else:
        block = bytes(range(channels))+struct.pack('<'+'h'*channels,*([32]*channels))+struct.pack('<'+'h'*channels,*[-1700+3400*c for c in range(channels)])+struct.pack('<'+'h'*channels,*[-1800+3400*c for c in range(channels)])
    block += bytes(((i*37+0x1b)&255) for i in range(alignment-len(block)))
    if truncate:
        block = block[:-truncate]
    return wave,block,reference(tag,channels,block)


def reference(tag,ch,block):
    header=(4 if tag==17 else 7)*ch
    frames=(1+(len(block)-header)//(4*ch)*8) if tag==17 else (2+(len(block)-header)*2//ch)
    if tag==17 and ch==2 and (len(block)-header)%8>4:
        frames+=((len(block)-header)%8-4)*2
    if tag==17 and ch==1:
        frames=1+(len(block)-header)*2
    states=[];result=[]
    for c in range(ch):
        if tag==17:
            predictor,index,_=struct.unpack_from('<hBB',block,c*4)
            states.append([predictor,index]);result.append(predictor)
        else:
            delta=struct.unpack_from('<h',block,ch+2*c)[0]
            s1=struct.unpack_from('<h',block,3*ch+2*c)[0]
            s2=struct.unpack_from('<h',block,5*ch+2*c)[0]
            states.append([s1,s2,delta,block[c]])
    if tag==2:
        result=[state[1] for state in states]+[state[0] for state in states]
    for j in range(1 if tag==17 else 2,frames):
        for c,state in enumerate(states):
            if tag==17:
                k=j-1;byte=header+(k//8)*(4*ch)+4*c+(k%8)//2
                code=(block[byte]>>(4*(k%2)))&15
                step=STEPS[state[1]]
                diff=step//8+(step//4 if code&1 else 0)+(step//2 if code&2 else 0)+(step if code&4 else 0)
                state[0]=max(-32768,min(32767,state[0]+(-diff if code&8 else diff)))
                state[1]=max(0,min(88,state[1]+(-1,-1,-1,-1,2,4,6,8)[code&7]))
            else:
                nibble=(j-2)*ch+c;code=(block[header+nibble//2]>>(0 if nibble%2 else 4))&15
                a,b=COEFS[state[3]];v=state[0]*a+state[1]*b
                prediction=abs(v)//256*(-1 if v<0 else 1)
                value=max(-32768,min(32767,prediction+(code-16 if code>=8 else code)*state[2]))
                state[1]=state[0];state[0]=value
                state[2]=max(16,(230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230)[code]*state[2]//256)
            result.append(state[0])
    return struct.pack('<'+'h'*len(result),*result)


def chunk(kind,payload):
    return kind+struct.pack('<I',len(payload))+payload+(b'\0' if len(payload)%2 else b'')


def cinepak(indices=b'\0',gray=True,partial=False,delta=False):
    values=bytes((16,48,80,112))+(b'' if gray else bytes((20,246)))
    book_id=(0x2700 if partial else 0x2600) if gray else (0x2300 if partial else 0x2200)
    book=(struct.pack('>I',0x80000000) if partial else b'')+values
    cb=struct.pack('>HH',book_id,4+len(book))+book
    pixels=(struct.pack('>HHI',0x3100,8,0) if delta else struct.pack('>HH',0x3200,4+len(indices))+indices)
    strip=struct.pack('>HHHHHH',0x1000,12+len(cb)+len(pixels),0,0,4,4)+cb+pixels
    n=10+len(strip)
    return bytes((0,))+n.to_bytes(3,'big')+struct.pack('>HHH',4,4,1)+strip


def avi(frames,compression=1,bits=8):
    fmt=struct.pack('<IiiHHIIiiII',40,4,4,1,bits,compression,0,0,0,0,0)
    # Palette is authored grayscale; no extracted media.
    fmt+=b''.join(bytes((i,i,i,0)) for i in range(1<<bits)) if bits<=8 else b''
    stream=struct.pack('<4s4sIHHIIIIIIIIhhhh',b'vids',b'mrle',0,0,0,0,1,15,0,len(frames),0,0,0,0,0,4,4)
    headers=chunk(b'LIST',b'hdrl'+chunk(b'LIST',b'strl'+chunk(b'strh',stream)+chunk(b'strf',fmt)))
    movi=chunk(b'LIST',b'movi'+b''.join(chunk(b'00dc',f) for f in frames))
    return chunk(b'RIFF',b'AVI '+headers+movi)
