"""Local Chrome refresh/cadence rig; the owner opens and starts it in the browser.

No browser input, display-mode changes or window positioning is performed here.
The optional native engine is owned by this process and always stopped after a run.
"""
import argparse
import shlex
import ctypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
ENGINE = Path(os.environ.get('NSP_ENGINE') or ROOT / 'prototype/silkplay.exe').resolve()
PAGE = r'''<!doctype html><meta charset="utf-8"><title>NSP 165Hz probe</title>
<style>
body{background:#151515;color:#eee;font:16px Segoe UI;margin:24px}
button,select{font:inherit;padding:8px;margin:6px}video{display:block;width:960px;height:540px;object-fit:contain;background:#000}
:fullscreen body{margin:0}:fullscreen #panel{display:none}:fullscreen video{width:100vw;height:100vh}
pre{white-space:pre-wrap}
</style>
<div id="panel"><h1>Chrome 165 Hz cadence probe</h1>
<select id="source"><option value="24">CFR 24</option><option value="30">CFR 30</option><option value="60">CFR 60</option><option value="bbb30">BBB 720p30</option><option value="meridian30">Meridian 1080p29.97</option></select>
<select id="arm"><option value="page">Page only</option><option value="ours">Current Nova engine</option></select>
<button id="run">Run 30 seconds fullscreen</button><pre id="status">Ready</pre></div>
<video id="video" muted playsinline loop></video>
<script>
const v=document.querySelector('video'), status=document.querySelector('#status');
let active=false, raf=[], rvfc=[], errors=[], start=0, last=0, frameHandle;
function screenInfo(){return {width:screen.width,height:screen.height,availLeft:screen.availLeft,availTop:screen.availTop,
  x:screenX,y:screenY,dpr:devicePixelRatio,innerWidth,innerHeight,fullscreen:!!document.fullscreenElement};}
status.textContent=JSON.stringify(screenInfo(),null,2);
function frame(now,meta){if(active)rvfc.push({now,mediaTime:meta.mediaTime,expectedDisplayTime:meta.expectedDisplayTime,presentedFrames:meta.presentedFrames});frameHandle=v.requestVideoFrameCallback(frame);}
v.requestVideoFrameCallback(frame);
function tick(t){if(active)raf.push(t);requestAnimationFrame(tick);}requestAnimationFrame(tick);
async function post(path,body){const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const data=await r.json();if(!r.ok)throw Error(data.error||r.status);return data;}
document.querySelector('#run').onclick=async()=>{
  const source=document.querySelector('#source').value, arm=document.querySelector('#arm').value;
  document.querySelector('#run').disabled=true; errors=[];
  try{
    v.src='/media/'+source; await v.play(); await document.documentElement.requestFullscreen();
    await new Promise(r=>setTimeout(r,1000));
    const qualityBefore=v.getVideoPlaybackQuality();
    const opened=await post('/start',{source,arm,screen:screenInfo()});
    raf=[];rvfc=[];start=performance.now();active=true;
    await new Promise(r=>setTimeout(r,30000));active=false;
    const result={...opened,source,arm,elapsedMs:performance.now()-start,screen:screenInfo(),
      qualityBefore:{total:qualityBefore.totalVideoFrames,dropped:qualityBefore.droppedVideoFrames},
      qualityAfter:v.getVideoPlaybackQuality().toJSON?.()||{totalVideoFrames:v.getVideoPlaybackQuality().totalVideoFrames,droppedVideoFrames:v.getVideoPlaybackQuality().droppedVideoFrames},
      raf,rvfc,errors};
    v.pause();const done=await post('/finish',result);await document.exitFullscreen();
    status.textContent=JSON.stringify(done,null,2);
  }catch(e){active=false;v.pause();status.textContent=String(e);await post('/abort',{error:String(e)}).catch(err=>status.textContent+='; abort: '+err);
    if(document.fullscreenElement)await document.exitFullscreen();
  }finally{document.querySelector('#run').disabled=false;}
};
v.addEventListener('error',()=>errors.push({time:performance.now(),code:v.error?.code,message:v.error?.message}));
</script>'''


def prepare_media(out):
    import cv2
    cv2.setNumThreads(2)
    for name, clip, fps in (('bbb30', 'R3_bbb60_720crop', 30), ('meridian30', 'R2_meridian', 30000 / 1001)):
        target = out / (name + '.webm')
        if target.exists():
            print(f'Keeping existing source: {target}', flush=True)
            continue
        frames = sorted((ROOT / 'harness/corpus' / clip).glob('*.png'))[::2]
        if not frames:
            raise RuntimeError(f'missing corpus: {clip}')
        first = cv2.imread(str(frames[0]))
        if first is None:
            raise RuntimeError(f'unreadable source: {frames[0]}')
        h, w = first.shape[:2]
        writer = cv2.VideoWriter(str(target), cv2.VideoWriter_fourcc(*'VP80'), fps, (w, h))
        if not writer.isOpened():
            raise RuntimeError(f'could not create {target}')
        try:
            for path in frames:
                frame = cv2.imread(str(path))
                if frame is None or frame.shape != first.shape:
                    raise RuntimeError(f'invalid source frame: {path}')
                writer.write(frame)
        finally:
            writer.release()
        print(f'{name}: {w}x{h}, {len(frames)} frames, {fps:.5f} fps', flush=True)


