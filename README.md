# Workers Tracking Native

## Stage 1: protocol foundation

This stage provides a C++20 CMake/MSVC project, stub `WorkerAgent` and `Observer`
Windows executables, and a shared binary protocol library with incremental stream
decoding. It does not include capture, networking, UI, configuration, archives,
or load simulation.

The protocol uses a fixed 12-byte, network-byte-order header containing magic,
version, message type, and payload length. Packets are capped at 8 MiB; control
payloads and UTF-8 control strings use tighter limits.

From PowerShell:

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
& $cmake --build build --config Debug
& $cmake --build build --config Release
& "$([IO.Path]::GetDirectoryName($cmake))\ctest.exe" --test-dir build -C Debug --output-on-failure
& "$([IO.Path]::GetDirectoryName($cmake))\ctest.exe" --test-dir build -C Release --output-on-failure
```
