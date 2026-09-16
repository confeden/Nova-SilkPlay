"""
Nova SilkPlay - NVOFA capability + timing probe (Vulkan VK_NV_optical_flow path).
No NVIDIA SDK headers required: talks to the driver's vulkan-1.dll via ctypes.
Run: python ofbench.py
"""
import ctypes, sys
from ctypes import *

vk = CDLL(r"C:\Windows\System32\vulkan-1.dll")

def chk(n, r):
    if r != 0:
        print("FAIL %s -> VkResult %d" % (n, r)); sys.exit(1)
    return r

class A(Structure):
    _fields_ = [('s', c_uint32), ('p', c_void_p), ('an', c_char_p), ('av', c_uint32),
                ('en', c_char_p), ('ev', c_uint32), ('api', c_uint32)]
class ICI(Structure):
    _fields_ = [('s', c_uint32), ('p', c_void_p), ('f', c_uint32), ('ai', POINTER(A)),
                ('lc', c_uint32), ('ln', c_void_p), ('ec', c_uint32), ('en', c_void_p)]

ap = A(s=0, p=None, an=b'ofbench', av=1, en=b'nova', ev=1, api=(1 << 22) | (3 << 12))
ici = ICI(s=1, p=None, f=0, ai=pointer(ap), lc=0, ln=None, ec=0, en=None)
inst = c_void_p(); chk("createInstance", vk.vkCreateInstance(byref(ici), None, byref(inst)))
n = c_uint32(0); vk.vkEnumeratePhysicalDevices(inst, byref(n), None)
pdl = (c_void_p * n.value)(); vk.vkEnumeratePhysicalDevices(inst, byref(n), pdl); pd = pdl[0]

# --- find the queue family exposing VK_QUEUE_OPTICAL_FLOW_BIT_NV (0x100) ---
class QFP(Structure):
    _fields_ = [('flags', c_uint32), ('count', c_uint32), ('tsbits', c_uint32),
                ('gw', c_uint32), ('gh', c_uint32), ('gd', c_uint32)]
qn = c_uint32(0); vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, byref(qn), None)
qfs = (QFP * qn.value)(); vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, byref(qn), qfs)
OFQF = None
for i in range(qn.value):
    if qfs[i].flags & 0x100:
        OFQF = i
        print("optical-flow queue family = %d (queues=%d, timestampValidBits=%d)"
              % (i, qfs[i].count, qfs[i].tsbits))
if OFQF is None:
    print("NO VK_QUEUE_OPTICAL_FLOW_BIT_NV QUEUE FAMILY -> NVOFA ABSENT. I6 FALSIFIED."); sys.exit(2)

class OFFeat(Structure): _fields_ = [('s', c_uint32), ('p', c_void_p), ('opticalFlow', c_uint32)]
class V13(Structure): _fields_ = [('s', c_uint32), ('p', c_void_p)] + [('b%d' % i, c_uint32) for i in range(15)]
class F2(Structure):  _fields_ = [('s', c_uint32), ('p', c_void_p)] + [('f%d' % i, c_uint32) for i in range(55)]
class QCI(Structure):
    _fields_ = [('s', c_uint32), ('p', c_void_p), ('f', c_uint32), ('qfi', c_uint32),
                ('qc', c_uint32), ('pri', POINTER(c_float))]
class DCI(Structure):
    _fields_ = [('s', c_uint32), ('p', c_void_p), ('f', c_uint32), ('qcic', c_uint32),
                ('pq', POINTER(QCI)), ('lc', c_uint32), ('ln', c_void_p),
                ('ec', c_uint32), ('en', c_void_p), ('pf', c_void_p)]

off = OFFeat(s=1000464000, p=None, opticalFlow=1)
v13 = V13(s=53, p=cast(pointer(off), c_void_p)); v13.b9 = 1          # synchronization2
f2 = F2(s=1000059000, p=cast(pointer(v13), c_void_p))
pri = (c_float * 1)(1.0)
q = QCI(s=2, p=None, f=0, qfi=OFQF, qc=1, pri=pri)
extn = (c_char_p * 1)(b"VK_NV_optical_flow")
dci = DCI(s=3, p=cast(pointer(f2), c_void_p), f=0, qcic=1, pq=pointer(q),
          lc=0, ln=None, ec=1, en=cast(extn, c_void_p), pf=None)