class Rig:
    def __init__(self, out, engine_args=()):
        self.out = out
        self.engine_args = list(engine_args)
        self.process = self.log = self.name = None
        self.watchdog = None
        self.lock = threading.Lock()

    def stop(self):
        if self.watchdog is not None:
            self.watchdog.cancel()
            self.watchdog = None
        if self.process is not None:
            if self.process.poll() is None:
                kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel.OpenEventW.restype = ctypes.c_void_p
                kernel.OpenEventW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
                kernel.SetEvent.argtypes = kernel.CloseHandle.argtypes = [ctypes.c_void_p]
                handle = kernel.OpenEventW(2, False, 'Local\\NovaSilkPlay.Quit')
                if not handle:
                    raise ctypes.WinError(ctypes.get_last_error())
                try:
                    if not kernel.SetEvent(handle):
                        raise ctypes.WinError(ctypes.get_last_error())
                finally:
                    kernel.CloseHandle(handle)
                try:
                    self.process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self.process.terminate()
                    self.process.wait(timeout=5)
                    raise RuntimeError('owned engine did not honor graceful quit')
            self.process = None
        if self.log is not None:
            self.log.close()
            self.log = None

    def start(self, data):
        if self.name is not None:
            raise RuntimeError('another measurement is active')
        if data['arm'] not in ('ours', 'page'):
            raise ValueError('unknown arm')
        if data['source'] not in ('24', '30', '60', 'bbb30', 'meridian30'):
            raise ValueError('unknown source')
        self.name = time.strftime('%Y%m%d-%H%M%S') + '-' + data['source'] + '-' + data['arm']
        if data['arm'] == 'ours':
            self.log = (self.out / (self.name + '-engine.log')).open('w', encoding='utf-8')
            command = [str(ENGINE), '--default-settings',
                       '--target-title', 'NSP 165Hz probe', '--stats-every', '2', '--fullscreen-only', '--no-badge',
                       '--occlusion-ignore', 'codex-computer-use.exe'] + self.engine_args
            self.process = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT,
                                            creationflags=subprocess.CREATE_NO_WINDOW, cwd=ROOT / 'prototype')
            time.sleep(0.5)
            if self.process.poll() is not None:
                raise RuntimeError(f'engine exited early ({self.process.returncode}); inspect {self.name}-engine.log')
            run = self.name
            def expire():
                with self.lock:
                    if self.name == run:
                        self.stop()
                        (self.out / (run + '-aborted.json')).write_text(
                            json.dumps({'error': 'browser did not finish within 60 seconds'}), encoding='utf-8')
                        self.name = None
            self.watchdog = threading.Timer(60, expire)
            self.watchdog.daemon = True
            self.watchdog.start()
        return {'run': self.name}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', required=True)
    ap.add_argument('--port', type=int, default=8766)
    ap.add_argument('--engine-args', default='', help='extra lab engine options; synthetic muted sources need --scout-always')
    ap.add_argument('--prepare-media', action='store_true', help='encode short corpus loops if absent; requires OpenCV')
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.prepare_media:
        prepare_media(out)
    rig = Rig(out, shlex.split(args.engine_args))
    media = {str(fps): ROOT / f'tools/yt-check/generated/cfr_{fps}.webm' for fps in (24, 30, 60)}
    media.update({'bbb30': out / 'bbb30.webm', 'meridian30': out / 'meridian30.webm'})

    class Handler(BaseHTTPRequestHandler):
        def reply(self, code, body, content_type='application/json'):
            if not isinstance(body, bytes):
                body = json.dumps(body).encode()
            self.send_response(code)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == '/':
                return self.reply(200, PAGE.encode(), 'text/html; charset=utf-8')
            target = media.get(path.removeprefix('/media/')) if path.startswith('/media/') else None
            if target is None or not target.is_file():
                return self.reply(404, {'error': 'not found'})
            self.reply(200, target.read_bytes(), 'video/webm')

        def do_POST(self):
            try:
                data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                with rig.lock:
                    if self.path == '/start':
                        return self.reply(200, rig.start(data))
                    if self.path == '/finish':
                        if not rig.name or data.get('run') != rig.name:
                            raise ValueError('run identity mismatch')
                        (out / (rig.name + '-page.json')).write_text(json.dumps(data, indent=2), encoding='utf-8')
                        rig.stop()
                        name, rig.name = rig.name, None
                        return self.reply(200, {'saved': name, 'rafCount': len(data['raf']), 'videoCount': len(data['rvfc']),
                                                'screen': data['screen'], 'qualityBefore': data['qualityBefore'], 'qualityAfter': data['qualityAfter']})
                    if self.path == '/abort':
                        rig.stop()
                        rig.name = None
                        return self.reply(200, {'aborted': data})
                    self.reply(404, {'error': 'unknown endpoint'})
            except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError) as exc:
                self.reply(500, {'error': str(exc)})

    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print(f'http://127.0.0.1:{args.port}/ -> {out}', flush=True)
    try:
        server.serve_forever()
    finally:
        rig.stop()
        server.server_close()


if __name__ == '__main__':
    main()
