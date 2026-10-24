"""Authored tiny HLP/CHM containers; no proprietary help content."""
# SPDX-License-Identifier: MIT
import struct

def u16(n): return struct.pack('<H', n)
def u32(n): return struct.pack('<I', n)
def u64(n): return struct.pack('<Q', n)
def context_hash(text):
    result=0
    for c in text.upper():
        value=ord(c)-ord('A')+17 if 'A'<=c<='Z' else int(c) if '1'<=c<='9' else 10 if c=='0' else 12 if c=='.' else 13 if c=='_' else 0
        if value: result=(result*43+value)&0xffffffff
    return result

def tree(entries, kind=b'z4', multi=False):
    size=512; header=bytearray(38)
    struct.pack_into('<HHH',header,0,0x293b,2,size)
    header[6:6+len(kind)]=kind
    leaves=[entries]
    if multi: leaves=[entries[:len(entries)//2],entries[len(entries)//2:]]
    pages=[]
    for i,rows in enumerate(leaves):
        data=b''.join(rows)
        assert len(data)<=size-8
        pages.append(u16(size-8-len(data))+u16(len(rows))+u16(0xffff if not i else i-1)+u16(i+1 if i+1<len(leaves) else 0xffff)+data+bytes(size-8-len(data)))
    root=0
    if multi:
        root=len(pages); pages.append(u16(size-6)+u16(0)+u16(0)+bytes(size-6))
    struct.pack_into('<H',header,26,root)
    struct.pack_into('<H',header,30,len(pages))
    struct.pack_into('<H',header,32,2 if multi else 1)
    struct.pack_into('<I',header,34,len(entries))
    return bytes(header)+b''.join(pages)

def lz_literals(data):
    return b''.join(b'\0'+data[i:i+8] for i in range(0,len(data),8))

def hlp(phrases40=False, topic_compressed=False):
    phrase=b'An authored phrase for the Help parser. '
    text=b'\0'+phrase*4+b'\0\0'
    if phrases40:
        # One dictionary phrase: unary high part then bc=1 low bit, LSB first.
        value=len(phrase)-1; bits=[1]*(value//2)+[0,value%2]
        packed=bytearray((len(bits)+7)//8)
        for i,b in enumerate(bits): packed[i//8]|=b<<(i%8)
        dictionaries={'|PhrIndex':u32(1)+u32(1)+u32(0)+u32(len(phrase))+u32(len(phrase))+u32(0)+u16(1)+u16(0)+bytes(packed), '|PhrImage':phrase}
        encoded=b'\x0f'+b'\x00'*4+b'\x0f\x0f'  # dictionary entry 0, then one NUL
    else:
        dictionaries={'|Phrases':u16(1)+u16(0x100)+u32(len(phrase))+u16(4)+u16(4+len(phrase))+lz_literals(phrase)}
        encoded=b'\0'+b'\x01\x00'*4+b'\0\0'
    second_text=b'Second topic. A related destination.\0'
    formats=b'\x00\x80'+(bytes([len(text)*2]) if len(text)<128 else u16(len(text)*2+1))+bytes(6)+b'\xe3'+u32(context_hash('second'))+b'\x89\xff'
    records=[]
    def record(kind,body,expanded,extra=b''):
        return u32(21+len(extra)+len(body))+u32(expanded)+u32(0)+u32(0)+u32(21+len(extra))+bytes([kind])+extra+body
    records.append(record(2,b'First topic\0',12,u32(0)+u32(0xffffffff)+u32(len(text))+bytes(16)))
    records.append(record(0x20,encoded,len(text),formats))
    records.append(record(2,b'Second topic\0',13,u32(0)+u32(0)+u32(0xffffffff)+bytes(16)))
    records.append(record(0x20,second_text,len(second_text),b'\x00\x80'+bytes([len(second_text)*2])+bytes(6)+b'\xff'))
    offset=12
    for i,r in enumerate(records):
        next_ref=offset+len(r) if i+1<len(records) else 0xffffffff
        records[i]=r[:12]+u32(next_ref)+r[16:]; offset+=len(r)
    stream=b''.join(records)
    topic=bytes(12)+(lz_literals(stream) if topic_compressed else stream)
    system=u16(0x036c)+u16(33)+u16(1)+u32(0)+u16(4 if topic_compressed else 0)+u16(3)+u16(4)+u32(0)
    files={'|SYSTEM':system,'|TOPIC':topic,'|CONTEXT':tree([u32(context_hash('first'))+u32(0),u32(context_hash('second'))+u32(len(text))],b'L4'),
           '|CTXOMAP':u16(2)+u32(41)+u32(0)+u32(42)+u32(len(text)),
           '|KWBTREE':tree([b'Overview\0'+u16(1)+u32(0),b'Second\0'+u16(1)+u32(4)]),
           '|KWDATA':u32(0)+u32(len(text)),**dictionaries}
    data=bytearray(16); entries=[]
    for name,contents in sorted(files.items()):
        entries.append(name.encode()+b'\0'+u32(len(data)))
        data+=u32(len(contents)+9)+u32(len(contents))+b'\0'+contents
    directory=tree(entries,multi=True); directory_offset=len(data)
    data+=u32(len(directory)+9)+u32(len(directory))+b'\0'+directory
    data[:16]=u32(0x35f3f)+u32(directory_offset)+u32(0xffffffff)+u32(len(data))
    return bytes(data)

def encint(n):
    out=bytearray([n&127]); n>>=7
    while n: out.insert(0,128|(n&127)); n>>=7
    return bytes(out)

def chm(compressed=False):
    members={'/first.htm':b'<html><h1>First topic</h1><p>Authored &amp; readable.</p><a href="sub/second.htm">Second topic</a><script>hidden script</script></html>',
             '/sub/second.htm':b'<p>Introduction.</p><h1 id="target">Second topic</h1><p>Related destination.</p><a href="../first.htm">First topic</a>',
             '/contents.hhc':b'<UL><LI><OBJECT type="text/sitemap"><param name="Name" value="Guide book"></OBJECT><UL><LI><OBJECT type="text/sitemap"><param name="Name" value="Welcome page"><param name="Local" value="first.htm"></OBJECT><LI><OBJECT type="text/sitemap"><param name="Name" value="Destination"><param name="Local" value="sub/second.htm"></OBJECT></UL></UL>',
             '/index.hhk':b'<OBJECT type="text/sitemap"><param name="Name" value="Overview"><param name="Local" value="first.htm"></OBJECT><OBJECT type="text/sitemap"><param name="Name" value="Second"><param name="Local" value="sub/second.htm"></OBJECT>'}
    system=u32(3)
    for kind,name in ((0,'contents.hhc'),(1,'index.hhk'),(2,'first.htm')):
        system+=u16(kind)+u16(len(name)+1)+name.encode()+b'\0'
    members['/#SYSTEM']=system
    members['/#STRINGS']=b'sub/second.htm\0'
    members['/#IVB']=u32(8)+u32(42)+u32(0)
    raw=bytearray(); items=[]
    for name,contents in sorted(members.items()):
        items.append((name,1 if compressed else 0,len(raw),len(contents))); raw+=contents
    if compressed:
        raw+=bytes(32768-len(raw))
        bits='0'+'011'+format(len(raw)>>8,'016b')+format(len(raw)&255,'08b')
        bits=bits.ljust((len(bits)+15)//16*16,'0')
        packed=b''.join(u16(int(bits[i:i+16],2)) for i in range(0,len(bits),16))
        stream=packed+u32(1)*3+bytes(raw)
        raw=bytearray()
        for name,contents in {
            '::DataSpace/Storage/MSCompressed/Content':stream,
            '::DataSpace/Storage/MSCompressed/ControlData':u32(6)+b'LZXC'+u32(2)+u32(1)+u32(1)+u32(0)+u32(0),
            '::DataSpace/Storage/MSCompressed/SpanInfo':u64(32768)}.items():
            items.append((name,0,len(raw),len(contents))); raw+=contents
    entries=b''.join(encint(len(name.encode()))+name.encode()+encint(section)+encint(offset)+encint(size)
                     for name,section,offset,size in sorted(items))
    chunk=b'PMGL'+u32(4096-20-len(entries))+u32(0)+u32(0xffffffff)*2+entries
    chunk+=bytes(4096-len(chunk)-2)+u16(len(items))
    directory=bytearray(0x54)
    directory[:4]=b'ITSP'
    for at,value in ((4,1),(8,0x54),(16,4096),(20,2),(24,1),(28,0xffffffff),(32,0),(36,0),(44,1)):
        struct.pack_into('<I',directory,at,value)
    directory+=chunk
    header=bytearray(0x60); header[:4]=b'ITSF'
    struct.pack_into('<II',header,4,3,0x60)
    header[24:56]=bytes.fromhex('10fd017caa7bd0119e0c00a0c922e6ec11fd017caa7bd0119e0c00a0c922e6ec')
    content_offset=0x60+0x18+len(directory)
    struct.pack_into('<QQQQQ',header,56,0x60,0x18,0x78,len(directory),content_offset)
    section0=u32(0x1fe)+u32(0)+u64(content_offset+len(raw))+u32(0)*2
    return bytes(header)+section0+bytes(directory)+bytes(raw)
