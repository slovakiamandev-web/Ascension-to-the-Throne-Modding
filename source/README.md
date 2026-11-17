# Source code of the AttT Mod Framework's programs

The source of every program file in the **AttT Mod Framework 2.0.1** download: the five DLLs in
`ModFramework\` and `AttT Mod Manager.exe`. Everything else in the download is plain text (Lua scripts,
settings, READMEs) or data.

| File in the download | Source | What it does |
|---|---|---|
| `ModFramework\engine.dll` | [`native/engine.c`](native/engine.c) | Battle access for mods: lists the units of a battle, their health and positions, deals damage, shows damage previews and hex highlights, damage shields. Finds the game's Lua functions inside the game's own program at runtime and checks every address it uses against the game's code first (another build of the game: it switches itself off). |
| `ModFramework\resources.dll` | [`native/resources.c`](native/resources.c) | Lets mods replace or edit the game's data files as the game loads them (terrain, textures, models, animations, scripts), and adds the extra lands. It hooks the game's file and script loaders and lists the mod folders for the loader. Edited copies of game files are built on the player's PC into `ModFramework\_cache`. |
| `ModFramework\keybridge.dll` | [`native/keybridge.c`](native/keybridge.c) | Key presses for mods: subclasses the game's own window and counts the key messages it receives (only while the game has focus). Also reads/writes the hero's facing and position in the game's memory, every address checked with `VirtualQuery` first. |
| `ModFramework\mousescale.dll` | [`native/mousescale.c`](native/mousescale.c) | Mouse speed setting: scales the movement the game reads from its own DirectInput mouse device. |
| `ModFramework\leakfix.dll` | [`native/leakfix.c`](native/leakfix.c) | Fixes the game's memory leak (about 45 MB lost per world load) and a crash when quitting, by redirecting four calls inside the game's program. |
| `AttT Mod Manager.exe` | [`manager/ModManager.cs`](manager/ModManager.cs) | The mod manager and launcher (C#, .NET Framework 4.x, Windows Forms). |

## What these programs do not do

- No network access: the DLLs import only `KERNEL32.dll`, `USER32.dll` (keybridge) and `msvcrt.dll`; the
  manager uses no networking classes.
- Nothing outside the game folder: no registry, no system folders, no services, no other processes.
- The DLLs run only inside the game's own process. The game's Lua scripts load them with Lua's standard
  `package.loadlib`; every patch to the game's code is checked byte for byte against the expected code first.
- The manager starts only the game, Explorer (the Mods folder) and Notepad (a mod's settings). On request it
  sets the "large address aware" flag in `ATThrone.exe` (so the 32-bit game can use 4 GB of memory) and keeps
  the untouched original as `ModFramework\ATThrone.exe.backup`.

## Building

### The DLLs

32-bit MinGW, **GCC 6.3.0** (`mingw32`, from MinGW.org). In the folder with the sources:

```
gcc -shared -O2 -Wall -static-libgcc -o engine.dll engine.c
gcc -shared -O2 -Wall -static-libgcc -o resources.dll resources.c
gcc -shared -O2 -Wall -static-libgcc -o keybridge.dll keybridge.c
gcc -shared -O2 -Wall -static-libgcc -o mousescale.dll mousescale.c
gcc -shared -O2 -Wall -static-libgcc -o leakfix.dll leakfix.c
```

Built this way, the code, data, imports and relocations of each DLL are identical to the released files;
only the build timestamps differ (in the PE header and the export table). Compare them with
`objdump -s -j .text -j .rdata -j .data -j .idata engine.dll`. Run the commands in the source folder with a
bare output name as above: GCC picks a DLL's base address from the output path, and a different path moves
every address.

### The manager

With the C# compiler that comes with Windows (.NET Framework 4.x):

```
%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe /nologo /target:winexe /optimize+ /out:"AttT Mod Manager.exe" /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll ModManager.cs
```

(The C# compiler stamps each build with a new id, so a rebuild is never byte-identical.)

## SHA-256 of the released files (AttT_Mod_Framework_2.0.1.zip)

```
c7abafd430dab50c322e290c8a8f4889382ceb29715a10f488aa578b70c6c1b2  AttT Mod Manager.exe
2c1190251032498b7e4390f91a78d8f8b7a864b1aee4333181a33f1cb6293491  ModFramework/engine.dll
2e726fbd7da095346cbf62041c43ba011efe4faac9f4737c26fc4ae0f57d8d34  ModFramework/keybridge.dll
e46e586a39d43c34afb1a12b658a01cebf5a490395976d809fc744281c84cf85  ModFramework/leakfix.dll
40d79feeca5d430ee750fbcc564a6f49e6ab4d4c33d0542374e50c5b87dadb56  ModFramework/mousescale.dll
5acb81f4b2042406c2148e783ed103726037bd30d1f502a32bde0dea79ac5938  ModFramework/resources.dll
```

The comments in the sources refer to the development project's own notes and folders (`CLAUDE.md`,
`notes/...`, `6_Mod Test/...`); those are not part of this repository.
