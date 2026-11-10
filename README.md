# GnuChanSound

GnuChanSound is a small C/X11 control panel for PipeWire audio. It lists output
(sink) and input (source) devices, marks each current default, lets you browse
devices and make one the default, and shows/changes the selected device's
volume and mute state. Device changes are refreshed periodically and on demand.

The app drives the standard **WirePlumber `wpctl` command-line client**. It
does not require a privileged helper, does not execute a shell command, and
passes numeric node IDs as separate arguments. Choosing **Set as default**
calls `wpctl set-default`; volume and mute controls target the selected device.
Controls allow 0–150% volume. `wpctl` errors and missing devices/backend are
shown in the window; `--check-backend` provides a terminal diagnostic.

## Build and install

On Debian/Ubuntu, install the build dependencies:

```sh
sudo apt-get install build-essential pkg-config libx11-dev libxft-dev
```

PipeWire/WirePlumber is needed at runtime; on Debian-family systems the `wpctl`
client is commonly provided by `pipewire-bin`:

```sh
sudo apt-get install pipewire-bin wireplumber
```

Build without installing:

```sh
sh install.sh build
./build/GnuChanSound
```

Or install the binary and desktop-menu entry under `/usr/local` and
`/usr/share/applications`:

```sh
sh install.sh install
```

The script uses `sudo` for installation if needed. Other actions are
`sh install.sh run`, `sh install.sh check`, and `sh install.sh uninstall`.
`make check` builds the program and runs parser, backend-diagnostic, and (when
available) headless X11 startup smoke checks. Build output stays under `build/`.

## Controls

- The **Output** section controls playback devices such as speakers and
  headphones. The **Input** section controls recording devices such as
  microphones. Each section has its own selected device, volume, and mute
  controls.
- Click `<` / `>` to browse outputs or inputs; **Set as default** applies the
  selected device as the system default.
- Click or drag the volume bar to set 0–150%; click **Mute** / **Unmute** to
  toggle the selected device.
- Keyboard: Left/Right changes the selected device in the active section;
  `Tab` switches between Output and Input; `+`/`-` adjusts volume by 5%, `M`
  toggles mute, `R` refreshes both device lists, and `Q`/Escape quits. Clicking
  either section also makes it active for keyboard controls.
- The displayed volume and mute state belong to the currently selected device.
  A running audio server and accessible PipeWire session are required for
  changes to take effect.
- In compact or scaled windows, long device and status text is shortened with
  an ellipsis instead of running underneath the controls.

## Language and platform

The app reads `ApplicationLanguage = Turkish` (or `English`) from
`~/.config/GnuChanSettings/system.conf`, honoring `XDG_CONFIG_HOME` when set.
English is the default when the setting is absent or invalid. No settings file
is needed.

This is an X11 application using Xlib/Xft. It can run through XWayland in many
Wayland sessions, but native Wayland support is not provided. It needs a
WirePlumber `wpctl`; PipeWire API-only setups without `wpctl` are not currently
supported. A GUI display is required to open the control panel.

## Version and license

Version: `0.1.0` (`version.txt`). Licensed under GPL-3.0-or-later; see
`LICENSE`.
