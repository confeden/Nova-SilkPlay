"""Independent verification of NVOFA capability claims (adversarial pass).
Queries VkPhysicalDeviceOpticalFlowFeaturesNV / PropertiesNV and
vkGetPhysicalDeviceOpticalFlowImageFormatsNV from scratch.
"""
import sys
from ctypes import *

vk = CDLL(r"C:\Windows\System32\vulkan-1.dll")

class A(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('an',c_char_p),('av',c_uint32),('en',c_char_p),('ev',c_uint32),('api',c_uint32)]
class ICI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('f',c_uint32),('ai',POINTER(A)),('lc',c_uint32),('ln',c_void_p),('ec',c_uint32),('en',c_void_p)]
ap=A(s=0,p=None,an=b'verify',av=1,en=b'n',ev=1,api=(1<<22)|(3<<12))
inst=c_void_p()
r=vk.vkCreateInstance(byref(ICI(s=1,p=None,f=0,ai=pointer(ap),lc=0,ln=None,ec=0,en=None)),None,byref(inst))
print("vkCreateInstance ->",r)
n=c_uint32(0); vk.vkEnumeratePhysicalDevices(inst,byref(n),None)
pdl=(c_void_p*n.value)(); vk.vkEnumeratePhysicalDevices(inst,byref(n),pdl); pd=pdl[0]

# ---- device properties (name / ids / driver) ----
class PDP(Structure):
    _fields_=[('api',c_uint32),('drv',c_uint32),('vendor',c_uint32),('device',c_uint32),
              ('type',c_uint32),('name',c_char*256),('uuid',c_uint8*16),
              ('limits',c_uint8*1024),('sparse',c_uint8*64)]
p=PDP(); vk.vkGetPhysicalDeviceProperties(pd,byref(p))
print("device: %s vendor=0x%04x device=0x%04x api=%d.%d.%d driverVersionRaw=%u"%(
    p.name.decode(),p.vendor,p.device,p.api>>22,(p.api>>12)&0x3ff,p.api&0xfff,p.drv))

# ---- extension present? ----
en=c_uint32(0); vk.vkEnumerateDeviceExtensionProperties(pd,None,byref(en),None)
class EP(Structure): _fields_=[('name',c_char*256),('rev',c_uint32)]
eps=(EP*en.value)(); vk.vkEnumerateDeviceExtensionProperties(pd,None,byref(en),eps)
ofext=[(e.name.decode(),e.rev) for e in eps if b'optical' in e.name]
print("optical extensions:",ofext)

# ---- queue families ----
class QFP(Structure): _fields_=[('flags',c_uint32),('count',c_uint32),('ts',c_uint32),('a',c_uint32),('b',c_uint32),('c',c_uint32)]
qn=c_uint32(0); vk.vkGetPhysicalDeviceQueueFamilyProperties(pd,byref(qn),None)
qfs=(QFP*qn.value)(); vk.vkGetPhysicalDeviceQueueFamilyProperties(pd,byref(qn),qfs)
for i in range(qn.value):
    print("  qf[%d] flags=0x%03x count=%d tsbits=%d"%(i,qfs[i].flags,qfs[i].count,qfs[i].ts))

# ---- Features2 with OpticalFlowFeaturesNV ----
class OFFeat(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('opticalFlow',c_uint32)]
class F2(Structure): _fields_=[('s',c_uint32),('p',c_void_p)]+[('f%d'%i,c_uint32) for i in range(55)]
off=OFFeat(s=1000464000,p=None,opticalFlow=0xDEADBEEF)
f2=F2(s=1000059000,p=cast(pointer(off),c_void_p))
gf2=vk.vkGetInstanceProcAddr; gf2.restype=c_void_p; gf2.argtypes=[c_void_p,c_char_p]
fn=CFUNCTYPE(None,c_void_p,c_void_p)(gf2(inst,b"vkGetPhysicalDeviceFeatures2"))
fn(pd,byref(f2))
print("OpticalFlowFeaturesNV.opticalFlow =",off.opticalFlow)

