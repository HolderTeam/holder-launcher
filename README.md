# holder-launcher

Windows product launcher for Holder.

`Holder.exe` is the user-facing Windows entrypoint. It keeps the GTK frontend
focused on UI work by handling Windows startup policy:

1. Check whether the local Holder backend is healthy.
2. Start `holderd.exe` hidden if the backend is not running.
3. Wait briefly for the backend to become ready.
4. Start `holder-desktop.exe`.
5. Exit.

The launcher has no GTK, Qt, MSYS2, Boost, or curl dependency. It uses Win32 and
WinHTTP directly.

## Expected Layout

Installer layout:

```text
Holder/
  Holder.exe
  bin/
    holder-desktop.exe
    holderd.exe
    holderctl.exe
```

Developer side-by-side layout is also accepted:

```text
bin/
  Holder.exe
  holder-desktop.exe
  holderd.exe
  holderctl.exe
```

## Build

From a Visual Studio developer shell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

The output executable is `build/Holder.exe`.

## Diagnostics

Failures are reported with a native Windows message box. The launcher also
appends a small log to:

```text
%LOCALAPPDATA%\holder\launcher.log
```
