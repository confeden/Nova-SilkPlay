"""Adversarial check the original probes never did:
feed NVOFA REAL content (noise pattern + a known shifted copy), read the flow
vectors back, and verify they equal the known ground-truth shift.
Also re-times with real content vs uninitialized VRAM.
Usage: python verify_ofcontent.py <perf 1|2|3> <grid 1|2|4> <iters> [dx dy] [--zero]
"""
import sys, random, struct
from ctypes import *

vk = CDLL(r"C:\Windows\System32\vulkan-1.dll")
PERF=int(sys.argv[1]); GRID=int(sys.argv[2]); ITER=int(sys.argv[3])
DX=int(sys.argv[4]) if len(sys.argv)>4 else 8
DY=int(sys.argv[5]) if len(sys.argv)>5 else 4
ZERO = '--zero' in sys.argv
W,H = 2560,1440

class A(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('an',c_char_p),('av',c_uint32),('en',c_char_p),('ev',c_uint32),('api',c_uint32)]
class ICI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('ai',POINTER(A)),('lc',c_uint32),('ln',c_void_p),('ec',c_uint32),('en',c_void_p)]
ap=A(s=0,p=None,an=b'of',av=1,en=b'n',ev=1,api=(1<<22)|(3<<12))
inst=c_void_p(); vk.vkCreateInstance(byref(ICI(s=1,p=None,f=0,ai=pointer(ap),lc=0,ln=None,ec=0,en=None)),None,byref(inst))
n=c_uint32(0); vk.vkEnumeratePhysicalDevices(inst,byref(n),None); pdl=(c_void_p*n.value)(); vk.vkEnumeratePhysicalDevices(inst,byref(n),pdl); pd=pdl[0]
class QFP(Structure): _fields_=[('flags',c_uint32),('count',c_uint32),('ts',c_uint32),('a',c_uint32),('b',c_uint32),('c',c_uint32)]
qn=c_uint32(0); vk.vkGetPhysicalDeviceQueueFamilyProperties(pd,byref(qn),None)
qfs=(QFP*qn.value)(); vk.vkGetPhysicalDeviceQueueFamilyProperties(pd,byref(qn),qfs)
OFQF=[i for i in range(qn.value) if qfs[i].flags&0x100][0]

class OFFeat(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('opticalFlow',c_uint32)]
class V13(Structure): _fields_=[('s',c_uint32),('p',c_void_p)]+[('b%d'%i,c_uint32) for i in range(15)]
class F2(Structure): _fields_=[('s',c_uint32),('p',c_void_p)]+[('f%d'%i,c_uint32) for i in range(55)]
class QCI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('qfi',c_uint32),('qc',c_uint32),('pri',POINTER(c_float))]
class DCI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('qcic',c_uint32),('pq',POINTER(QCI)),('lc',c_uint32),('ln',c_void_p),('ec',c_uint32),('en',c_void_p),('pf',c_void_p)]
off=OFFeat(s=1000464000,p=None,opticalFlow=1)
v13=V13(s=53,p=cast(pointer(off),c_void_p)); v13.b9=1
f2=F2(s=1000059000,p=cast(pointer(v13),c_void_p))
pri=(c_float*1)(1.0); q=QCI(s=2,p=None,f=0,qfi=OFQF,qc=1,pri=pri)
extn=(c_char_p*1)(b"VK_NV_optical_flow")
dev=c_void_p(); vk.vkCreateDevice(pd,byref(DCI(s=3,p=cast(pointer(f2),c_void_p),f=0,qcic=1,pq=pointer(q),lc=0,ln=None,ec=1,en=cast(extn,c_void_p),pf=None)),None,byref(dev))
g=vk.vkGetDeviceProcAddr; g.restype=c_void_p; g.argtypes=[c_void_p,c_char_p]
mkS=CFUNCTYPE(c_int,c_void_p,c_void_p,c_void_p,POINTER(c_uint64))(g(dev,b"vkCreateOpticalFlowSessionNV"))
bnd=CFUNCTYPE(c_int,c_void_p,c_uint64,c_int,c_uint64,c_int)(g(dev,b"vkBindOpticalFlowSessionImageNV"))
exe=CFUNCTYPE(None,c_void_p,c_uint64,c_void_p)(g(dev,b"vkCmdOpticalFlowExecuteNV"))