dev = c_void_p(); chk("createDevice", vk.vkCreateDevice(pd, byref(dci), None, byref(dev)))
print("vkCreateDevice with VK_NV_optical_flow enabled: OK")

gdpa = vk.vkGetDeviceProcAddr; gdpa.restype = c_void_p; gdpa.argtypes = [c_void_p, c_char_p]
def dfn(name, proto):
    p = gdpa(dev, name.encode())
    if not p: print("missing entrypoint", name); sys.exit(1)
    return proto(p)
vkCreateOFSession = dfn("vkCreateOpticalFlowSessionNV", CFUNCTYPE(c_int, c_void_p, c_void_p, c_void_p, POINTER(c_uint64)))
vkDestroyOFSession = dfn("vkDestroyOpticalFlowSessionNV", CFUNCTYPE(None, c_void_p, c_uint64, c_void_p))
vkBindOFImage    = dfn("vkBindOpticalFlowSessionImageNV", CFUNCTYPE(c_int, c_void_p, c_uint64, c_int, c_uint64, c_int))
vkCmdOFExecute   = dfn("vkCmdOpticalFlowExecuteNV", CFUNCTYPE(None, c_void_p, c_uint64, c_void_p))

W, H = 2560, 1440
FMT_BGRA, FMT_S105 = 44, 1000464000

class OFIFI(Structure): _fields_ = [('s', c_uint32), ('p', c_void_p), ('usage', c_uint32)]
class Ext(Structure):   _fields_ = [('w', c_uint32), ('h', c_uint32), ('d', c_uint32)]
class SRR(Structure):   _fields_ = [('aspect', c_uint32), ('bm', c_uint32), ('lc', c_uint32),
                                    ('bl', c_uint32), ('lyc', c_uint32)]
class ImgCI(Structure):
    _fields_ = [('s', c_uint32), ('p', c_void_p), ('f', c_uint32), ('it', c_uint32), ('fmt', c_uint32),
                ('ext', Ext), ('mips', c_uint32), ('layers', c_uint32), ('samples', c_uint32),
                ('tiling', c_uint32), ('usage', c_uint32), ('sharing', c_uint32),
                ('qfic', c_uint32), ('pqfi', c_void_p), ('initLayout', c_uint32)]
class MemReq(Structure): _fields_ = [('size', c_uint64), ('align', c_uint64), ('bits', c_uint32)]
class MAI(Structure):    _fields_ = [('s', c_uint32), ('p', c_void_p), ('size', c_uint64), ('idx', c_uint32)]
class MemType(Structure):_fields_ = [('flags', c_uint32), ('heap', c_uint32)]
class MemHeap(Structure):_fields_ = [('size', c_uint64), ('flags', c_uint32)]
class MemProps(Structure):
    _fields_ = [('tc', c_uint32), ('types', MemType * 32), ('hc', c_uint32), ('heaps', MemHeap * 16)]
class IVCI(Structure):
    _fields_ = [('s', c_uint32), ('p', c_void_p), ('f', c_uint32), ('img', c_uint64), ('vt', c_uint32),
                ('fmt', c_uint32), ('r', c_uint32), ('g', c_uint32), ('b', c_uint32), ('a', c_uint32), ('sr', SRR)]

