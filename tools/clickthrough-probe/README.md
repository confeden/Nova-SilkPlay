# clickthrough-probe — does real mouse input pass through the overlay?

Settles a question the prototype's own notes contradicted each other on. For each
window-style configuration it creates an overlay the way `prototype/nsp_overlay.cpp`
does (DComp visual + composition swapchain, `WS_EX_NOREDIRECTIONBITMAP`) over the
middle of the test page, fills it with magenta, and then:

* reads the composed desktop through DXGI Desktop Duplication — a click-through
  window that is not on screen proves nothing (`onscr`);
* reads the swapchain's composition mode (`plane`);
* sends real `SendInput` moves, a left click and a wheel notch, and reads what the
  PAGE received from its title (`testmotion.html?input=1`) (`page d/u/m/w`);
* counts `WM_NCHITTEST`, mouse and `WM_SETCURSOR` messages the overlay got;
* for the non-topmost variant, checks the overlay is still above Chrome after the
  click activated it.

It moves the real cursor and clicks the test page; cursor position and the
foreground window are restored at the end. Open the page on the secondary monitor.

```cmd
build.cmd
python ..\..\prototype\run_testpage.py --url "file:///.../testmotion.html?input=1"
clickthrough_probe.exe [--title SUBSTR] [--only CONFIG]
```

## Result on the dev machine (2026-09-13, build 26100, 60 Hz secondary)

```
config          onscr   plane     WFP      page d/u/m/w  NCHIT  mouse  setcur above
no-overlay      no      -         chrome   +1/+1/+5/+1   0      0      0      -
current         MAGENTA COMPOSED  chrome   +0/+0/+0/+0   38     0      0      yes
current-noHT    MAGENTA COMPOSED  OVERLAY  +0/+0/+0/+0   13     12     11     yes
layered         MAGENTA COMPOSED  chrome   +1/+1/+4/+1   0      0      0      yes
layered-noHT    MAGENTA COMPOSED  chrome   +1/+1/+4/+1   0      0      0      yes
layered-noattr  MAGENTA COMPOSED  chrome   +1/+1/+4/+1   0      0      0      yes
layered-ztrack  MAGENTA COMPOSED  chrome   +1/+1/+4/+1   0      0      0      NO
```

* `WS_EX_TRANSPARENT` without `WS_EX_LAYERED` ("current", the prototype until this
  run) swallows everything; answering `HTTRANSPARENT` only forwards to windows of
  the same thread, and Chrome's is not one.
* `WS_EX_LAYERED | WS_EX_TRANSPARENT` passes everything and the overlay is still on
  screen, with or without `SetLayeredWindowAttributes`.
* `WindowFromPoint` answers "chrome" for the swallowing configuration too — it is
  worthless as a click-through test. The hit-test count is the test.
* A non-topmost overlay ends up below Chrome once a click activates Chrome, which
  is why `Overlay::KeepAbove` runs every loop iteration.
* `plane` reads COMPOSED for every variant here, a static magenta clear presented
  with `Present(1,0)`; the prototype with real content reports OVERLAY with and
  without LAYERED, so LAYERED does not cost the plane.

The `fg ms` column of the raw output is not meaningful: the probe moves the
foreground to the shell before each configuration, and that event is the first
one recorded.
