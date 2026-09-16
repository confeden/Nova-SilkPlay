"""Independent re-probe of D3D12 ID3D12VideoMotionEstimator support on GB206."""
from ctypes import *
from ctypes import wintypes

d3d12 = WinDLL("d3d12"); dxgi = WinDLL("dxgi")

class GUID(Structure):
    _fields_=[('d1',c_uint32),('d2',c_uint16),('d3',c_uint16),('d4',c_uint8*8)]
def guid(a,b,c,*d): return GUID(a,b,c,(c_uint8*8)(*d))

IID_ID3D12Device      = guid(0x189819f1,0x1db6,0x4b57,0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7)
IID_ID3D12VideoDevice = guid(0x1F052807,0x0B46,0x4ACC,0x8A,0x89,0x36,0x4F,0x79,0x37,0x18,0xA4)

dev = c_void_p()
d3d12.D3D12CreateDevice.restype = c_long
# D3D_FEATURE_LEVEL_11_0 = 0xb000
hr = d3d12.D3D12CreateDevice(None, 0xb000, byref(IID_ID3D12Device), byref(dev))
print("D3D12CreateDevice -> hr=0x%08x" % (hr & 0xffffffff))
if hr != 0: raise SystemExit(1)

vt = cast(dev, POINTER(POINTER(c_void_p))).contents
QI = CFUNCTYPE(c_long, c_void_p, POINTER(GUID), POINTER(c_void_p))(vt[0])
vdev = c_void_p()
hr = QI(dev, byref(IID_ID3D12VideoDevice), byref(vdev))
print("QueryInterface(ID3D12VideoDevice) -> hr=0x%08x" % (hr & 0xffffffff))
if hr != 0: raise SystemExit(1)

vvt = cast(vdev, POINTER(POINTER(c_void_p))).contents
CheckFeature = CFUNCTYPE(c_long, c_void_p, c_int, c_void_p, c_uint32)(vvt[3])

class SizeRange(Structure): _fields_=[('MaxW',c_uint32),('MaxH',c_uint32),('MinW',c_uint32),('MinH',c_uint32)]
class FDME(Structure):
    _fields_=[('NodeIndex',c_uint32),('InputFormat',c_uint32),
              ('BlockSizeFlags',c_uint32),('PrecisionFlags',c_uint32),('SizeRange',SizeRange)]
class FDMES(Structure):
    _fields_=[('NodeIndex',c_uint32),('InputFormat',c_uint32),('BlockSize',c_int),('Precision',c_int),
              ('SizeRange',SizeRange),('Protected',c_int32),
              ('MVHeapL0',c_uint64),('MVHeapL1',c_uint64),('MEL0',c_uint64),('MEL1',c_uint64)]

FMTS = {103:'NV12', 87:'B8G8R8A8_UNORM', 28:'R8G8B8A8_UNORM', 61:'R8_UNORM',
        108:'P010', 24:'R10G10B10A2_UNORM', 10:'R16G16B16A16_FLOAT'}
BS = {0:'NONE',1:'8x8',2:'16x16',3:'8x8|16x16'}
print("\nD3D12_FEATURE_VIDEO_MOTION_ESTIMATOR (20):")
for f,name in FMTS.items():
    d = FDME(NodeIndex=0, InputFormat=f)
    hr = CheckFeature(vdev, 20, byref(d), sizeof(d))
    print("  %-20s hr=0x%08x BlockSizeFlags=%s(%d) PrecisionFlags=%d Size %dx%d..%dx%d"%(
        name, hr & 0xffffffff, BS.get(d.BlockSizeFlags,'?'), d.BlockSizeFlags, d.PrecisionFlags,
        d.SizeRange.MinW, d.SizeRange.MinH, d.SizeRange.MaxW, d.SizeRange.MaxH))

print("\nD3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_SIZE (21), NV12 2560x1440:")
for bs,bsn in [(0,'8x8'),(1,'16x16')]:
    d = FDMES(NodeIndex=0, InputFormat=103, BlockSize=bs, Precision=0,
              SizeRange=SizeRange(2560,1440,2560,1440))
    hr = CheckFeature(vdev, 21, byref(d), sizeof(d))
    tot = d.MVHeapL0 + d.MEL0
    print("  block %-5s hr=0x%08x MVHeapL0=%d MVHeapL1=%d MEL0=%d MEL1=%d  total L0=%d B (%.2f MB)"%(
        bsn, hr & 0xffffffff, d.MVHeapL0, d.MVHeapL1, d.MEL0, d.MEL1, tot, tot/1048576))