mp = MemProps(); vk.vkGetPhysicalDeviceMemoryProperties(pd, byref(mp))
total_vram = [0]
def mkimg(fmt, w, h, usage_of):
    fi = OFIFI(s=1000464002, p=None, usage=usage_of)
    ic = ImgCI(s=14, p=cast(pointer(fi), c_void_p), f=0, it=1, fmt=fmt, ext=Ext(w, h, 1),
               mips=1, layers=1, samples=1, tiling=0, usage=(1 << 2) | (1 << 3),
               sharing=0, qfic=0, pqfi=None, initLayout=0)          # SAMPLED | STORAGE
    img = c_uint64()
    chk("createImage(fmt=%d,of_usage=%d)" % (fmt, usage_of), vk.vkCreateImage(dev, byref(ic), None, byref(img)))
    mr = MemReq(); vk.vkGetImageMemoryRequirements(dev, img, byref(mr))
    idx = next(i for i in range(mp.tc) if (mr.bits >> i) & 1 and mp.types[i].flags & 1)
    mem = c_uint64(); mai = MAI(s=5, p=None, size=mr.size, idx=idx)
    chk("allocMem", vk.vkAllocateMemory(dev, byref(mai), None, byref(mem)))
    chk("bindImage", vk.vkBindImageMemory(dev, img, mem, 0))
    iv = IVCI(s=15, p=None, f=0, img=img.value, vt=1, fmt=fmt, r=0, g=0, b=0, a=0, sr=SRR(1, 0, 1, 0, 1))
    view = c_uint64(); chk("createView", vk.vkCreateImageView(dev, byref(iv), None, byref(view)))
    total_vram[0] += mr.size
    return img.value, view.value, mr.size

inp_i, inp_v, s1 = mkimg(FMT_BGRA, W, H, 1)
ref_i, ref_v, _  = mkimg(FMT_BGRA, W, H, 1)
print("input/reference images: %dx%d B8G8R8A8_UNORM, %.2f MB each" % (W, H, s1 / 1048576))

class OFSCI(Structure):
    _fields_ = [('s', c_uint32), ('p', c_void_p), ('w', c_uint32), ('h', c_uint32),
                ('imageFormat', c_uint32), ('flowVectorFormat', c_uint32), ('costFormat', c_uint32),
                ('outputGridSize', c_uint32), ('hintGridSize', c_uint32),
                ('perfLevel', c_uint32), ('flags', c_uint32)]
class OFEI(Structure): _fields_ = [('s', c_uint32), ('p', c_void_p), ('f', c_uint32), ('rc', c_uint32), ('pr', c_void_p)]
class CPCI(Structure): _fields_ = [('s', c_uint32), ('p', c_void_p), ('f', c_uint32), ('qfi', c_uint32)]
class CBAI(Structure): _fields_ = [('s', c_uint32), ('p', c_void_p), ('pool', c_uint64), ('lvl', c_uint32), ('cnt', c_uint32)]
class CBBI(Structure): _fields_ = [('s', c_uint32), ('p', c_void_p), ('f', c_uint32), ('inh', c_void_p)]
class QPCI(Structure): _fields_ = [('s', c_uint32), ('p', c_void_p), ('f', c_uint32), ('qt', c_uint32), ('cnt', c_uint32), ('stats', c_uint32)]
class IMB(Structure):
    _fields_ = [('s', c_uint32), ('p', c_void_p), ('src', c_uint32), ('dst', c_uint32),
                ('old', c_uint32), ('new', c_uint32), ('sq', c_uint32), ('dq', c_uint32),
                ('img', c_uint64), ('sr', SRR)]
class SI(Structure):
    _fields_ = [('s', c_uint32), ('p', c_void_p), ('wsc', c_uint32), ('pws', c_void_p), ('pwdsm', c_void_p),
                ('cbc', c_uint32), ('pcb', POINTER(c_void_p)), ('ssc', c_uint32), ('pss', c_void_p)]

pool = c_uint64(); chk("cmdPool", vk.vkCreateCommandPool(dev, byref(CPCI(s=39, p=None, f=2, qfi=OFQF)), None, byref(pool)))
cb = c_void_p(); chk("allocCB", vk.vkAllocateCommandBuffers(dev, byref(CBAI(s=40, p=None, pool=pool.value, lvl=0, cnt=1)), byref(cb)))
qq = c_void_p(); vk.vkGetDeviceQueue(dev, OFQF, 0, byref(qq))
ITER = 16   # G13: more than ~16-18 executes in one command buffer loses the device
TSP = 1.0   # timestampPeriod (ns/tick); vulkaninfo reports 1 on this driver
qp = c_uint64(); chk("queryPool", vk.vkCreateQueryPool(dev, byref(QPCI(s=11, p=None, f=0, qt=2, cnt=2 * ITER, stats=0)), None, byref(qp)))

