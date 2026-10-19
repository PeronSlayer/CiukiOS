"""Bounded read-only qcow2 overlay view for the lead's disk-log read-back.

Supports qemu-img's ordinary uncompressed v2/v3 overlays, never converts or
copies the canonical image. Unsupported feature bits fail explicitly.
Format: https://www.qemu.org/docs/master/interop/qcow2.html
SPDX-License-Identifier: GPL-2.0-only
"""
import struct


class Overlay:
    def __init__(self, overlay, base):
        self.delta=open(overlay,"rb");self.base=open(base,"rb");self.position=0
        try:
            header=self.delta.read(104)
            if len(header)<72 or header[:4]!=b"QFI\xfb": raise ValueError("invalid qcow2 header")
            version=struct.unpack_from(">I",header,4)[0]
            bits=struct.unpack_from(">I",header,20)[0]
            self.size=struct.unpack_from(">Q",header,24)[0]
            encryption,self.l1_size,self.l1=struct.unpack_from(">IIQ",header,32)
            if version not in (2,3) or encryption or not 9<=bits<=21: raise ValueError("unsupported qcow2 geometry")
            if version==3 and (len(header)<104 or struct.unpack_from(">Q",header,72)[0]):
                raise ValueError("unsupported qcow2 incompatible features")
            self.cluster=1<<bits;self.entries=self.cluster//8
            self.delta.seek(0,2);self.delta_size=self.delta.tell()
            self.base.seek(0,2)
            if self.size!=self.base.tell() or self.l1_size*self.entries*self.cluster<self.size:
                raise ValueError("qcow2 backing size mismatch")
            self._at(self.l1,self.l1_size*8)
        except BaseException:
            self.close();raise

    def _at(self,at,count):
        if at<0 or count<0 or at+count>self.delta_size: raise ValueError("qcow2 extent outside overlay")
        self.delta.seek(at);data=self.delta.read(count)
        if len(data)!=count: raise ValueError("short qcow2 read")
        return data

    def seek(self,at,whence=0):
        self.position=at if whence==0 else self.position+at if whence==1 else self.size+at if whence==2 else -1
        if self.position<0: raise ValueError("invalid overlay seek")
        return self.position

    def tell(self): return self.position

    def read(self,count=-1):
        if count<0: count=self.size-self.position
        count=max(0,min(count,self.size-self.position))
        if count>16*1024*1024: raise ValueError("overlay read budget exceeded")
        result=bytearray()
        while count:
            at=self.position;index=at//self.cluster;n=min(count,self.cluster-at%self.cluster)
            l2=struct.unpack(">Q",self._at(self.l1+(index//self.entries)*8,8))[0]
            mask=0x00fffffffffffe00
            if l2&~(mask|(1<<63)) or l2&mask and (l2&mask)%self.cluster: raise ValueError("unsupported qcow2 L1 entry")
            entry=struct.unpack(">Q",self._at((l2&mask)+(index%self.entries)*8,8))[0] if l2&mask else 0
            if entry&~(mask|(1<<63)|1): raise ValueError("unsupported compressed qcow2 cluster")
            if entry&1: block=bytes(n)
            elif entry&mask:
                if (entry&mask)%self.cluster: raise ValueError("unaligned qcow2 cluster")
                block=self._at((entry&mask)+at%self.cluster,n)
            else:
                self.base.seek(at);block=self.base.read(n)
                if len(block)!=n: raise ValueError("short backing read")
            result.extend(block);self.position+=n;count-=n
        return bytes(result)

    def close(self):
        self.delta.close();self.base.close()
    def __enter__(self): return self
    def __exit__(self,*_): self.close()
