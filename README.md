# Ascension to the Throne - Mod Collection

Mods and modding tools for **Ascension to the Throne** (DVS / 1C, 2007): new quests and story, reworked
magic and items, a new land, and fixes for the game itself.

- The game's own files are never changed. The mods add one small pak of their own and a `Mods` folder,
  and removing them leaves the game exactly as it was.
- Every module can be switched on or off on its own.
- Texts in English, Russian, Polish and German (the Russian, Polish and German texts are first drafts -
  corrections from native speakers are very welcome).
- Works together with [AttT-Fix](https://github.com/Leftaseer/AttT-Fix).

**[Download the latest version](https://github.com/slovakiamandev-web/Ascension-to-the-Throne-Modding/releases/latest)**

---

## Contents

- [Requirements](#requirements)
- [Downloads](#downloads)
- [Installing](#installing)
- [Saves - please read](#saves---please-read)
- [The modules](#the-modules)
- [Keys](#keys)
- [Reporting problems](#reporting-problems)
- [Making your own mods](#making-your-own-mods)
- [Source code](#source-code)
- [Legal](#legal)

## Requirements

- The **Steam version** of the game (1.1.128) in English, Russian, Polish or German.
- Other copies (disc editions, GOG, other patches): the mods load, but every part that needs the game's own
  program code switches itself off - battle effects, keys, the new lands, mouse speed.
- Start a **new game** for the experience the mods are balanced for. Old saves load and play, but what the
  old rules already gave your hero stays as it was.

## Downloads

Each release has three files. You need **either** the installer **or** the manual version - they install
the same thing.

| File | What it is |
|---|---|
| `AttT_Mods_<version>_Installer.zip` | The installer (`AttT_Mods_Installer.exe`) and its README. Installs, updates and uninstalls for you. |
| `AttT_Mods_<version>_Manual.zip` | The same mods to copy into the game folder by hand - no program to run. |
| `AttT_Mods_<version>_DeveloperKit.zip` | Optional: tools and documentation for making your own mods. |

The installer is a packed Python program without a publisher signature, so Windows SmartScreen or an
antivirus may warn about it. If you prefer not to run it, use the manual version. Every release lists the
SHA-256 checksums of its files.

## Installing

Close the game first. Find the game folder: in Steam, right-click *Ascension to the Throne* >
*Manage* > *Browse local files* (the folder that holds `ATThrone.exe`).

### With the installer

Unpack the zip, open a command window in the unpacked folder (click its address bar, type `cmd`, press
Enter) and run, with your own game folder in quotes:

```
AttT_Mods_Installer.exe install "C:\Program Files (x86)\Steam\steamapps\common\AttT"
```

- `--dry-run` at the end shows what it would do without changing anything.
- It reads the game's language by itself; `--language en` (or `ru`, `pl`, `de`) overrides it.
- `status "<game folder>"` shows what is installed, `uninstall "<game folder>"` removes everything.
- Running a newer installer the same way updates the mods and keeps your own module list.

### By hand

> **Polish version of the game:** your game folder already has a `Resource2.pak` holding the Polish texts -
> never replace it. Rename our `Resource2.pak` to **`Resource3.pak`** before copying.
> (English, Russian and German games: copy it as it is. If your game folder has a `Resource2.pak` before
> you install anything, you need the rename.)

1. Unpack the zip.
2. Copy everything inside its folder `Game folder` - the pak file and the folder `Mods` - into the game
   folder.
3. Start the game.

To remove the mods, delete our pak (`Resource2.pak`, on the Polish game `Resource3.pak`), the `Mods` folder,
`modloader.log` and `modapi_history.log`.

The README inside each zip has the full instructions: updating, removing, switching modules on and off,
screen resolution and languages.

## Saves - please read

- A save made inside one of the new lands - the inside of the great pyramid, or the south of Annaroth -
  needs the module that adds it (`nemesis.lua`, `south_annaroth.lua`). The game would crash loading it
  without them. Leave those places and save before you switch a module off or remove the mods.
- Items, spells and troops from the mods belong to their modules. A save holding them should be loaded with
  those modules on.
- Everything else is safe: a save made with a module loads without it.
- Before every quicksave and quickload the last good quicksave is copied to `Saves\game0_backup.sav`.
- To be safe, copy your `Saves` folder somewhere before installing.

## The modules

Switch modules on and off in `Mods\modlist.lua` (two dashes `--` in front of a line switch it off).

### Framework

- **ModAPI** - the framework every module is built on. It lets mods add quests, NPCs, spells, items, troops,
  map objects and even whole new lands without touching the game's files. Comes with the installation.

### Fixes and quality of life

- **Leak Fix** - fixes the game's own memory leak, which lost about 45 MB every time a world was loaded and
  crashed the game after many loads. Also fixes a crash when quitting through the menu.
- **No Edge Pan** - the camera no longer turns when the cursor touches the edge of the screen.
- **Extra Resolutions** - adds 1920x1080 and 2560x1440 to the Options screen.
- **UI Scaling** - the main menu, the spellbook and the spell shop are drawn with the right proportions on
  wide screens.
- **Mouse Sensitivity** - F9 / F10 make the mouse slower / faster. Handy for remote play, where the cursor
  races.
- **Shop Fixes** - the Ring of Steel Skin now costs what it should (it was free), and the Ring of Health and
  the Amulet of Armor show their real numbers.
- **Battle Camera** - a faster battle camera and a short opening fly-around. C finishes a camera move; hold
  it to skip them all.
- **Battle Prompt Stats** - the "attack the enemy?" window shows the reward (gold and experience), and each
  troop's stats when you hover over it.
- **Portal Key** - P opens the portal map once your hero knows the Portal spell.

When AttT-Fix is installed, Extra Resolutions, UI Scaling and Mouse Sensitivity step aside and leave those
jobs to it.

### Balance

- **Unit Balance** - troops rebalanced: archers and mages are weak in melee, the outliers are fixed
  (Olog-Shah, Fury, Assassin and others), and the army speed and range rings are capped.
- **Scapegoat Fix** *(optional)* - Scapegoat can no longer freeze the whole enemy army when its target is
  boxed in.
- **Spell Rework** - lasting spells run for 3 turns, speed spells change speed sensibly, Dispel removes only
  what it should (and only magic weaker than its own), Lightning strikes up to five units, healing leaves a
  small shield, big area spells cost more. Every wizard teaches at least four spells; the protection spells
  are gone.
- **Spell AI** - enemy casters use their support spells (Hermes' Sandals, Armor, Curse, Slowdown, Dispel)
  when they actually help, and attack when that is better.
- **Chest Rework** - chests inside towns and castles are gone. Chests in the wild pay gold or authority by
  how dangerous the area around them is.
- **Hero Training** - every town's trainers - combat instructors, wizards, monks and blacksmiths - teach
  your hero once, for gold: attack, magic power, health or armour. Authority you don't spend on troops gives
  your hero extra health in battle.

### New magic, items and abilities

- **New Spells** - Stone Spear, Curse of Frailty, Volley, Swap, Siphon, Stone, Meteor, Creeping Rot,
  Hermes' Host and Chain Lightning.
- **Altar Trials** - nine altars across the three continents test you in a battle of pure magic: the altar
  sets your stats and spells, you solve the fight, and you learn the altar's spell. Each needs a certain
  magic power.
- **Combat School** - a COMBAT spell school for warriors: Throw Dagger, Sunder, Cleave, Bloodthirst,
  Demoralise, Whirlwind and Execution. Each is won by beating a town's instructor in a duel of steel.
- **Charm Slot** - a new inventory slot for charms.
- **Charms** *(needs Charm Slot)* - charms with special battle effects - archers cannot shoot you, every
  enemy aims at you, you act first, you can raise the fallen, and more.
- **Tiered Items** - 16 lines of rings and amulets in up to four tiers. Each banker along the road sells the
  next tier and takes your old one in trade.

### Story and quests

- **Werewolf Curse** - the tower quest retold: the scroll is read at you, not by the digger. Live with the
  curse until help comes - and afterwards, hold Shift to call the beast and run twice as fast.
- **Edward's Armour** - after Edward's fall, his castle gives you his armour - with his look.
- **Innkeeper Gossip** - ask innkeepers "What's new around these parts?" - their rumours lead to quests.
- **Companion Levels** - named companions (Kira, Helga, Eneya, Rafael and others) grow in level with your
  hero and learn new spells.
- **Companions** - new companions with stories of their own, and a proper goodbye for some old ones: each
  asks to be taken somewhere, says farewell and stays in the world.
- **Witch Coven** - a hidden coven behind the mountains - Witches for hire, so a mage army can start early.
- **Faith** - shrines across the land. Pray to hear the Warden's story, grow your FAITH, and learn seven
  HOLY spells from the monks. The Warden rewards the faithful.
- **The Warden's Isle** *(needs Faith)* - strange lights on an island off the coast. Drive out what came
  through, and the Sisters of the Gate join your cause.
- **Southern Road** - Sir Gilbert's south gates open after the battle for Amaz - you walk south instead of
  taking the broken portal.
- **South of Annaroth** - a whole new land in the south of Annaroth, reached through the dwarves' tunnel.
- **Nemesis** - Grimwald's story: a hunter gone missing near Huntervill, a cult in the great pyramid, a vault
  of the ancients, the drained giants of the south, and the end of it all. Recruit the cult's casters along
  the way. One chapter needs South of Annaroth. Replaces the game's Aphorism of Gideon quest.

## Keys

| Key | What it does |
|---|---|
| P | Open the portal map, once your hero knows the Portal spell. |
| Shift (hold) | Take the form of the beast and run fast - after the werewolf quest. |
| C | In battle: finish the camera move at once (hold it to skip every camera move). |
| F9 / F10 | Mouse slower / faster. |

## Reporting problems

Please open an [issue](https://github.com/slovakiamandev-web/Ascension-to-the-Throne-Modding/issues) and describe what you did. Attach these files from the game folder:

- `modapi_history.log` - everything the mods did
- `modloader.log` - the last start of the game
- `__ATThrone__ERRORS__<date>.log` - the game's own crash records, if there are any
- `Mods\modlist.lua` - if you changed it

Say which language version of the game you have, and whether AttT-Fix is installed.

## Making your own mods

The **developer kit** (`AttT_Mods_<version>_DeveloperKit.zip`, or the installer's `--dev` option) adds:

- `ModdingKit\ModAPI_Reference.md` - every function of the framework;
- `Mods\example_mod.lua` - a small, commented example to copy from;
- `Mods\debug.lua` - test tools: the hero's position and the nearest object's id on screen, gold and levels
  on a key, test travel at every portal;
- `Mods\harness.lua` + `ModdingKit\harness.ps1` - run Lua in the running game from PowerShell;
- `AGENTS.md` / `CLAUDE.md` - a guide to modding this game for AI coding assistants, and a good read for
  people too.

A mod is a plain-text Lua file in the `Mods` folder plus its line in `Mods\modlist.lua` - nothing is
compiled. The installer can also copy the game's own scripts out of its paks for you to study
(`AttT_Mods_Installer.exe files` / `extract`); those files belong to the game, so please don't share them.

## Source code

The source of the framework's program files - the five DLLs and the AttT Mod Manager - is in
[`source/`](source/), with build instructions and the SHA-256 checksums of the released files. Built with the
same compiler, the DLLs come out with identical code and data; only their build timestamps differ.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by DVS, 1C or the game's other rights
holders. You need your own copy of the game. No files of the game are included: the few files the mods
need to change are made on your own PC, from your own game's files, the first time the game needs them.
