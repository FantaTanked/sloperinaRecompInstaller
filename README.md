# sloperinaRecomp Installer

A mod manager that runs stock Switch exefs mods in the PC recompilations of Ocarina of Time (Switch 2): **Hookshot**,
**Longshot**, **Triforce** and other recomps made with the same build tools. It's made for
[sloperinaGZ](https://github.com/PracticeROM/sloperina), the gz-style practice menu, which it installs straight from
sloperinaGZ's own GitHub releases: nothing special is needed from sloperinaGZ's side.

A recompilation turns the game's Switch code into PC code when it's built, so a Switch mod can't just be dropped in. The
manager adds a small mod loader to your game once, and turns each mod into a DLL on your PC with the builder that made
your game folder (its translator and compiler: nothing else is downloaded for that).

Windows only for now.


## Using it

1. Download the zip from [Releases](../../releases), unzip it anywhere, and double-click `ModManager.bat`.
2. Drop your game's exe on the window (`hookshot.exe`, `longshot.exe`, `Triforce.exe` ...), or click Browse.
3. Click **Install loader** (with the game closed). A console window shows the build: a minute or two the first time, at
   the speed you pick (the default leaves your PC usable). Only the game's runtime is rebuilt; the game's own code comes
   from your builder's cache. Your saves and settings stay.
4. Pick sloperinaGZ and click **Install / update**: the newest release is downloaded from
   github.com/PracticeROM/sloperina and turned into a DLL for your game (about a minute).
5. Start the game as usual. Open sloperinaGZ's menu with R + right stick down (RB + right stick down on an Xbox
   controller). Its settings, saves and log are in `<game folder>\user\sdmc\sloperinaGZ`.

**Updating sloperinaGZ:** click **Check for updates**; when a newer one is out, click **Install / update**. The game
itself isn't rebuilt.

**Removing a mod:** pick it and click **Remove**. Its settings and saves in `user\sdmc` stay. To take the loader out too,
rebuild or update the game with its own builder or launcher.

**Other mods:** **Add mod from file...** takes a mod's release zip, or its subsdk module in its folder (with its .ips
beside it).

**When your recomp updates** (its launcher or builder rebuilds the game), the game's exe is replaced and the loader with
it: the manager says "Mod loader: not in this game yet". Click **Install loader** again; your mods stay.

**If the builder isn't found:** the manager looks for it where recomp builders install (it's the folder with `python`
and `nx2` in it, of the same release as your game). If it can't find it, it asks you to pick that `tools` folder once.


## What works differently from a Switch

- A patch that hooks into the middle of a game function can't be kept in a recomp (its code is entered only at the
  start of a function): the manager leaves it out. For sloperinaGZ that's Lock X/Y/Z.
- A mod's once-a-frame hook runs when the game reads the controller each frame, rather than at the exact place it
  patched.


## Where things are

- `<game folder>\exefs_mods\<mod>\mod.dll`: the installed mods (the loader looks for them there).
- `%LOCALAPPDATA%\sloperinaRecomp`: downloads and the mods' build files. The loader's build files: a `sloperinaRecomp`
  folder next to your builder's own work folder. Delete either to free the space; the manager makes them again when it
  needs them.


## How it works

- `loader/patch_loader.py` adds the mod loader (`loader/files/host/modloader.c`) to a copy of your builder's tools and
  rebuilds the game folder with it. It adds no new runtime functions, so the game's translation is unchanged and comes
  from the builder's cache.
- `dll/modlift.py` translates the mod's module on its own with your builder's translator, placed after the game's `sdk`
  so its imports bind to the game's real addresses, and applies its .ips call hooks. `dll/build_dll.ps1` compiles that
  with `dll/smod_rt.c` (the mod's side: calls into the game, system calls, controller, SD card) into `mod.dll`.
- `loader/files/host/smod_api.h` is the interface between the two, versioned and size-checked both ways.
- `mods/<name>/source.json` names a mod's GitHub repo and release asset, for **Install / update** and
  **Check for updates**.


Never share the game folder's files (`hookshot.exe`, `longshot.exe`, `image.bin` ...): they're made from your copy of
the game. This repository holds none of them.
