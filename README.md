# Workers Tracking Native

## Native foundations

The C++20 CMake/MSVC project currently includes:

- `worker_common`: the bounded binary protocol and incremental stream decoder.
- `worker_capture`: validated owned BGRA frames, DXGI Desktop Duplication capture,
  GDI fallback capture, backend coordination and recovery, change detection, a
  latest-frame queue, capture profiles, and WIC scaling/JPEG encoding.
- `worker_transport`: single-owner agent sessions, finite-timeout Winsock
  transport, reconnect/heartbeat scheduling, adaptive Grid rate control, and a
  composable orchestration boundary with a latest-only encoded-frame handoff.
- `WorkerAgent`: links the capture and transport primitives but intentionally
  does not start a continuous application loop yet.
- `observer_core`: thread-safe agent registry, IOCP server, layout/profile
  routing, WIC JPEG decoding, and local rename persistence.
- `Observer`: a native Win32 Direct2D/DirectWrite monitoring wall.

The protocol uses a fixed 12-byte, network-byte-order header. Each complete
packet, including its header, is capped at 8 MiB; control payloads and UTF-8
control strings use tighter limits. `Hello` carries a fixed 16-byte agent GUID,
an employee name, and a hostname; both identity strings are validated UTF-8 and
limited to 255 encoded bytes.

The Grid profile limits output to 480x720 and adapts between 250-500 ms (4-2
FPS) and JPEG quality 45-65. Overload evidence backs off immediately; four
consecutive healthy samples are required for each cautious recovery step. The
Focus profile is fixed at 1280x720, JPEG quality 75, and 250 ms unless an
Observer `SetProfile` explicitly supplies values, which are clamped to the safe
global bounds of 1-1280 width, 1-720 height, quality 45-75, and 250-500 ms.
WIC still enforces its independent inclusive quality range of 1-100 and never
upscales.

Agent TCP sessions connect only to validated IPv4 endpoints, send `Hello`
before any frame or heartbeat, incrementally decode only inbound `SetProfile`
messages, and detect orderly close. Reads, writes, connection attempts, and
tests use finite timeouts. Reconnect delays are 250, 500, 1000, then 1500 ms
with a 1500 ms cap and reset after the `Hello` handshake. Heartbeats are due
every two seconds. Frames use a capacity-one latest-only handoff; identity,
profile control, and heartbeat traffic are never put in a generic frame queue.

Capture is limited to the primary monitor and requires an interactive desktop.
DXGI is preferred and GDI is used when DXGI is unavailable or recovering.
Capture is unavailable or may fail while the session is locked, on the secure
UAC desktop, in non-interactive service sessions, or when desktop duplication
is not supported. Secure desktop content is never captured.

## Observer

Observer listens on TCP port **45873** by default. Use
`Observer.exe --port <1-65535>` to change the port and
`--bind <IPv4-address>` to change the bind address (the default is `0.0.0.0`).
The server uses overlapped Winsock, `AcceptEx`, and IOCP worker threads; it
supports at most 40 registered agents. A connection must send `Hello` within
three seconds. If the same GUID connects twice, the newest connection
deterministically replaces the old one. Agent-originated `SetProfile` and
`RenameAck` packets are invalid and close the connection.

Controls:

- Double-click a tile to enter Focus; double-click again or press Escape to
  return to Grid.
- Right-click a tile and choose **Rename** to set an Observer-local display
  name. Names are stored atomically under
  `%LOCALAPPDATA%\WorkersTracking\observer_names`; the agent's configured source
  name is not changed.
- Press F11 to toggle TV fullscreen mode. Escape leaves Focus first, then
  fullscreen.

Observer displays the effective employee name, hostname, measured FPS, and
Online/Stale/Offline status. It sends the Grid profile on registration and the
1280x720, 250 ms, JPEG-quality-75 Focus profile only to the selected agent.

This stage is LAN/local use only: it has **no authentication, authorization,
encryption, or Windows Firewall installer rule**. WorkerAgent
tray/configuration/autostart, archives/IFrameSink, load simulation, packaging,
and installer work remain outside this stage.

From PowerShell:

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
& $cmake --build build --config Debug
& $cmake --build build --config Release
& "$([IO.Path]::GetDirectoryName($cmake))\ctest.exe" --test-dir build -C Debug --output-on-failure
& "$([IO.Path]::GetDirectoryName($cmake))\ctest.exe" --test-dir build -C Release --output-on-failure
& $cmake --build build --config Debug --target worker_common worker_capture worker_transport observer_core Observer -- /p:RunCodeAnalysis=true
```
