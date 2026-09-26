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
4. Replace itself with `holder-desktop`.

The launcher has no GTK, Qt, Boost, curl, or Homebrew/MacPorts API dependency.
It uses POSIX process launching and a tiny localhost socket probe. The frontend
is launched with `exec` so macOS keeps the running app associated with
`Holder.app` for Dock identity.

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

The full app's minimum macOS version is not yet established. Testing an older
system requires compatible builds of the launcher, backend, frontend, and all
bundled libraries; changing the launcher's deployment target alone is insufficient.

### Tests

The default macOS build includes component and launcher integration tests.
Python 3 is required (standard library only).

```sh
ctest --test-dir build --output-on-failure
```

Tests use temporary installations, fake child programs, and private loopback
ports; they do not access your Holder data or show dialogs. macOS CI runs them
before artifact upload. Configure with `-DBUILD_TESTING=OFF` to build only the
launcher without requiring Python.

### Startup behavior

The launcher checks `127.0.0.1:11499` for HTTP 200 with body `pong`. It allows
up to 60 seconds for backend readiness and opens the desktop as soon as the
backend responds. An early backend exit is reported after a final health check.
Exit code 2 can mean another instance holds the daemon lock, so the launcher
keeps waiting within that same budget. An incompatible ping response is reported
without starting another daemon. A timeout does not kill or restart the backend.

Layout is determined by the resolved launcher location, not the working
directory. App bundles use `Contents/Resources` as the runtime root; developer
installs use the parent of `bin`. Missing bundle components produce an error
rather than falling back to adjacent executables. Bundled GTK runtime variables
are applied only to the desktop; the backend inherits the launcher's environment.
Command-line arguments and file/URL activation are not forwarded.

### Diagnostics

Failures are reported with a native macOS alert. The launcher also appends a
small log to:

```text
~/Library/Logs/Holder/launcher.log
```

The log includes timestamps, launcher version, and startup timing, and rotates
at 256 KiB with one `.1` backup. Errors also go to stderr if an alert cannot be
displayed. For backend failures, check `~/.local/share/holder/server/logs/server.log`
(or `$XDG_DATA_HOME/holder/server/logs/server.log` when an absolute override is set).
If a bundled executable is missing, restore or reinstall the complete app bundle.
