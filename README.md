# Ascension to the Throne - Mod Framework and Overhaul

Mods and modding tools for **Ascension to the Throne** (DVS / 1C, 2007), by slovakiaman.

- **AttT Mod Framework** - the mod loader, the **AttT Mod Manager** (switch mods on and off, set their
  order, change their settings, install them from zips, start the game), ModAPI - the toolkit mods are built
  with - and five fixes for the game itself.
- **AttT Overhaul** - a collection of mods built on it: new quests and story, reworked magic and items, new
  lands, and a rebalanced game.

The game's own files are never changed. Every mod is a folder of its own and can be switched on or off on its
own. Texts in English, Russian, Polish and German. Works together with
[AttT-Fix](https://github.com/Leftaseer/AttT-Fix).

**[Download the latest version](https://github.com/slovakiamandev-web/Ascension-to-the-Throne-Modding/releases/latest)**

---

## Contents

- [Requirements](#requirements)
- [Downloads](#downloads)
- [Installing](#installing)
- [Updating from the old collection (0.3)](#updating-from-the-old-collection-03)
- [The mod manager](#the-mod-manager)
- [Saves - please read](#saves---please-read)
- [The framework's fixes](#the-frameworks-fixes)
- [The Overhaul's mods](#the-overhauls-mods)
- [Keys](#keys)
- [Screen resolution and memory](#screen-resolution-and-memory)
- [Languages](#languages)
- [Reporting problems](#reporting-problems)
- [Making your own mods](#making-your-own-mods)
- [Source code](#source-code)
- [Legal](#legal)

## Requirements

- The **Steam version** of the game (1.1.128) in English, Russian, Polish or German.
- Other copies (disc editions, GOG, other patches): the mods load, but every part that needs the game's own
  program code switches itself off - battle effects, keys, the new lands, mouse speed.
- Windows with .NET Framework 4.5 or newer (every Windows 10 and 11 has it).
- Close the game while you install, update or uninstall.

## Downloads

| File | What it is |
|---|---|
| `AttT_Mod_Framework_<version>.zip` | **Needed.** The mod manager, the framework and its five fixes. |
| `AttT_Overhaul_<version>.zip` | The Overhaul's mods. Needs the framework. |
| `AttT_Mod_Framework_<version>_Developer_Kit.zip` | Optional: tools and documentation for making your own mods. |

Every zip has its own README with the full instructions. The program files (the manager and five DLLs) have
no publisher's signature, so Windows SmartScreen or an antivirus may warn about them - their source code is
in [`source/`](source/).

## Installing

1. Find the game folder: in Steam, right-click *Ascension to the Throne* > *Manage* > *Browse local files*
   (the folder that holds `ATThrone.exe`).
2. Unpack the **framework** zip **into** the game folder. It adds `AttT Mod Manager.exe`, the folder
   `ModFramework` and the folder `Mods`. It never replaces one of the game's own files.
3. Start `AttT Mod Manager.exe` once. It puts the framework's file in place - `Resource2.pak` (in the Polish
   game `Resource3.pak`) next to the game's own files, which stay untouched. The window title then says
   "framework 2.0".
4. Install the **Overhaul**: press **Install...** in the manager and pick its zip - or unpack the zip into the
   game folder.
5. Press **Play** in the manager, or start the game from Steam as always - both load the mods.

Start a **new game** for the experience the Overhaul is balanced for. Old saves load and play, but what the
old rules already gave your hero stays as it was.

**Uninstalling:** in the manager, *Framework* > *Uninstall the framework's pak* - the game then starts
without mods. Then delete `AttT Mod Manager.exe`, `ModFramework` and, if you don't want the mods any more,
`Mods`. If you let the game use 4 GB (see below), switch that off first, or let Steam verify the game files.
Your saves stay.

## Updating from the old collection (0.3)

Unpack the new framework and Overhaul zips into the game folder and start the manager. It notices the old
collection and offers to convert it:

- every mod keeps its on/off state and its place in the list,
- the old files (single-file mods, `Mods\modlist.lua`, the DLLs in `Mods`, the old installer's folder
  `ModInstaller`) are removed,
- the framework's file is updated,
- your saves are not touched.

Until you have run the manager once, the game keeps running the old collection as before.

## The mod manager

`AttT Mod Manager.exe` in the game folder:

| | |
|---|---|
| The list | Every mod in `Mods`, in load order (top first). Untick a mod to switch it off. The Status column says why a mod will not load - for example "needs charm_slot, which is switched off". |
| Up / Down | Move a mod in the load order. A mod cannot be moved past one it depends on; the manager says which. |
| Settings | Open the mod's settings (its `config.txt`) in Notepad. Each setting has a comment above it. |
| Install... | Install mods from zips - several at once, or drop zips onto the window. Updating a mod this way keeps your settings. |
| Remove | Delete the mod's folder (untick it instead to keep it). |
| Refresh | Read the `Mods` folder again. |
| Play | Start the game. |
| Framework menu | Install, update or uninstall the framework's file; let the game use 4 GB of memory. |

The list is kept in `Mods\modlist.txt`. A mod you add while the manager is closed appears in it - switched
on - the next time the manager or the game starts.

## Saves - please read

- A save made inside one of the new lands - the inside of the great pyramid, or the south of Annaroth - needs
  the mod that adds it (*Nemesis*, *The south of Annaroth*). The game would crash loading it without them.
  Leave those places and save before you switch such a mod off.
- Items, spells and troops from the mods belong to their mods. A save holding them should be loaded with
  those mods on. Troops of a removed mod turn back into ordinary troops; items may cause trouble.
- Everything else is safe: a save made with a mod loads without it.
- Before every quicksave and quickload the last good quicksave is copied to `Saves\game0_backup.sav`. If a
  quicksave ever gets damaged, rename the backup to `game0.sav`.
- To be safe, copy your `Saves` folder somewhere before installing.

## The framework's fixes

Each is a mod folder you can switch off in the manager.

- **Memory leak fix** - fixes the game's own memory leak, which lost about 45 MB every time a world was built
  (new game, loading from the main menu, travelling to another continent) and crashed the game after about
  17 of them. Also fixes a crash when quitting through the menu.
- **No edge panning** - the camera no longer turns when the cursor touches the edge of the screen; turn it
  with the right mouse button.
- **Extra resolutions** - adds 1920x1080 and 2560x1440 to the Options screen.
- **Wide-screen UI scaling** - the main menu, the spellbook and the spell shop are drawn with the right
  proportions on wide screens.
- **Mouse sensitivity** - F9 / F10 make the mouse slower / faster (kept in its settings). Handy for remote
  play, where the cursor races.

## The Overhaul's mods

### Balance

- **Unit rebalance** - troops rebalanced: archers and mages are weak in melee, the outliers are fixed
  (Olog-Shah, Fury, Assassin and others), and the army speed and range rings are capped. Every change is
  listed in `Mods\unit_balance\balancing_changes.txt`.
- **Scapegoat fix** *(optional)* - Scapegoat can no longer freeze the whole enemy army when its target is
  boxed in.
- **Spell rework** - lasting spells run for 3 turns, speed spells change speed sensibly, Dispel removes only
  what it should (and only magic weaker than its own), Lightning strikes up to five units, healing leaves a
  small shield, big area spells cost more. Every wizard teaches at least four spells; the protection spells
  are gone.
- **Spell AI** - enemy casters use their support spells (Hermes' Sandals, Armor, Curse, Slowdown, Dispel)
  when they actually help, and attack when that is better.
- **Chest rework** - chests inside towns and castles are gone. Chests in the wild pay gold or authority by
  how dangerous the area around them is.
- **Hero training** - every town's trainers - combat instructors, wizards, monks and blacksmiths - teach your
  hero once, for gold: attack, magic power, health or armour. Authority you don't spend on troops gives your
  hero extra health in battle.
- **Shop fixes** - the Ring of Steel Skin now costs what it should (it was free), and the Ring of Health and
  the Amulet of Armor show their real numbers.

### New magic, items and abilities

- **New spells** - Stone Spear, Curse of Frailty, Volley, Swap, Siphon, Stone, Meteor, Creeping Rot, Hermes'
  Host and Chain Lightning.
- **Altar trials** - nine altars across the three continents test you in a battle of pure magic: the altar
  sets your stats and spells, you solve the fight, and you learn the altar's spell. Each needs a certain
  magic power.
- **Combat school** - a COMBAT spell school for warriors: Throw Dagger, Sunder, Cleave, Bloodthirst,
  Demoralise, Whirlwind and Execution. Each is won by beating a town's instructor in a duel of steel.
- **Charm slot** - a new inventory slot for charms.
- **Charms** *(needs Charm slot)* - charms with special battle effects - archers cannot shoot you, every enemy
  aims at you, you act first, you can raise the fallen, and more.
- **Tiered rings and amulets** - 16 lines of rings and amulets in up to four tiers. Each banker along the road
  sells the next tier and takes your old one in trade.
- **Portal key** - P opens the portal map once your hero knows the Portal spell.

### Battle screen

- **Battle camera** - a faster battle camera and a short opening fly-around. C finishes a camera move; hold
  it to skip them all.
- **Battle prompt stats** - the "attack the enemy?" window shows the reward (gold and experience), and each
  troop's stats when you hover over it.

### Story and quests

- **The wolf's curse** - the tower quest retold: the scroll is read at you, not by the digger. Live with the
  curse until help comes - and afterwards, hold Shift to call the beast and run twice as fast.
- **Edward's armour** - after Edward's fall, his castle gives you his armour - with his look. How fast his
  walk is can be set in its settings.
- **Innkeeper gossip** - ask innkeepers "What's new around these parts?" - their rumours lead to quests.
- **Companion levels** - named companions (Kira, Helga, Eneya, Rafael and others) grow in level with your hero
  and learn new spells.
- **Companions** - new companions with stories of their own, and a proper goodbye for some old ones: each asks
  to be taken somewhere, says farewell and stays in the world.
- **The hidden coven** - a hidden coven behind the mountains - Witches for hire, so a mage army can start
  early.
- **Faith** - shrines across the land. Pray to hear the Warden's story, grow your FAITH, and learn seven HOLY
  spells from the monks. The Warden rewards the faithful.
- **The Warden's Isle** *(needs Faith)* - strange lights on an island off the coast. Drive out what came
  through, and the Sisters of the Gate join your cause.
- **The southern road** - Sir Gilbert's south gates open after the battle for Amaz - you walk south instead
  of taking the broken portal.
- **The south of Annaroth** - a whole new land in the south of Annaroth, reached through the dwarves' tunnel.
- **Nemesis** - Grimwald's story: a hunter gone missing near Huntervill, a cult in the great pyramid, a vault
  of the ancients, the drained giants of the south, and the end of it all. Recruit the cult's casters along
  the way. One chapter takes place in the south of Annaroth: without that mod the story stops before it.
  Replaces the game's Aphorism of Gideon quest.

## Keys

| Key | What it does |
|---|---|
| P | Open the portal map, once your hero knows the Portal spell. |
| Shift (hold) | Take the form of the beast and run fast - after the werewolf quest. |
| C | In battle: finish the camera move at once (hold it to skip every camera move). Can be changed in the battle camera's settings. |
| F9 / F10 | Mouse slower / faster. |

## Screen resolution and memory

The game is a 32-bit program and can use only 2 GB of memory. A high resolution (2560x1440) uses a lot of
it, and long sessions with many mods can run out and crash. In the manager, *Framework* > *Let the game use
4 GB of memory* sets one flag in `ATThrone.exe` so 64-bit Windows gives the game 4 GB. Nothing else in the
program changes, and the Polish edition of the game already ships with this flag. The original program is
kept as `ModFramework\ATThrone.exe.backup`; choose the menu item again to switch back. Steam's *Verify
integrity of game files* puts the original program back - set the option again afterwards.

[AttT-Fix](https://github.com/Leftaseer/AttT-Fix) (a separate project) works together with the framework and
is recommended for wide screens and better graphics. When it is installed, Extra resolutions, Wide-screen UI
scaling and Mouse sensitivity step aside and leave those jobs to it.

## Languages

The framework reads the game's language by itself. The mods' texts exist in English, Russian, Polish and
German - the Russian, Polish and German texts are first drafts, and corrections from native speakers are very
welcome. Games in other languages show the mods' texts in English. To force a language, put the line
`language = en` (or `ru`, `pl`, `de`) into `ModFramework\settings.txt`.

## Reporting problems

A mod with a problem is shown in red in the manager, with the reason. Otherwise please open an
[issue](https://github.com/slovakiamandev-web/Ascension-to-the-Throne-Modding/issues) and describe what you
did. Attach these files from the game folder:

- `modapi_history.log` - everything the mods did (never larger than 10 MB)
- `modloader.log` - the last start of the game
- `__ATThrone__ERRORS__<date>.log` - the game's own crash records, if there are any
- `Mods\modlist.txt` - which mods were on

Say which language version of the game you have, and whether AttT-Fix is installed.

## Making your own mods

A mod is one folder in `Mods`: a `mod.txt` (name, version, author, which framework and mods it needs), a
`main.lua` (plain-text Lua - nothing is compiled), optionally a `config.txt` with the player's settings, and a
`resources` folder for texts, models and data. The game finds a new mod folder by itself. To publish a mod,
zip its folder; players install it with the manager.

The **developer kit** adds:

- `ModdingKit\ModAPI_Reference.md` - every function of ModAPI, the framework's toolkit: quests, NPCs, spells,
  items, troops, battles, map objects, terrain, whole new lands, saved data and more;
- `Mods\example_mod` - a small, commented example to copy from;
- `Mods\debug` - test tools: the hero's position and the nearest object's id on screen, gold and levels on a
  key, test travel at every portal;
- `Mods\harness` + `ModdingKit\harness.ps1` - run Lua in the running game from PowerShell;
- `AGENTS.md` / `CLAUDE.md` - a guide to modding this game for AI coding assistants, and a good read for
  people too.

The manager also copies the game's own files out of its paks for you to study
(`"AttT Mod Manager.exe" --files "*.lua"`, `--extract <pattern> <folder>`); those files belong to the game, so
please don't share them.

## Source code

The source of the framework's program files - the five DLLs and the AttT Mod Manager - is in
[`source/`](source/), with build instructions and the SHA-256 checksums of the released files. Built with the
same compiler, the DLLs come out with identical code and data; only their build timestamps differ.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by DVS, 1C or the game's other rights
holders. You need your own copy of the game. No files of the game are included: the few files the mods need
to change are made on your own PC, from your own game's files, the first time the game needs them.
