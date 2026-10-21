# Workers Tracking Native

## Native foundations

The C++20 CMake/MSVC project currently includes:

- `worker_common`: the bounded binary protocol and incremental stream decoder.
- `worker_capture`: validated owned BGRA frames, DXGI Desktop Duplication capture,
  GDI fallback capture, backend coordination and recovery, change detection, a
  latest-frame queue, capture profiles, and WIC scaling/JPEG encoding.
- `WorkerAgent`: links the capture pipeline but intentionally does not start a
  continuous capture loop yet.
- `Observer`: remains a stub executable.

The protocol uses a fixed 12-byte, network-byte-order header. Each complete
packet, including its header, is capped at 8 MiB; control payloads and UTF-8
control strings use tighter limits.

The Grid profile limits output width to 480 pixels, uses JPEG quality 60, and
defines adaptive scheduling bounds of 250-500 ms. The Focus profile limits
output to 1280x720, uses JPEG quality 75, and defines a 250 ms interval. Stage 2
applies only the image dimensions and quality; scheduling is not active. WIC
clamps all requested JPEG quality values to the documented inclusive range
1-100 and never upscales.

Capture is limited to the primary monitor and requires an interactive desktop.
DXGI is preferred and GDI is used when DXGI is unavailable or recovering.
Capture is unavailable or may fail while the session is locked, on the secure
UAC desktop, in non-interactive service sessions, or when desktop duplication
is not supported. Secure desktop content is never captured.

Networking, Observer UI, tray/configuration/autostart, archives, and load
simulation are outside the current stage.

From PowerShell:

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
& $cmake --build build --config Debug
& $cmake --build build --config Release
& "$([IO.Path]::GetDirectoryName($cmake))\ctest.exe" --test-dir build -C Debug --output-on-failure
& "$([IO.Path]::GetDirectoryName($cmake))\ctest.exe" --test-dir build -C Release --output-on-failure
```
