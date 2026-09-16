# window-probe — what sits above the browser, and what does it cover?

`zorder_dump.py` walks the top-level z-order from the top down to a target window
and prints, per window: pid, image, class, title, visible/iconic/cloaked, styles and
decoded extended styles, layered attributes, `GetWindowRect`, extended frame
bounds, owner, window region, and the overlap of both rects with the target's
client area. Stdlib Python only.

```cmd
python zorder_dump.py --exe chrome.exe                 # the test page by title
python zorder_dump.py --title "YouTube" --exe chrome.exe --all
```

It is the ground truth behind the occlusion rules in `prototype/nsp_common.cpp`
`VideoRectObscured`. Two things it showed on the dev machine (2026-09-13):

* the owner's maximised Chrome on the 2560x1440 primary: `GetWindowRect`
  `-8,-8 2576x1408`, extended frame bounds `0,0 2560x1392` — `GetWindowRect`
  reaches 8 px into the secondary monitor;
* `quotty.exe` (topmost, `2559,988 386x92`) sits one column over the primary and
  385 px over the bottom-left corner of the secondary; directly above Chrome sit
  its own invisible `IME` / `MSCTFIME UI` windows.