print()
print("  %-7s %-8s %-12s %-10s %-9s %-9s" % ("perf", "outGrid", "flowRes", "median ms", "min ms", "p95 ms"))
print("  " + "-" * 62)
for perf, pn in [(1, "SLOW"), (2, "MEDIUM"), (3, "FAST")]:
    for grid, gn in [(4, "4x4"), (2, "2x2"), (1, "1x1")]:
        fw, fh = (W + grid - 1) // grid, (H + grid - 1) // grid
        fi2, fv2, fsz = mkimg(FMT_S105, fw, fh, 2)
        sci = OFSCI(s=1000464004, p=None, w=W, h=H, imageFormat=FMT_BGRA,
                    flowVectorFormat=FMT_S105, costFormat=0,
                    outputGridSize=grid, hintGridSize=grid, perfLevel=perf, flags=0)
        sess = c_uint64()
        r = vkCreateOFSession(dev, byref(sci), None, byref(sess))
        if r != 0:
            print("  %-7s %-8s vkCreateOpticalFlowSessionNV FAILED VkResult %d" % (pn, gn, r)); continue
        ok = True
        for bp, v in [(1, inp_v), (2, ref_v), (4, fv2)]:
            rr = vkBindOFImage(dev, sess, bp, v, 1)   # VK_IMAGE_LAYOUT_GENERAL
            if rr != 0:
                print("  %-7s %-8s bind bindingPoint=%d FAILED %d" % (pn, gn, bp, rr)); ok = False
        if not ok:
            vkDestroyOFSession(dev, sess, None); continue
        vk.vkResetCommandBuffer(cb, 0)
        vk.vkBeginCommandBuffer(cb, byref(CBBI(s=42, p=None, f=0, inh=None)))
        vk.vkCmdResetQueryPool(cb, qp, 0, 2 * ITER)
        bars = (IMB * 3)(*[IMB(s=45, p=None, src=0, dst=0, old=0, new=1,
                               sq=0xffffffff, dq=0xffffffff, img=i, sr=SRR(1, 0, 1, 0, 1))
                           for i in (inp_i, ref_i, fi2)])
        vk.vkCmdPipelineBarrier(cb, 0x10000, 0x10000, 0, 0, None, 0, None, 3, bars)
        ei = OFEI(s=1000464005, p=None, f=0, rc=0, pr=None)
        for k in range(ITER):
            vk.vkCmdWriteTimestamp(cb, 1, qp, 2 * k)          # TOP_OF_PIPE
            vkCmdOFExecute(cb, sess, byref(ei))
            vk.vkCmdWriteTimestamp(cb, 0x2000, qp, 2 * k + 1)  # BOTTOM_OF_PIPE
            vk.vkCmdPipelineBarrier(cb, 0x10000, 0x10000, 0, 0, None, 0, None, 0, None)
        vk.vkEndCommandBuffer(cb)
        si = SI(s=4, p=None, wsc=0, pws=None, pwdsm=None, cbc=1, pcb=pointer(cb), ssc=0, pss=None)
        rs = vk.vkQueueSubmit(qq, 1, byref(si), 0)
        rw = vk.vkQueueWaitIdle(qq)
        res = (c_uint64 * (2 * ITER))()
        vk.vkGetQueryPoolResults.argtypes = [c_void_p, c_uint64, c_uint32, c_uint32,
                                             c_size_t, c_void_p, c_uint64, c_uint32]
        vk.vkGetQueryPoolResults.restype = c_int
        rq = vk.vkGetQueryPoolResults(dev, qp, 0, 2 * ITER, sizeof(res), res,
                                      8, 1 | 2)   # 64BIT | WAIT
        if rs != 0 or rw != 0 or rq != 0:
            print("  %-7s %-8s submit=%d waitIdle=%d queryResults=%d raw[0..3]=%s"
                  % (pn, gn, rs, rw, rq, [res[i] for i in range(4)]))
        ts = sorted((res[2 * k + 1] - res[2 * k]) * TSP / 1e6 for k in range(ITER))
        print("  %-7s %-8s %4dx%-7d %-10.3f %-9.3f %-9.3f" %
              (pn, gn, fw, fh, ts[ITER // 2], ts[0], ts[int(ITER * 0.95)]))
        vkDestroyOFSession(dev, sess, None)
print("\ntotal VRAM allocated by probe: %.2f MB" % (total_vram[0] / 1048576))
