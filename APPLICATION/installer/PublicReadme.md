# GMod Replay (public edition)

GMod Replay is a Windows viewer for Garry's Mod PERP demo recordings (`.dem`).
This is the public edition. It needs no GitHub account and has no sign-in.

## Install

Download the latest `GModReplay-Public-Setup-<version>-win-x64.exe` from
[Releases](https://github.com/JoeyMarrey29/gmod-replay-public/releases) and run it.
The viewer checks this repository for updates when it starts.

Requirements: Windows 10/11 x64, a DirectX 12 graphics card, and Garry's Mod with
the PERP content installed through Steam.

## Recording hold

A recording can only be opened once it is at least 15 real-world minutes old,
measured from the end of the recording. This stops live recordings from being used
to cheat. Copying, moving or renaming a demo does not reset the hold. When a demo
is too new, the viewer says how many minutes are left.

## Controls

Press F1 in a replay, or open Escape > Controls, for the full list.

## Building from source

The source is published with each release. The `public` edition is the one built
for this repository:

```powershell
git submodule update --init
.\APPLICATION\installer\Build-Release.ps1 -Edition public
```

Only the current release's notes are bundled, in `releases-public/`.

## Licences

The bundled parser (`gmod-demo-parser-app`) is GPL-3.0. Third-party licences are
installed in the `licenses` folder next to the viewer.