FMT_BGRA,FMT_S105=44,1000464000
class OFIFI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('usage',c_uint32)]
class Ext(Structure): _fields_=[('w',c_uint32),('h',c_uint32),('d',c_uint32)]
class Off3(Structure): _fields_=[('x',c_int32),('y',c_int32),('z',c_int32)]
class SRR(Structure): _fields_=[('a',c_uint32),('bm',c_uint32),('lc',c_uint32),('bl',c_uint32),('ly',c_uint32)]
class SRL(Structure): _fields_=[('a',c_uint32),('mip',c_uint32),('bl',c_uint32),('lc',c_uint32)]
class BIC(Structure): _fields_=[('off',c_uint64),('rowlen',c_uint32),('imgh',c_uint32),('sub',SRL),('o',Off3),('e',Ext)]
class ImgCI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('it',c_uint32),('fmt',c_uint32),('ext',Ext),('mips',c_uint32),('layers',c_uint32),('samples',c_uint32),('tiling',c_uint32),('usage',c_uint32),('sharing',c_uint32),('qfic',c_uint32),('pqfi',c_void_p),('il',c_uint32)]
class BufCI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('size',c_uint64),('usage',c_uint32),('sharing',c_uint32),('qfic',c_uint32),('pqfi',c_void_p)]
class MemReq(Structure): _fields_=[('size',c_uint64),('align',c_uint64),('bits',c_uint32)]
class MAI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('size',c_uint64),('idx',c_uint32)]
class MT(Structure): _fields_=[('f',c_uint32),('h',c_uint32)]
class MH(Structure): _fields_=[('s',c_uint64),('f',c_uint32)]
class MP(Structure): _fields_=[('tc',c_uint32),('t',MT*32),('hc',c_uint32),('h',MH*16)]
class IVCI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('img',c_uint64),('vt',c_uint32),('fmt',c_uint32),('r',c_uint32),('gg',c_uint32),('b',c_uint32),('a',c_uint32),('sr',SRR)]
mp=MP(); vk.vkGetPhysicalDeviceMemoryProperties(pd,byref(mp))

def mkimg(fmt,w,h,u):
    fi=OFIFI(s=1000464002,p=None,usage=u)
    # SAMPLED|STORAGE|TRANSFER_SRC|TRANSFER_DST
    ic=ImgCI(s=14,p=cast(pointer(fi),c_void_p),f=0,it=1,fmt=fmt,ext=Ext(w,h,1),mips=1,layers=1,samples=1,tiling=0,usage=(1<<2)|(1<<3)|(1<<0)|(1<<1),sharing=0,qfic=0,pqfi=None,il=0)
    im=c_uint64(); r=vk.vkCreateImage(dev,byref(ic),None,byref(im)); assert r==0,("createImage",r)
    mr=MemReq(); vk.vkGetImageMemoryRequirements(dev,im,byref(mr))
    idx=next(i for i in range(mp.tc) if (mr.bits>>i)&1 and mp.t[i].f&1)
    mem=c_uint64(); vk.vkAllocateMemory(dev,byref(MAI(s=5,p=None,size=mr.size,idx=idx)),None,byref(mem))
    vk.vkBindImageMemory(dev,im,mem,0)
    v=c_uint64(); vk.vkCreateImageView(dev,byref(IVCI(s=15,p=None,f=0,img=im.value,vt=1,fmt=fmt,r=0,gg=0,b=0,a=0,sr=SRR(1,0,1,0,1))),None,byref(v))
    return im.value,v.value

def mkbuf(size,usage):
    b=c_uint64(); r=vk.vkCreateBuffer(dev,byref(BufCI(s=12,p=None,f=0,size=size,usage=usage,sharing=0,qfic=0,pqfi=None)),None,byref(b)); assert r==0,r
    mr=MemReq(); vk.vkGetBufferMemoryRequirements(dev,b,byref(mr))
    # HOST_VISIBLE|HOST_COHERENT = 0x2|0x4
    idx=next(i for i in range(mp.tc) if (mr.bits>>i)&1 and (mp.t[i].f&0x6)==0x6)
    mem=c_uint64(); r=vk.vkAllocateMemory(dev,byref(MAI(s=5,p=None,size=mr.size,idx=idx)),None,byref(mem)); assert r==0,r
    vk.vkBindBufferMemory(dev,b,mem,0)
    ptr=c_void_p(); vk.vkMapMemory(dev,mem,0,mr.size,0,byref(ptr))
    return b.value,ptr

