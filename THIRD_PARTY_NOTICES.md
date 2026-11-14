# Third-party notices

Nova SilkPlay's own code is licensed under GPL-3.0-only; see [LICENSE](LICENSE).
The installer displays the same GPLv3 text. Earlier beta-only restrictions are superseded.

## NVIDIA Optical Flow SDK interface headers

`third_party/nvofapi/nvOpticalFlowCommon.h` and `nvOpticalFlowD3D11.h` retain their
NVIDIA copyright and MIT license notices verbatim. See that directory's README
for provenance. The NVIDIA driver runtime is loaded from Windows and is not bundled.

## Magpie design reference

[Magpie](https://github.com/Blinue/Magpie), GPLv3, was reviewed at commit
`241e0a447d1c45fc95781f9cd4b5a9198e738a3b`. Its `AutoStartHelper.cpp` informed the
use of `FOLDERID_Startup` and `IShellLink`; the implementation here is independent,
checks each operation, preserves Task Manager disables and working shortcut targets,
and migrates the old Run entry. No Magpie source, shaders, models or binaries are bundled.
See [the comparison](docs/magpie-comparison.md) for scope and rejected transfers.

## NVIDIA header license text

The Common header is Copyright (c) 2018-2023 NVIDIA Corporation; the D3D11 header
is Copyright (c) 2019-2023 NVIDIA Corporation. Both carry the following terms.

```text
This copyright notice applies to this header file only:

Copyright (c) 2019-2023 NVIDIA Corporation

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the software, and to permit persons to whom the
software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```

## Optional learned-VFI research dependencies

`harness/learned_probe.py` evaluates external local RIFE (MIT), NeuFlow v2
(Apache-2.0), NVIDIA nvidia-vfx/TensorRT-RTX (proprietary runtimes), and
FrameFusion (Apache-2.0 code; CC BY-NC 4.0 provided weights). These dependencies
and weights remain in ignored local directories and are not bundled in the
installer or copied into the project source. See docs/learned-vfi-evaluation.md
for provenance, measured results and limits. Successful local evaluation does
not establish a right to redistribute a third-party runtime or model.
