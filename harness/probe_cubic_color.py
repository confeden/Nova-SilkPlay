"""Build an isolated P20 Catmull-Rom color-fetch candidate from fresh native objects.

Only PSWarp's two final color fetches change; costs, fields, cuts and endpoints stay
identical. Shipping sources/executable are untouched. Build prototype first, then run
through res.py. Score with NSP_ENGINE pointing at the printed candidate executable.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess


CUBIC = r'''
// Exact separable Catmull-Rom using nine bilinear samples in linear light.
float3 CubicColor(Texture2D<float4> image, float2 uv) {
    float2 pixel = uv / gInvSize - 0.5;
    float2 base = floor(pixel);
    float2 f = pixel - base;
    float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = -0.5 * f + f2 - 0.5 * f3;
    float2 w1 = 1.0 - 2.5 * f2 + 1.5 * f3;
    float2 w2 = 0.5 * f + 2.0 * f2 - 1.5 * f3;
    float2 w3 = -0.5 * f2 + 0.5 * f3;
    float2 middle = w1 + w2;
    float2 p0 = (base - 0.5) * gInvSize;
    float2 p1 = (base + 0.5 + w2 / middle) * gInvSize;
    float2 p2 = (base + 2.5) * gInvSize;
    float3 color = 0.0;
    color += image.SampleLevel(smpLin, float2(p0.x, p0.y), 0).rgb * w0.x * w0.y;
    color += image.SampleLevel(smpLin, float2(p1.x, p0.y), 0).rgb * middle.x * w0.y;
    color += image.SampleLevel(smpLin, float2(p2.x, p0.y), 0).rgb * w3.x * w0.y;
    color += image.SampleLevel(smpLin, float2(p0.x, p1.y), 0).rgb * w0.x * middle.y;
    color += image.SampleLevel(smpLin, float2(p1.x, p1.y), 0).rgb * middle.x * middle.y;
    color += image.SampleLevel(smpLin, float2(p2.x, p1.y), 0).rgb * w3.x * middle.y;
    color += image.SampleLevel(smpLin, float2(p0.x, p2.y), 0).rgb * w0.x * w3.y;
    color += image.SampleLevel(smpLin, float2(p1.x, p2.y), 0).rgb * middle.x * w3.y;
    color += image.SampleLevel(smpLin, float2(p2.x, p2.y), 0).rgb * w3.x * w3.y;
    return saturate(color);
}

'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    prototype = root / 'prototype'
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (prototype / 'nsp_synth.cpp').read_text(encoding='utf-8')
    start = source.index('float4 PSWarp(VSOut i)')
    end = source.index('// --------------------------------------------------------- warp laboratory', start)
    warp = source[start:end]
    for texture, uv in (('texA', 'uvA'), ('texB', 'uvB')):
        old = f'{texture}.SampleLevel(smpLin, {uv}, 0).rgb'
        if warp.count(old) != 1:
            raise RuntimeError(f'color-fetch anchor changed: {old}')
        warp = warp.replace(old, f'CubicColor({texture}, {uv})')
    candidate = source[:start] + CUBIC + warp + source[end:]
    (out / 'nsp_synth_cubic.cpp').write_text(candidate, encoding='utf-8')
    sources = re.search(r'set "SOURCES=([^"]+)"',
                        (prototype / 'build.cmd').read_text(encoding='utf-8')).group(1).split()
    objects = []
    for name in sources:
        obj = prototype / 'obj' / Path(name).with_suffix('.obj')
        if not obj.exists() or obj.stat().st_mtime < (prototype / name).stat().st_mtime:
            raise RuntimeError(f'build prototype first: missing/stale {obj}')
        if name != 'nsp_synth.cpp':
            objects.append(obj)
    resource = prototype / 'obj/silkplay.res'
    if not resource.exists():
        raise RuntimeError('build prototype first: missing resource')
    command = (
        '@echo off\n'
        'call "D:\\Programs\\Microsoft\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
        'if errorlevel 1 exit /b 1\n'
        f'cl /nologo /EHsc /std:c++20 /W4 /bigobj /O2 /utf-8 /I"{prototype}" '
        '/c nsp_synth_cubic.cpp /Fo:nsp_synth_cubic.obj\n'
        'if errorlevel 1 exit /b 1\n'
        'link /nologo /SUBSYSTEM:WINDOWS /OUT:silkplay-cubic.exe nsp_synth_cubic.obj '
        + ' '.join(f'"{p}"' for p in objects + [resource])
        + ' d3d11.lib dxgi.lib dcomp.lib dwmapi.lib user32.lib dxguid.lib d3dcompiler.lib '
          'windowscodecs.lib bcrypt.lib windowsapp.lib shell32.lib comctl32.lib gdiplus.lib '
          'shcore.lib ole32.lib gdi32.lib pdh.lib advapi32.lib\nexit /b %errorlevel%\n')
    build = out / 'build.cmd'
    build.write_text(command, encoding='utf-8')
    with (out / 'build.log').open('w', encoding='utf-8') as log:
        subprocess.run(['cmd', '/c', str(build)], cwd=out, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    for path in (prototype / 'silkplay.exe', out / 'silkplay-cubic.exe'):
        print(f'{path}: SHA256 {hashlib.sha256(path.read_bytes()).hexdigest()}')


if __name__ == '__main__':
    main()
