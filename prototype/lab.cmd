@echo off
rem lab.cmd - run the windowless silkplay.exe from cmd.exe with a console and with cmd waiting.
rem Developer aid only; NOT shipped by installer\silkplay.iss.
rem
rem From bash or a harness script this is unnecessary: `silkplay.exe ... > out.log 2>&1` and
rem subprocess.Popen(stdout=<file or pipe>) both work unchanged, because a GUI-subsystem process
rem inherits std handles exactly like a console one. The one case that differs is a human at a
rem cmd prompt: cmd does not wait for a /SUBSYSTEM:WINDOWS process, so it returns to the prompt
rem while output is still arriving. start /wait /b fixes exactly that, and --console gives the
rem run somewhere to print (main.cpp attaches to the parent console when no handle is inherited).
start "" /wait /b "%~dp0silkplay.exe" --console %*