fw,fh=(W+GRID-1)//GRID,(H+GRID-1)//GRID
i1,v1=mkimg(FMT_BGRA,W,H,1); i2,v2=mkimg(FMT_BGRA,W,H,1); i3,v3=mkimg(FMT_S105,fw,fh,2)

# ---- build content: blocky noise (video-like, has texture at many scales) ----
stage_sz=W*H*4
sb1,sp1=mkbuf(stage_sz,1)   # TRANSFER_SRC
sb2,sp2=mkbuf(stage_sz,1)
readback,rp=mkbuf(fw*fh*4,2)  # TRANSFER_DST

if not ZERO:
    random.seed(1234)
    CELL=16
    cw,ch=(W//CELL)+2,(H//CELL)+2
    cells=[random.randrange(256) for _ in range(cw*ch)]
    row=(c_uint8*(W*4))()
    buf1=cast(sp1,POINTER(c_uint8)); buf2=cast(sp2,POINTER(c_uint8))
    for y in range(H):
        cy=y//CELL
        base=y*W*4
        for x in range(0,W,4):
            v=cells[cy*cw+(x//CELL)]
            for k in range(4):
                o=base+(x+k)*4
                buf1[o]=v; buf1[o+1]=(v*7)&255; buf1[o+2]=(v*13)&255; buf1[o+3]=255
    # image2 = image1 shifted by (DX,DY): pixel(x,y) in img2 == pixel(x-DX, y-DY) in img1
    for y in range(H):
        sy=y-DY
        if sy<0 or sy>=H:
            for x in range(W):
                o=(y*W+x)*4; buf2[o]=0; buf2[o+1]=0; buf2[o+2]=0; buf2[o+3]=255
            continue
        for x in range(W):
            sx=x-DX
            o=(y*W+x)*4
            if sx<0 or sx>=W:
                buf2[o]=0; buf2[o+1]=0; buf2[o+2]=0; buf2[o+3]=255
            else:
                s=(sy*W+sx)*4
                buf2[o]=buf1[s]; buf2[o+1]=buf1[s+1]; buf2[o+2]=buf1[s+2]; buf2[o+3]=255

class OFSCI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('w',c_uint32),('h',c_uint32),('imf',c_uint32),('fvf',c_uint32),('cf',c_uint32),('og',c_uint32),('hg',c_uint32),('perf',c_uint32),('flags',c_uint32)]
class OFEI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('rc',c_uint32),('pr',c_void_p)]
class CPCI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('qfi',c_uint32)]
class CBAI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('pool',c_uint64),('l',c_uint32),('c',c_uint32)]
class CBBI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('i',c_void_p)]
class QPCI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('qt',c_uint32),('c',c_uint32),('st',c_uint32)]
class IMB(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('sa',c_uint32),('da',c_uint32),('ol',c_uint32),('nl',c_uint32),('sq',c_uint32),('dq',c_uint32),('img',c_uint64),('sr',SRR)]
class SI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('wsc',c_uint32),('pws',c_void_p),('pw',c_void_p),('cbc',c_uint32),('pcb',POINTER(c_void_p)),('ssc',c_uint32),('pss',c_void_p)]

sci=OFSCI(s=1000464004,p=None,w=W,h=H,imf=FMT_BGRA,fvf=FMT_S105,cf=0,og=GRID,hg=0,perf=PERF,flags=0)
sess=c_uint64(); r=mkS(dev,byref(sci),None,byref(sess))
assert r==0,("createSession",r)
for bp,v in [(1,v1),(2,v2),(4,v3)]:
    rb=bnd(dev,sess,bp,v,1); assert rb==0,("bind",bp,rb)

pool=c_uint64(); vk.vkCreateCommandPool(dev,byref(CPCI(s=39,p=None,f=2,qfi=OFQF)),None,byref(pool))
cb=c_void_p(); vk.vkAllocateCommandBuffers(dev,byref(CBAI(s=40,p=None,pool=pool.value,l=0,c=1)),byref(cb))
qq=c_void_p(); vk.vkGetDeviceQueue(dev,OFQF,0,byref(qq))
qp=c_uint64(); vk.vkCreateQueryPool(dev,byref(QPCI(s=11,p=None,f=0,qt=2,c=2*ITER,st=0)),None,byref(qp))