# ---- Properties2 with OpticalFlowPropertiesNV ----
class OFProps(Structure):
    _fields_=[('s',c_uint32),('p',c_void_p),
              ('supportedOutputGridSizes',c_uint32),
              ('supportedHintGridSizes',c_uint32),
              ('hintSupported',c_uint32),
              ('costSupported',c_uint32),
              ('bidirectionalFlowSupported',c_uint32),
              ('globalFlowSupported',c_uint32),
              ('minWidth',c_uint32),('minHeight',c_uint32),
              ('maxWidth',c_uint32),('maxHeight',c_uint32),
              ('maxNumRegionsOfInterest',c_uint32)]
class P2(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('props',PDP)]
ofp=OFProps(s=1000464001,p=None)
for f,_ in OFProps._fields_[2:]: setattr(ofp,f,0xDEADBEEF)
p2=P2(s=1000059001,p=cast(pointer(ofp),c_void_p))
fn2=CFUNCTYPE(None,c_void_p,c_void_p)(gf2(inst,b"vkGetPhysicalDeviceProperties2"))
fn2(pd,byref(p2))
def gs(m):
    return "|".join(n for b,n in ((1,'1x1'),(2,'2x2'),(4,'4x4'),(8,'8x8')) if m&b) or "NONE"
print("OpticalFlowPropertiesNV:")
print("  supportedOutputGridSizes = 0x%x (%s)"%(ofp.supportedOutputGridSizes,gs(ofp.supportedOutputGridSizes)))
print("  supportedHintGridSizes   = 0x%x (%s)"%(ofp.supportedHintGridSizes,gs(ofp.supportedHintGridSizes)))
print("  hintSupported=%d costSupported=%d bidirectionalFlowSupported=%d globalFlowSupported=%d"%(
    ofp.hintSupported,ofp.costSupported,ofp.bidirectionalFlowSupported,ofp.globalFlowSupported))
print("  min=%dx%d max=%dx%d maxROIs=%d"%(ofp.minWidth,ofp.minHeight,ofp.maxWidth,ofp.maxHeight,ofp.maxNumRegionsOfInterest))

# ---- image formats per usage ----
class OFIFI(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('usage',c_uint32)]
class OFIFP(Structure): _fields_=[('s',c_uint32),('p',c_void_p),('format',c_uint32)]
gfmt=CFUNCTYPE(c_int,c_void_p,c_void_p,POINTER(c_uint32),POINTER(OFIFP))(
    gf2(inst,b"vkGetPhysicalDeviceOpticalFlowImageFormatsNV"))
FMTNAME={9:'R8_UNORM',13:'R8_UINT',37:'R8G8B8A8_UNORM',44:'B8G8R8A8_UNORM',98:'R32_UINT',
         1000156003:'G8_B8_R8_3PLANE_420_UNORM',1000156004:'G8_B8R8_2PLANE_420_UNORM',
         1000464000:'R16G16_S10_5_NV',30:'R8G8_UNORM',
         1000156007:'G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16',
         1000156017:'G16_B16R16_2PLANE_420_UNORM',
         1000156008:'G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16'}
for uname,ubit in [('UNKNOWN',0),('INPUT',1),('OUTPUT',2),('HINT',4),('COST',8),('GLOBAL_FLOW',16)]:
    fi=OFIFI(s=1000464002,p=None,usage=ubit)
    c=c_uint32(0)
    rr=gfmt(pd,byref(fi),byref(c),None)
    if rr!=0 or c.value==0:
        print("  usage %-12s -> result=%d count=%d"%(uname,rr,c.value)); continue
    arr=(OFIFP*c.value)()
    for a in arr: a.s=1000464003; a.p=None
    gfmt(pd,byref(fi),byref(c),arr)
    print("  usage %-12s -> %s"%(uname,[ "%s(%d)"%(FMTNAME.get(a.format,'?'),a.format) for a in arr]))
