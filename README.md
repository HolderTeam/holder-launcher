# holder-launcher

App launcher for Holder. 

Holder-daemon aka holderd (the backend server) runs in the background,
the reference GTK implementation (holder-desktop) runs in the frontend
and expects holderd to be listening.

To make a friendly user experience. The launcher checks the backend is up,
starts it if needed, then hands the user over to the frontend.

## Windows

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

### Expected Layout

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

### Build

From a Visual Studio developer shell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

The output executable is `build/Holder.exe`.

### Diagnostics

Failures are reported with a native Windows message box. The launcher also
appends a small log to:

```text
%LOCALAPPDATA%\holder\launcher.log
```

## macOS

`Holder` is also the user-facing executable inside `Holder.app`.

The macOS launcher has the same job as the Windows launcher:

1. Check whether the local Holder backend responds to `GET /ping`.
2. Start `holderd` if the backend is not running.
3. Wait briefly for the backend to become ready.
4. Start `holder-desktop`.
5. Exit.

The launcher has no GTK, Qt, Boost, curl, or Homebrew/MacPorts API dependency.
It uses POSIX process spawning and a tiny localhost socket probe.

### Expected Layout

App bundle layout:

```text
Holder.app/
  Contents/
    MacOS/
      Holder
    Resources/
      bin/
        holder-desktop
        holderd
        holderctl
      schema/
      config/
      assets/
      share/
      lib/
```

Developer side-by-side layout is also accepted:

```text
bin/
  Holder
  holder-desktop
  holderd
  holderctl
```

### Build

On macOS:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

The output executable is `build/Holder`.

### Diagnostics

Failures are reported with a native macOS alert. The launcher also appends a
small log to:

```text
~/Library/Logs/Holder/launcher.log
```