vk.vkBeginCommandBuffer(cb,byref(CBBI(s=42,p=None,f=0,i=None)))
vk.vkCmdResetQueryPool(cb,qp,0,2*ITER)
# UNDEFINED -> TRANSFER_DST(7) for the two inputs, UNDEFINED -> GENERAL for flow
b_in=(IMB*2)(*[IMB(s=45,p=None,sa=0,da=0x1000,ol=0,nl=7,sq=0xffffffff,dq=0xffffffff,img=i,sr=SRR(1,0,1,0,1)) for i in (i1,i2)])
vk.vkCmdPipelineBarrier(cb,0x10000,0x10000,0,0,None,0,None,2,b_in)
if not ZERO:
    reg=BIC(off=0,rowlen=0,imgh=0,sub=SRL(1,0,0,1),o=Off3(0,0,0),e=Ext(W,H,1))
    vk.vkCmdCopyBufferToImage(cb,sb1,i1,7,1,byref(reg))
    vk.vkCmdCopyBufferToImage(cb,sb2,i2,7,1,byref(reg))
b_all=(IMB*3)(*[IMB(s=45,p=None,sa=0x1000,da=0,ol=(7 if i in (i1,i2) and not ZERO else (7 if i in (i1,i2) else 0)),nl=1,sq=0xffffffff,dq=0xffffffff,img=i,sr=SRR(1,0,1,0,1)) for i in (i1,i2,i3)])
b_all[2].ol=0
vk.vkCmdPipelineBarrier(cb,0x10000,0x10000,0,0,None,0,None,3,b_all)
ei=OFEI(s=1000464005,p=None,f=0,rc=0,pr=None)
for k in range(ITER):
    vk.vkCmdWriteTimestamp(cb,1,qp,2*k); exe(cb,sess,byref(ei)); vk.vkCmdWriteTimestamp(cb,0x2000,qp,2*k+1)
    vk.vkCmdPipelineBarrier(cb,0x10000,0x10000,0,0,None,0,None,0,None)
# copy flow -> readback
b_fl=(IMB*1)(IMB(s=45,p=None,sa=0,da=0x800,ol=1,nl=6,sq=0xffffffff,dq=0xffffffff,img=i3,sr=SRR(1,0,1,0,1)))
vk.vkCmdPipelineBarrier(cb,0x10000,0x10000,0,0,None,0,None,1,b_fl)
regf=BIC(off=0,rowlen=0,imgh=0,sub=SRL(1,0,0,1),o=Off3(0,0,0),e=Ext(fw,fh,1))
vk.vkCmdCopyImageToBuffer(cb,i3,6,readback,1,byref(regf))
vk.vkEndCommandBuffer(cb)
si=SI(s=4,p=None,wsc=0,pws=None,pw=None,cbc=1,pcb=pointer(cb),ssc=0,pss=None)
rs=vk.vkQueueSubmit(qq,1,byref(si),0); rw=vk.vkQueueWaitIdle(qq)
print("content=%s perf=%d grid=%d  submit=%d waitIdle=%d"%("ZERO/uninit" if ZERO else "noise+shift(%d,%d)"%(DX,DY),PERF,GRID,rs,rw))
if rw!=0: sys.exit(1)
res=(c_uint64*(2*ITER))()
vk.vkGetQueryPoolResults.argtypes=[c_void_p,c_uint64,c_uint32,c_uint32,c_size_t,c_void_p,c_uint64,c_uint32]
rq=vk.vkGetQueryPoolResults(dev,qp,0,2*ITER,sizeof(res),res,8,1|2)
ts=sorted((res[2*k+1]-res[2*k])/1e6 for k in range(ITER))
print("  median=%.3f ms  min=%.3f  p95=%.3f  (flow %dx%d)"%(ts[ITER//2],ts[0],ts[int(ITER*0.95)],fw,fh))

# ---- decode S10.5 flow vectors ----
fp=cast(rp,POINTER(c_int16))
samples=[]
for gy in range(fh//4, fh*3//4, max(1,fh//16)):
    for gx in range(fw//4, fw*3//4, max(1,fw//16)):
        o=(gy*fw+gx)*2
        samples.append((fp[o]/32.0, fp[o+1]/32.0))
import statistics
xs=[s[0] for s in samples]; ys=[s[1] for s in samples]
print("  flow samples n=%d  median=(%.3f, %.3f)  mean=(%.3f, %.3f)  nonzero=%d/%d"%(
    len(samples),statistics.median(xs),statistics.median(ys),
    sum(xs)/len(xs),sum(ys)/len(ys),
    sum(1 for a,b in samples if a or b),len(samples)))
print("  first 8 vectors:",[("%.2f,%.2f"%s) for s in samples[:8]])
