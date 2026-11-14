"""Build/run the real overlay with injected Present1 failures, without visible UI.

Fault injection is confined to an output copy; shipping sources/binary are untouched.
Run through the shared build queue, e.g. res.py run --shell powershell --
"python prototype/tests/run_present_fault_test.py --out <absolute directory>".
"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    prototype = Path(__file__).resolve().parents[1]
    source = (prototype / 'nsp_overlay.cpp').read_text(encoding='utf-8')
    call = 'const HRESULT hr = d.swap->Present1(0, 0, &pp);'
    if source.count(call) != 1:
        raise RuntimeError('Present1 injection anchor changed; review test before running')
    instrumented = '#include <windows.h>\nHRESULT g_presentFault = S_OK;\n' + source.replace(
        call, 'const HRESULT hr = FAILED(g_presentFault) ? g_presentFault : d.swap->Present1(0, 0, &pp);')
    (out / 'overlay_injected.cpp').write_text(instrumented, encoding='utf-8')
    build = out / 'build.cmd'
    build.write_text(
        '@echo off\n'
        'call "D:\\Programs\\Microsoft\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
        'if errorlevel 1 exit /b 1\n'
        f'cl /nologo /EHsc /std:c++20 /W4 /O2 /utf-8 /I"{prototype}" '
        f'"{prototype / "tests/present_fault_test.cpp"}" overlay_injected.cpp '
        f'"{prototype / "nsp_common.cpp"}" /Fe:present_fault_test.exe '
        '/link d3d11.lib dxgi.lib dcomp.lib dwmapi.lib user32.lib gdi32.lib '
        'shell32.lib ole32.lib psapi.lib\n'
        'exit /b %errorlevel%\n', encoding='utf-8')
    with (out / 'build.log').open('w', encoding='utf-8') as log:
        subprocess.run(['cmd', '/c', str(build)], cwd=out, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    subprocess.run([str(out / 'present_fault_test.exe')], cwd=out, check=True)


if __name__ == '__main__':
    main()
