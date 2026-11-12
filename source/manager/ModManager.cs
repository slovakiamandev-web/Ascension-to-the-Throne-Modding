// AttT Mod Manager - the mod manager and launcher of the AttT Mod Framework (notes/Mod framework plan.md).
// By slovakiaman.
//
// Lives in the game folder. Lists the mod folders in Mods/, switches them on and off, sets their load
// order, opens their settings (config.txt), installs mods from zips, and starts the game. It writes
// Mods/modlist.txt with the same rules as ModAPI.LoadMods (6_Mod Test/Mods/modapi.lua) - keep the two
// in step. It also puts the framework's pak in place: the download carries it as ModFramework/loader.pak
// (never as a Resource<N>.pak, so unpacking can never overwrite one of the game's own paks); the manager
// copies it to the right Resource<N>.pak, and converts an install of the old collection (0.3).
//
// C# 5 for the compiler that is part of Windows (.NET Framework 4.x):
//   python tools/manager/build_manager.py
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("AttT Mod Manager")]
[assembly: System.Reflection.AssemblyProduct("AttT Mod Framework")]
[assembly: System.Reflection.AssemblyCompany("slovakiaman")]
[assembly: System.Reflection.AssemblyCopyright("AttT Mod Framework by slovakiaman")]
[assembly: System.Reflection.AssemblyDescription("Mod manager and launcher for Ascension to the Throne, by slovakiaman")]
[assembly: System.Reflection.AssemblyVersion("2.0.0.0")]

namespace AttTModManager
{
    // ---------------------------------------------------------------- mods, the list, the rules

    class ModInfo
    {
        public string Id, Folder, Name, Version = "", Author = "", Description = "", Framework, Main = "main.lua";
        public List<string> Requires = new List<string>(), After = new List<string>(), Before = new List<string>();
        public List<string> LoadsAfter = new List<string>();     // other mods whose mod.txt says "before = <this>"
        public string Broken;           // why it cannot load at all (main.lua missing)
        public string Why;              // why it will not load now (set by ModSet.Check), null = it loads
        public bool On;

        public bool HasConfig { get { return File.Exists(Path.Combine(Folder, "config.txt")); } }

        public IEnumerable<string> Deps() { return Requires.Concat(After).Concat(LoadsAfter); }
    }

    static class KeyValues
    {
        // "key = value" lines; "#" starts a comment line; a line starting with "+" continues the previous value
        public static Dictionary<string, string> Read(string path)
        {
            if (!File.Exists(path)) return null;
            var d = new Dictionary<string, string>();
            string last = null;
            foreach (var raw in File.ReadAllLines(path))
            {
                var t = raw.Trim().TrimStart('﻿');
                if (t.StartsWith("+") && last != null) { d[last] = d[last] + " " + t.Substring(1).Trim(); continue; }
                last = null;
                if (t.Length == 0 || t.StartsWith("#")) continue;
                int eq = t.IndexOf('=');
                if (eq <= 0) continue;
                var k = t.Substring(0, eq).Trim();
                if (k.Any(c => !(char.IsLetterOrDigit(c) || c == '_' || c == '.'))) continue;
                d[k] = t.Substring(eq + 1).Trim();
                last = k;
            }
            return d;
        }

        public static string Get(Dictionary<string, string> d, string key, string def)
        {
            string v;
            return d != null && d.TryGetValue(key, out v) && v.Length > 0 ? v : def;
        }

        public static List<string> Ids(string text)
        {
            return (text ?? "").Split(new[] { ',', ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries)
                               .Select(s => s.ToLowerInvariant()).ToList();
        }
    }

    static class Versions
    {
        // part by part as whole numbers: 1.6 is older than 1.54
        public static int Compare(string a, string b)
        {
            var pa = Parts(a); var pb = Parts(b);
            for (int i = 0; i < Math.Max(pa.Count, pb.Count); i++)
            {
                long x = i < pa.Count ? pa[i] : 0, y = i < pb.Count ? pb[i] : 0;
                if (x != y) return x < y ? -1 : 1;
            }
            return 0;
        }

        static List<long> Parts(string v)
        {
            var list = new List<long>();
            var num = new StringBuilder();
            foreach (var c in (v ?? "") + ".")
            {
                if (char.IsDigit(c)) { num.Append(c); continue; }
                if (num.Length > 0) { list.Add(long.Parse(num.ToString())); num.Clear(); }
            }
            return list;
        }
    }

    class ModSet
    {
        public readonly string GameDir, ModsDir, ListFile;
        public Dictionary<string, ModInfo> Mods = new Dictionary<string, ModInfo>();
        public List<ModInfo> Order = new List<ModInfo>();
        public string FrameworkVersion;     // of ModFramework/framework.txt, null = not there
        public int Added;                   // new mods found by the last Scan
        public bool Scanned;                // Save writes nothing before the list was read
        string listText;                    // modlist.txt as it was read

        public ModSet(string gameDir)
        {
            GameDir = gameDir;
            ModsDir = Path.Combine(gameDir, "Mods");
            ListFile = Path.Combine(ModsDir, "modlist.txt");
        }

        public static ModInfo ReadMod(string folder)
        {
            var info = KeyValues.Read(Path.Combine(folder, "mod.txt"));
            var main = KeyValues.Get(info, "main", "main.lua");
            if (info == null && !File.Exists(Path.Combine(folder, main))) return null;
            var name = Path.GetFileName(folder);
            var m = new ModInfo
            {
                Id = name.ToLowerInvariant(), Folder = folder, Main = main, Name = KeyValues.Get(info, "name", name),
                Version = KeyValues.Get(info, "version", ""), Author = KeyValues.Get(info, "author", ""),
                Description = KeyValues.Get(info, "description", ""), Framework = KeyValues.Get(info, "framework", null),
                Requires = KeyValues.Ids(KeyValues.Get(info, "requires", "")),
                After = KeyValues.Ids(KeyValues.Get(info, "after", "")),
                Before = KeyValues.Ids(KeyValues.Get(info, "before", "")),
            };
            if (!File.Exists(Path.Combine(folder, main))) m.Broken = main + " is missing";
            return m;
        }

        // reads Mods/ and modlist.txt and applies the list rules (the same as ModAPI.LoadMods)
        public void Scan()
        {
            var fw = KeyValues.Read(Path.Combine(GameDir, "ModFramework", "framework.txt"));
            FrameworkVersion = KeyValues.Get(fw, "version", null);
            Mods.Clear();
            var folders = Directory.Exists(ModsDir)
                ? Directory.GetDirectories(ModsDir).Select(Path.GetFileName)
                           .Where(n => !n.StartsWith(".")).OrderBy(n => n.ToLowerInvariant(), StringComparer.Ordinal).ToList()
                : new List<string>();
            foreach (var f in folders)
            {
                var m = ReadMod(Path.Combine(ModsDir, f));
                if (m != null && !Mods.ContainsKey(m.Id)) Mods[m.Id] = m;
            }
            foreach (var m in Mods.Values) m.LoadsAfter.Clear();
            foreach (var m in Mods.Values)
                foreach (var b in m.Before)
                {
                    ModInfo other;
                    if (Mods.TryGetValue(b, out other)) other.LoadsAfter.Add(m.Id);
                }
            listText = File.Exists(ListFile) ? File.ReadAllText(ListFile) : null;
            var list = new List<KeyValuePair<string, bool>>();
            var seen = new HashSet<string>();
            foreach (var line in (listText ?? "").Split('\n'))
            {
                var parts = line.Trim().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length < 2) continue;
                var state = parts[0].ToLowerInvariant();
                var id = parts[1].ToLowerInvariant();
                if ((state == "on" || state == "off") && !id.StartsWith("#") && seen.Add(id))
                    list.Add(new KeyValuePair<string, bool>(id, state == "on"));
            }
            Order = Arrange(list, folders.Select(f => f.ToLowerInvariant()).ToList());
            Scanned = true;
            Check();
        }

        // rule 1: lines of gone folders dropped; a new folder gets a line, ON, right below the last of its
        // requires/after mods (none: at the end). Rule 2: a mod above one of its requires/after moves just below it.
        List<ModInfo> Arrange(List<KeyValuePair<string, bool>> list, List<string> folders)
        {
            var o = new List<ModInfo>();
            foreach (var e in list)
            {
                ModInfo m;
                if (Mods.TryGetValue(e.Key, out m)) { m.On = e.Value; o.Add(m); }
            }
            Added = 0;
            foreach (var id in folders)
            {
                ModInfo m;
                if (!Mods.TryGetValue(id, out m) || o.Contains(m)) continue;
                int at = 0;
                foreach (var d in m.Deps())
                {
                    int i = o.FindIndex(x => x.Id == d) + 1;
                    if (i > at) at = i;
                }
                if (at == 0) at = o.Count;
                m.On = true;
                o.Insert(at, m);
                Added++;
            }
            for (int pass = 0; pass < o.Count * o.Count; pass++)
            {
                bool moved = false;
                for (int i = 0; i < o.Count && !moved; i++)
                {
                    int below = i;
                    foreach (var d in o[i].Deps())
                    {
                        int j = o.FindIndex(x => x.Id == d);
                        if (j > below) below = j;
                    }
                    if (below > i)
                    {
                        var m = o[i];
                        o.RemoveAt(i);
                        o.Insert(below, m);
                        moved = true;
                    }
                }
                if (!moved) break;
            }
            return o;
        }

        // rule 3: why each mod will not load (null = it loads)
        public void Check()
        {
            var loads = new HashSet<string>();
            foreach (var m in Order)
            {
                string why = null;
                if (!m.On) why = "switched off";
                else if (m.Broken != null) why = m.Broken;
                else if (m.Framework != null && FrameworkVersion != null && Versions.Compare(FrameworkVersion, m.Framework) < 0)
                    why = "needs framework " + m.Framework + " (this is " + FrameworkVersion + ")";
                else
                    foreach (var r in m.Requires)
                    {
                        ModInfo other;
                        if (!Mods.TryGetValue(r, out other)) why = "needs " + r + ", which is not installed";
                        else if (!other.On) why = "needs " + r + ", which is switched off";
                        else if (!loads.Contains(r)) why = "needs " + r + ", which will not load";
                        if (why != null) break;
                    }
                m.Why = why;
                if (why == null) loads.Add(m.Id);
            }
        }

        public string ListText()
        {
            var sb = new StringBuilder();
            sb.Append("# Which mods load, top first: \"on <mod>\" or \"off <mod>\" (the mod's folder in Mods/).\r\n");
            sb.Append("# Written by the mod manager; the game adds mods it finds missing (switched on).\r\n");
            foreach (var m in Order) sb.Append(m.On ? "on  " : "off ").Append(m.Id).Append("\r\n");
            return sb.ToString();
        }

        public void Save()
        {
            if (!Scanned) return;
            var text = ListText();
            if (listText != null && listText.Replace("\r\n", "\n") == text.Replace("\r\n", "\n")) return;
            Directory.CreateDirectory(ModsDir);
            File.WriteAllText(ListFile, text, new UTF8Encoding(false));
            listText = text;
        }

        // a mod's place in the list may change only within what its requires/after allow
        public string CanSwap(int upper, int lower)
        {
            var a = Order[upper]; var b = Order[lower];
            if (b.Deps().Contains(a.Id))
                return b.Name + " has to load after " + a.Name + " (" +
                       (a.Before.Contains(b.Id) ? a.Name : b.Name) + "'s mod.txt says so).";
            return null;
        }
    }

    // ---------------------------------------------------------------- the game's paks and the framework's pak

    static class Paks
    {
        public static string PakPath(string gameDir, int n) { return Path.Combine(gameDir, "Resource" + n + ".pak"); }

        public static List<int> Numbers(string gameDir)
        {
            var list = new List<int>();
            for (int n = 0; File.Exists(PakPath(gameDir, n)); n++) list.Add(n);
            return list;
        }

        // the names in a pak's index ("GRES", u32 version, u32 count, then 0x88-byte records:
        // 0x80 name (NUL padded), u32 offset, u32 size) - name -> (offset, size)
        public static Dictionary<string, long[]> Index(string path)
        {
            var d = new Dictionary<string, long[]>(StringComparer.OrdinalIgnoreCase);
            using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var r = new BinaryReader(f))
            {
                if (f.Length < 12 || Encoding.ASCII.GetString(r.ReadBytes(4)) != "GRES") return d;
                r.ReadUInt32();
                uint count = r.ReadUInt32();
                for (uint i = 0; i < count && f.Position + 0x88 <= f.Length; i++)
                {
                    var nameBytes = r.ReadBytes(0x80);
                    int end = Array.IndexOf(nameBytes, (byte)0);
                    var name = Encoding.ASCII.GetString(nameBytes, 0, end < 0 ? 0x80 : end);
                    d[name] = new long[] { r.ReadUInt32(), r.ReadUInt32() };
                }
            }
            return d;
        }

        public static byte[] ReadFile(string path, long[] rec)
        {
            using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                f.Seek(rec[0], SeekOrigin.Begin);
                var buf = new byte[rec[1]];
                int got = 0;
                while (got < buf.Length) { int n = f.Read(buf, got, buf.Length - got); if (n <= 0) break; got += n; }
                return buf;
            }
        }

        // the framework's pak = the one holding modapi.lua (ours, of any version); -1 = none
        public static int Ours(string gameDir)
        {
            foreach (var n in Numbers(gameDir).AsEnumerable().Reverse())
                if (Index(PakPath(gameDir, n)).ContainsKey("modapi.lua")) return n;
            return -1;
        }

        // a loader put into the game's own maininit by hand (our development game before the installer):
        // the paks must be left alone then
        public static bool ManualLoader(string gameDir, int ours)
        {
            foreach (var n in Numbers(gameDir))
            {
                if (n == ours) continue;
                var path = PakPath(gameDir, n);
                var idx = Index(path);
                long[] rec;
                if (!idx.TryGetValue("maininit.lua", out rec) || rec[1] > 1000000) continue;
                var text = Encoding.ASCII.GetString(ReadFile(path, rec));
                if (text.Contains("ModLoader")) return true;
            }
            return false;
        }
    }

    // For modders: the game's own files as the game sees them (Resource0, 1, ... - a later pak wins; the
    // framework's pak left out).  --files [pattern]   --extract <pattern> <folder>
    static class GameFiles
    {
        public static int Run(string gameDir, string[] args)
        {
            int ours = Paks.Ours(gameDir);
            var found = new SortedDictionary<string, KeyValuePair<string, long[]>>(StringComparer.OrdinalIgnoreCase);
            foreach (var n in Paks.Numbers(gameDir))
            {
                if (n == ours) continue;
                var path = Paks.PakPath(gameDir, n);
                foreach (var e in Paks.Index(path)) found[e.Key] = new KeyValuePair<string, long[]>(path, e.Value);
            }
            int at = Array.IndexOf(args, args.Contains("--files") ? "--files" : "--extract");
            var rest = args.Skip(at + 1).TakeWhile(a => !a.StartsWith("--")).ToList();
            var pattern = rest.Count > 0 ? rest[0] : "*";
            var re = new System.Text.RegularExpressions.Regex("^" + System.Text.RegularExpressions.Regex.Escape(pattern)
                         .Replace("\\*", ".*").Replace("\\?", ".") + "$", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
            var hits = found.Where(e => re.IsMatch(e.Key)).ToList();
            if (args.Contains("--files"))
            {
                foreach (var e in hits) Console.WriteLine("{0,-50} {1,10}  {2}", e.Key, e.Value.Value[1], Path.GetFileName(e.Value.Key));
                Console.WriteLine(hits.Count + " file(s)");
                return 0;
            }
            if (rest.Count < 2) { Console.WriteLine("--extract <pattern> <folder>"); return 1; }
            if (hits.Count == 0) { Console.WriteLine("no game file matches " + pattern); return 1; }
            Directory.CreateDirectory(rest[1]);
            foreach (var e in hits) File.WriteAllBytes(Path.Combine(rest[1], e.Key), Paks.ReadFile(e.Value.Key, e.Value.Value));
            Console.WriteLine(hits.Count + " file(s) copied to " + Path.GetFullPath(rest[1]));
            return 0;
        }
    }

    static class Framework
    {
        public static string LoaderPak(string gameDir) { return Path.Combine(gameDir, "ModFramework", "loader.pak"); }

        public static bool GameRunning()
        {
            return Process.GetProcessesByName("ATThrone").Length > 0;
        }

        // puts ModFramework/loader.pak in place (first install or update); returns what was done, null = nothing
        public static string Place(string gameDir)
        {
            var loader = LoaderPak(gameDir);
            if (!File.Exists(loader)) return null;
            int ours = Paks.Ours(gameDir);
            if (Paks.ManualLoader(gameDir, ours)) return null;
            if (ours >= 0 && FilesEqual(loader, Paks.PakPath(gameDir, ours))) return null;
            if (GameRunning()) throw new InvalidOperationException("Close the game first - the framework's pak is in use.");
            int target = ours >= 0 ? ours : Paks.Numbers(gameDir).Count;
            File.Copy(loader, Paks.PakPath(gameDir, target), true);
            return (ours >= 0 ? "Framework updated (Resource" : "Framework installed (Resource") + target + ".pak).";
        }

        public static string Remove(string gameDir)
        {
            int ours = Paks.Ours(gameDir);
            if (ours < 0) return "The framework's pak is not installed.";
            if (GameRunning()) throw new InvalidOperationException("Close the game first.");
            if (ours != Paks.Numbers(gameDir).Last())
                return "Resource" + ours + ".pak is not the last pak - remove it by hand (the game stops at a missing number).";
            File.Delete(Paks.PakPath(gameDir, ours));
            return "Resource" + ours + ".pak removed. The game starts without mods now; Mods/ is untouched.";
        }

        static bool FilesEqual(string a, string b)
        {
            var fa = new FileInfo(a); var fb = new FileInfo(b);
            if (fa.Length != fb.Length) return false;
            return File.ReadAllBytes(a).SequenceEqual(File.ReadAllBytes(b));
        }
    }

    // ---------------------------------------------------------------- 4 GB: the exe's large-address-aware flag
    // ATThrone.exe is 32-bit and can use only 2 GB of address space; with the flag set (one bit in the PE header)
    // 64-bit Windows gives it 4 GB. Nothing else in the exe changes - the code our DLLs check is untouched, and the
    // Polish edition ships with the flag set. The first change keeps the untouched exe as
    // ModFramework/ATThrone.exe.backup. Steam's "verify integrity of game files" puts the original back.
    static class LargeAddress
    {
        const ushort Flag = 0x0020;                       // IMAGE_FILE_LARGE_ADDRESS_AWARE

        public static string ExePath(string gameDir) { return Path.Combine(gameDir, "ATThrone.exe"); }
        public static string BackupPath(string gameDir) { return Path.Combine(gameDir, "ModFramework", "ATThrone.exe.backup"); }

        // offset of the COFF Characteristics field; throws when the file is no 32-bit PE
        static int Characteristics(byte[] b)
        {
            if (b.Length < 0x40 || b[0] != 'M' || b[1] != 'Z') throw new InvalidDataException("ATThrone.exe is not a Windows program.");
            int pe = BitConverter.ToInt32(b, 0x3C);
            if (pe < 0 || pe + 0x60 > b.Length || BitConverter.ToUInt32(b, pe) != 0x00004550 || BitConverter.ToUInt16(b, pe + 4) != 0x14C)
                throw new InvalidDataException("ATThrone.exe is not the 32-bit game program.");
            return pe + 22;
        }

        public static bool IsSet(string gameDir)
        {
            var b = File.ReadAllBytes(ExePath(gameDir));
            return (BitConverter.ToUInt16(b, Characteristics(b)) & Flag) != 0;
        }

        // the PE header checksum (as imagehlp's CheckSumMappedFile): 16-bit one's-complement-style sum of the
        // file with the checksum field counted as 0, plus the file length
        public static uint Checksum(byte[] b, int checksumAt)
        {
            ulong sum = 0;
            for (int i = 0; i + 1 < b.Length; i += 2)
            {
                if (i == checksumAt || i == checksumAt + 2) continue;
                sum += BitConverter.ToUInt16(b, i);
                sum = (sum & 0xFFFF) + (sum >> 16);
            }
            if ((b.Length & 1) != 0) { sum += b[b.Length - 1]; sum = (sum & 0xFFFF) + (sum >> 16); }
            sum = (sum & 0xFFFF) + (sum >> 16);
            return (uint)(sum + (ulong)b.Length);
        }

        public static uint StoredChecksum(byte[] b) { return BitConverter.ToUInt32(b, Characteristics(b) - 22 + 0x58); }
        public static uint ComputedChecksum(byte[] b) { return Checksum(b, Characteristics(b) - 22 + 0x58); }

        // returns what was done
        public static string Set(string gameDir, bool on)
        {
            var exe = ExePath(gameDir);
            var b = File.ReadAllBytes(exe);
            int at = Characteristics(b);
            ushort c = BitConverter.ToUInt16(b, at);
            if (((c & Flag) != 0) == on)
                return on ? "The game can already use 4 GB of memory." : "The game already uses the standard 2 GB.";
            if (Framework.GameRunning()) throw new InvalidOperationException("Close the game first.");
            var backup = BackupPath(gameDir);
            if (!File.Exists(backup))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(backup));
                File.Copy(exe, backup);
            }
            c = (ushort)(on ? c | Flag : c & ~Flag);
            BitConverter.GetBytes(c).CopyTo(b, at);
            int sumAt = at - 22 + 0x58;
            if (BitConverter.ToUInt32(b, sumAt) != 0)     // a file without a checksum keeps none
                BitConverter.GetBytes(Checksum(b, sumAt)).CopyTo(b, sumAt);
            var tmp = exe + ".tmp";
            File.WriteAllBytes(tmp, b);
            File.Copy(tmp, exe, true);
            File.Delete(tmp);
            return on ? "The game can now use 4 GB of memory (the original program is kept as ModFramework\\ATThrone.exe.backup)."
                      : "The game is back to the standard 2 GB.";
        }
    }

    // ---------------------------------------------------------------- the old collection (0.3) -> framework 2.0

    static class Converter
    {
        // Leftovers of 0.3 in Mods/: single-file modules, the old list, DLLs and their files, language.txt,
        // the old data folders. Our own 2.0 mod folders are already unpacked next to them.
        public static bool Needed(string gameDir)
        {
            var mods = Path.Combine(gameDir, "Mods");
            if (!Directory.Exists(mods)) return false;
            return File.Exists(Path.Combine(mods, "modlist.lua")) || Directory.GetFiles(mods, "*.lua").Length > 0 ||
                   Directory.GetFiles(mods, "*.dll").Length > 0 || Directory.Exists(Path.Combine(gameDir, "ModInstaller"));
        }

        public static List<string> Convert(string gameDir)
        {
            var done = new List<string>();
            var mods = Path.Combine(gameDir, "Mods");
            // the old list's on/off and order, for the mods that exist as folders now
            // (the old list wins: a modlist.txt may exist already, written with every mod on by the game when
            // it was started before the conversion)
            var old = Path.Combine(mods, "modlist.lua");
            if (File.Exists(old))
            {
                var sb = new StringBuilder();
                var listed = new HashSet<string>();
                int n = 0;
                foreach (var line in File.ReadAllLines(old))
                {
                    var t = line.Trim();
                    bool off = t.StartsWith("--");
                    if (off) t = t.Substring(2).Trim();
                    if (!t.StartsWith("\"")) continue;
                    int end = t.IndexOf(".lua\"", StringComparison.OrdinalIgnoreCase);
                    if (end < 0) continue;
                    var id = t.Substring(1, end - 1).ToLowerInvariant();
                    if (!Directory.Exists(Path.Combine(mods, id))) continue;
                    if (!listed.Add(id)) continue;
                    sb.Append(off ? "off " : "on  ").Append(id).Append("\r\n");
                    n++;
                }
                var txt = Path.Combine(mods, "modlist.txt");
                if (File.Exists(txt))                       // the rest of the current list, after the old one's mods
                    foreach (var line in File.ReadAllLines(txt))
                    {
                        var parts = line.Trim().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                        if (parts.Length >= 2 && (parts[0] == "on" || parts[0] == "off") && listed.Add(parts[1].ToLowerInvariant()))
                            sb.Append(parts[0] == "on" ? "on  " : "off ").Append(parts[1].ToLowerInvariant()).Append("\r\n");
                    }
                File.WriteAllText(txt, sb.ToString());
                done.Add("on/off and order of " + n + " mods taken from the old modlist.lua");
            }
            // the language override
            var lang = Path.Combine(mods, "language.txt");
            if (File.Exists(lang))
            {
                var word = File.ReadAllText(lang).Trim().Split(' ', '\r', '\n')[0].ToLowerInvariant();
                var settings = Path.Combine(gameDir, "ModFramework", "settings.txt");
                Directory.CreateDirectory(Path.GetDirectoryName(settings));
                if (word.Length > 0) File.AppendAllText(settings, "language = " + word + "\r\n");
                File.Delete(lang);
                done.Add("language override (" + word + ") moved to ModFramework/settings.txt");
            }
            int files = 0;
            foreach (var f in Directory.GetFiles(mods))
            {
                var name = Path.GetFileName(f).ToLowerInvariant();
                if (name == "modlist.txt" || name == "mouse_sensitivity.txt") continue;   // the mouse mod takes it over itself
                var ext = Path.GetExtension(name);
                if (ext == ".lua" || ext == ".dll" || ext == ".txt" || ext == ".log" || ext == ".tmp")
                {
                    TryDelete(f);
                    files++;
                }
            }
            if (files > 0) done.Add(files + " old files removed from Mods/ (single-file mods, the old list, DLLs, logs)");
            int folders = 0;
            foreach (var d in Directory.GetDirectories(mods))
            {
                var name = Path.GetFileName(d).ToLowerInvariant();
                bool isMod = File.Exists(Path.Combine(d, "mod.txt")) || File.Exists(Path.Combine(d, "main.lua"));
                if (!isMod && (name == "_cache" || name == "south" || name == "modapi" || name == "valkyrie" ||
                               OnlyOldData(d)))
                {
                    TryDeleteDir(d);
                    folders++;
                    continue;
                }
                if (isMod && Directory.Exists(Path.Combine(d, "resources", "lang")) && Directory.Exists(Path.Combine(d, "lang")))
                {
                    TryDeleteDir(Path.Combine(d, "lang"));
                    folders++;
                }
                if (isMod && name == "nemesis")
                    foreach (var f in new[] { "PyramidInterior.gdl", "PyramidInteriorGizmo.gdl", "layout.lua" })
                        if (File.Exists(Path.Combine(d, "resources", f))) TryDelete(Path.Combine(d, f));
            }
            if (folders > 0) done.Add(folders + " old data folders removed");
            var installer = Path.Combine(gameDir, "ModInstaller");
            if (Directory.Exists(installer))
            {
                TryDeleteDir(installer);
                done.Add("the old installer's notes (ModInstaller/) removed");
            }
            return done;
        }

        // a folder of the old collection that held only texts (lang/) and data
        static bool OnlyOldData(string d)
        {
            return Directory.GetFiles(d).Length == 0 &&
                   Directory.GetDirectories(d).All(s => Path.GetFileName(s).ToLowerInvariant() == "lang");
        }

        static void TryDelete(string f) { try { File.Delete(f); } catch (IOException) { } catch (UnauthorizedAccessException) { } }
        static void TryDeleteDir(string d) { try { Directory.Delete(d, true); } catch (IOException) { } catch (UnauthorizedAccessException) { } }
    }

    // ---------------------------------------------------------------- installing from zips

    static class ZipInstall
    {
        // Every folder in the zip that holds a mod.txt is a mod (at the top, inside Mods/, or deeper - but not
        // inside another mod's resources/). A ModFramework/ folder in the zip (the framework download) is
        // copied too. Returns one line per thing done or skipped.
        public static List<string> Install(string zipPath, string gameDir)
        {
            var log = new List<string>();
            var mods = Path.Combine(gameDir, "Mods");
            using (var zip = ZipFile.OpenRead(zipPath))
            {
                var entries = zip.Entries.Where(e => e.Name.Length > 0).ToList();
                var roots = new List<string>();
                foreach (var e in entries)
                {
                    var p = e.FullName.Replace('\\', '/');
                    if (!p.EndsWith("/mod.txt", StringComparison.OrdinalIgnoreCase) && !p.Equals("mod.txt", StringComparison.OrdinalIgnoreCase)) continue;
                    var root = p.Length > 8 ? p.Substring(0, p.Length - 8) : "";
                    if (root.Length == 0 || root.ToLowerInvariant().Contains("/resources/")) continue;
                    roots.Add(root + "/");
                }
                foreach (var root in roots.Distinct())
                {
                    var id = root.TrimEnd('/').Split('/').Last();
                    if (id.Length == 0 || id.StartsWith(".")) continue;
                    var target = Path.Combine(mods, id);
                    bool update = Directory.Exists(target);
                    var cfgPath = Path.Combine(target, "config.txt");
                    string playerCfg = File.Exists(cfgPath) ? File.ReadAllText(cfgPath) : null;
                    if (update) Directory.Delete(target, true);
                    foreach (var e in entries)
                    {
                        var p = e.FullName.Replace('\\', '/');
                        if (!p.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;
                        Extract(e, target, p.Substring(root.Length));
                    }
                    if (playerCfg != null)
                    {
                        if (File.Exists(cfgPath)) File.WriteAllText(cfgPath, MergeConfig(File.ReadAllText(cfgPath), playerCfg));
                        else File.WriteAllText(cfgPath, playerCfg);
                    }
                    var m = ModSet.ReadMod(target);
                    log.Add((update ? "updated: " : "installed: ") + (m != null ? m.Name + " " + m.Version : id) +
                            (playerCfg != null ? " (your settings kept)" : ""));
                }
                // the framework download: ModFramework/ (the manager itself is never overwritten while it runs)
                var fwRoot = entries.Select(e => e.FullName.Replace('\\', '/'))
                                    .Where(p => p.EndsWith("ModFramework/framework.txt", StringComparison.OrdinalIgnoreCase))
                                    .Select(p => p.Substring(0, p.Length - "framework.txt".Length)).FirstOrDefault();
                if (fwRoot != null)
                {
                    var target = Path.Combine(gameDir, "ModFramework");
                    foreach (var e in entries)
                    {
                        var p = e.FullName.Replace('\\', '/');
                        if (!p.StartsWith(fwRoot, StringComparison.OrdinalIgnoreCase)) continue;
                        try { Extract(e, target, p.Substring(fwRoot.Length)); }
                        catch (IOException) { log.Add("could not replace ModFramework/" + p.Substring(fwRoot.Length) + " - is the game running?"); }
                    }
                    log.Add("framework files updated (ModFramework/)");
                }
                if (roots.Count == 0 && fwRoot == null) log.Add("no mod in " + Path.GetFileName(zipPath) + " (no folder with a mod.txt)");
            }
            return log;
        }

        static void Extract(ZipArchiveEntry e, string targetDir, string rel)
        {
            var full = Path.GetFullPath(Path.Combine(targetDir, rel.Replace('/', Path.DirectorySeparatorChar)));
            if (!full.StartsWith(Path.GetFullPath(targetDir) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                return;                                               // a path leaving the folder - never
            Directory.CreateDirectory(Path.GetDirectoryName(full));
            e.ExtractToFile(full, true);
        }

        // the new config.txt (its lines and comments) with the player's value wherever the key still exists
        public static string MergeConfig(string fresh, string player)
        {
            var mine = new Dictionary<string, string>();
            foreach (var line in player.Replace("\r\n", "\n").Split('\n'))
            {
                var t = line.Trim();
                int eq = t.IndexOf('=');
                if (t.StartsWith("#") || eq <= 0) continue;
                mine[t.Substring(0, eq).Trim()] = t.Substring(eq + 1).Trim();
            }
            var sb = new StringBuilder();
            foreach (var line in fresh.Replace("\r\n", "\n").TrimEnd('\n').Split('\n'))
            {
                var t = line.Trim();
                int eq = t.IndexOf('=');
                string v;
                if (!t.StartsWith("#") && eq > 0 && mine.TryGetValue(t.Substring(0, eq).Trim(), out v))
                    sb.Append(t.Substring(0, eq).Trim()).Append(" = ").Append(v).Append("\r\n");
                else
                    sb.Append(line).Append("\r\n");
            }
            return sb.ToString();
        }
    }

    // ---------------------------------------------------------------- the window

    class MainForm : Form
    {
        readonly ModSet set;
        readonly ListView list = new ListView();
        readonly TextBox details = new TextBox();
        readonly Button up = new Button(), down = new Button(), settings = new Button(), install = new Button(),
                        remove = new Button(), refresh = new Button(), folder = new Button(), play = new Button();
        bool filling, userToggle;

        public MainForm(string gameDir)
        {
            set = new ModSet(gameDir);
            Text = "AttT Mod Manager";
            Font = new Font("Segoe UI", 9f);
            Size = new Size(900, 600);
            MinimumSize = new Size(700, 400);
            StartPosition = FormStartPosition.CenterScreen;
            AllowDrop = true;
            try { Icon = Icon.ExtractAssociatedIcon(Path.Combine(gameDir, "ATThrone.exe")); } catch (Exception) { }

            list.View = View.Details;
            list.CheckBoxes = true;
            list.FullRowSelect = true;
            list.HideSelection = false;
            list.MultiSelect = false;
            list.Columns.Add("Mod", 220);
            list.Columns.Add("Version", 70);
            list.Columns.Add("Author", 140);
            list.Columns.Add("Status", 330);
            list.ItemChecked += OnChecked;
            list.MouseDown += (s, e) =>
            {
                var hit = list.HitTest(e.Location);
                userToggle = hit.Item != null && (hit.Location == ListViewHitTestLocations.StateImage || e.Clicks == 2);
            };
            list.KeyDown += (s, e) => { if (e.KeyCode == Keys.Space) userToggle = true; };
            list.SelectedIndexChanged += delegate { ShowDetails(); };

            details.Multiline = true;
            details.ReadOnly = true;
            details.ScrollBars = ScrollBars.Vertical;
            details.Height = 110;
            details.Dock = DockStyle.Bottom;

            var buttons = new FlowLayoutPanel { Dock = DockStyle.Right, Width = 140, FlowDirection = FlowDirection.TopDown, Padding = new Padding(6) };
            foreach (var b in new[] { up, down, settings, install, remove, refresh, folder, play }) { b.Width = 124; b.Height = 30; buttons.Controls.Add(b); }
            up.Text = "Up"; down.Text = "Down"; settings.Text = "Settings"; install.Text = "Install...";
            remove.Text = "Remove"; refresh.Text = "Refresh"; folder.Text = "Open Mods folder"; play.Text = "Play";
            play.Font = new Font(Font, FontStyle.Bold);
            play.Margin = new Padding(3, 24, 3, 3);
            up.Click += delegate { MoveSelected(-1); };
            down.Click += delegate { MoveSelected(1); };
            settings.Click += delegate { OpenSettings(); };
            install.Click += delegate { PickZips(); };
            remove.Click += delegate { RemoveMod(); };
            refresh.Click += delegate { Reload(); };
            folder.Click += delegate { Directory.CreateDirectory(set.ModsDir); Process.Start("explorer.exe", "\"" + set.ModsDir + "\""); };
            play.Click += delegate { Play(); };

            var menu = new MenuStrip();
            var fwMenu = new ToolStripMenuItem("Framework");
            fwMenu.DropDownItems.Add("Install / update the framework's pak", null, delegate { PlaceFramework(true); });
            fwMenu.DropDownItems.Add("Uninstall the framework's pak...", null, delegate { RemoveFramework(); });
            fwMenu.DropDownItems.Add(new ToolStripSeparator());
            var laa = new ToolStripMenuItem("Let the game use 4 GB of memory (large address aware)");
            laa.Click += delegate { ToggleLargeAddress(); };
            fwMenu.DropDownItems.Add(laa);
            fwMenu.DropDownOpening += delegate
            {
                try { laa.Checked = LargeAddress.IsSet(set.GameDir); laa.Enabled = true; }
                catch (Exception) { laa.Checked = false; laa.Enabled = false; }
            };
            menu.Items.Add(fwMenu);
            var helpMenu = new ToolStripMenuItem("Help");
            helpMenu.DropDownItems.Add("About...", null, delegate
            {
                MessageBox.Show(this, "AttT Mod Manager\nAttT Mod Framework " + (set.FrameworkVersion ?? "?") +
                                "\n\nby slovakiaman\n\nMods, settings and load order for Ascension to the Throne.",
                                "About AttT Mod Manager", MessageBoxButtons.OK, MessageBoxIcon.Information);
            });
            menu.Items.Add(helpMenu);
            MainMenuStrip = menu;

            list.Dock = DockStyle.Fill;
            Controls.Add(list);
            Controls.Add(buttons);
            Controls.Add(details);
            Controls.Add(menu);

            DragEnter += (s, e) => { e.Effect = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None; };
            DragDrop += (s, e) => InstallZips((string[])e.Data.GetData(DataFormats.FileDrop));
            FormClosing += delegate { SaveList(); };
            Shown += delegate { Startup(); };
        }

        void Startup()
        {
            if (Converter.Needed(set.GameDir) && Directory.Exists(Path.Combine(set.GameDir, "ModFramework")))
            {
                var answer = MessageBox.Show(this,
                    "This game still has files of the old mod collection (0.3) in Mods/: single-file mods, the old " +
                    "mod list, DLLs.\n\nConvert it to the new framework now? Your mods' on/off and order are kept; " +
                    "the old files are removed. Saves are not touched.",
                    "AttT Mod Manager", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
                if (answer == DialogResult.Yes)
                {
                    try { Report("Converted", Converter.Convert(set.GameDir)); }
                    catch (Exception ex) { MessageBox.Show(this, ex.Message, "Conversion failed", MessageBoxButtons.OK, MessageBoxIcon.Error); }
                }
            }
            PlaceFramework(false);
            Reload();
        }

        void PlaceFramework(bool asked)
        {
            try
            {
                var done = Framework.Place(set.GameDir);
                if (done != null) MessageBox.Show(this, done, "AttT Mod Manager", MessageBoxButtons.OK, MessageBoxIcon.Information);
                else if (asked) MessageBox.Show(this, File.Exists(Framework.LoaderPak(set.GameDir))
                        ? "The framework's pak is already up to date." : "ModFramework/loader.pak is missing - unpack the framework download into the game folder.",
                        "AttT Mod Manager");
            }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "AttT Mod Manager", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
            UpdateTitle();
        }

        void RemoveFramework()
        {
            if (MessageBox.Show(this, "Remove the framework's pak? The game then starts without any mods. Your Mods folder and saves stay.",
                                "AttT Mod Manager", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            try { MessageBox.Show(this, Framework.Remove(set.GameDir), "AttT Mod Manager"); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "AttT Mod Manager", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
            UpdateTitle();
        }

        void ToggleLargeAddress()
        {
            bool on;
            try { on = !LargeAddress.IsSet(set.GameDir); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "AttT Mod Manager", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            var question = on
                ? "The game is a 32-bit program and can use only 2 GB of memory. At high resolutions with many mods, " +
                  "long sessions can run out of it and crash.\n\nThis sets one flag in ATThrone.exe so 64-bit Windows " +
                  "gives the game 4 GB. Nothing else in the program changes; the original is kept as " +
                  "ModFramework\\ATThrone.exe.backup. Steam's \"Verify integrity of game files\" undoes it (set it " +
                  "again here).\n\nLet the game use 4 GB?"
                : "Put the game back to the standard 2 GB of memory?";
            if (MessageBox.Show(this, question, "AttT Mod Manager", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            try { MessageBox.Show(this, LargeAddress.Set(set.GameDir, on), "AttT Mod Manager"); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "AttT Mod Manager", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
        }

        void UpdateTitle()
        {
            string state;
            if (Paks.Ours(set.GameDir) < 0) state = "framework NOT installed";
            else state = "framework " + (set.FrameworkVersion ?? "?");
            Text = "AttT Mod Manager - " + state;
        }

        void Reload()
        {
            SaveList();
            int keep = list.SelectedIndices.Count > 0 ? list.SelectedIndices[0] : 0;
            set.Scan();
            if (set.Added > 0) set.Save();
            Fill(keep);
            UpdateTitle();
        }

        // one row = one mod; rows are rewritten in place (a toggle or a move never rebuilds the list, which
        // would scroll it back to the top)
        void ApplyRow(ListViewItem it, ModInfo m)
        {
            bool was = filling;
            filling = true;
            it.Tag = m;
            it.Text = m.Name;
            while (it.SubItems.Count < 4) it.SubItems.Add("");
            it.SubItems[1].Text = m.Version;
            it.SubItems[2].Text = m.Author;
            it.SubItems[3].Text = m.Why == null ? "OK" : m.Why;
            if (it.Checked != m.On) it.Checked = m.On;
            var color = m.Why != null && m.On ? Color.Firebrick : !m.On ? Color.Gray : SystemColors.WindowText;
            if (it.ForeColor != color) it.ForeColor = color;
            filling = was;
        }

        void UpdateRows()
        {
            list.BeginUpdate();
            for (int i = 0; i < list.Items.Count && i < set.Order.Count; i++) ApplyRow(list.Items[i], set.Order[i]);
            list.EndUpdate();
        }

        void Fill(int select)
        {
            int top = list.TopItem != null ? list.TopItem.Index : 0;
            filling = true;
            list.BeginUpdate();
            list.Items.Clear();
            foreach (var m in set.Order)
            {
                var it = new ListViewItem(m.Name);
                ApplyRow(it, m);
                list.Items.Add(it);
            }
            list.EndUpdate();
            filling = false;
            if (list.Items.Count > 0)
            {
                var i = Math.Max(0, Math.Min(select, list.Items.Count - 1));
                list.Items[i].Selected = true;
                if (top < list.Items.Count) list.TopItem = list.Items[top];      // stay where the player was
                list.Items[i].EnsureVisible();
            }
            ShowDetails();
        }

        ModInfo Selected { get { return list.SelectedItems.Count > 0 ? (ModInfo)list.SelectedItems[0].Tag : null; } }

        void ShowDetails()
        {
            var m = Selected;
            up.Enabled = down.Enabled = remove.Enabled = m != null;
            settings.Enabled = m != null && m.HasConfig;
            if (m == null) { details.Text = ""; return; }
            var sb = new StringBuilder();
            sb.Append(m.Name).Append(m.Version.Length > 0 ? " " + m.Version : "").Append(m.Author.Length > 0 ? " - " + m.Author : "").Append("\r\n");
            if (m.Description.Length > 0) sb.Append(m.Description).Append("\r\n");
            if (m.Requires.Count > 0) sb.Append("Needs: ").Append(string.Join(", ", m.Requires)).Append("\r\n");
            if (m.After.Count > 0) sb.Append("Loads after: ").Append(string.Join(", ", m.After)).Append("\r\n");
            if (m.Before.Count > 0) sb.Append("Loads before: ").Append(string.Join(", ", m.Before)).Append("\r\n");
            if (m.Framework != null) sb.Append("Framework: ").Append(m.Framework).Append(" or newer\r\n");
            if (m.Why != null) sb.Append("Not loaded: ").Append(m.Why).Append("\r\n");
            sb.Append("Folder: Mods\\").Append(Path.GetFileName(m.Folder)).Append(m.HasConfig ? "  (has settings: config.txt)" : "");
            details.Text = sb.ToString();
        }

        // The list control also reports check changes it makes by itself while it fills (a switched-off mod came
        // back on just by opening the window), so a change counts only when the player made it: a click on the
        // checkbox, a double-click, or Space. Any other change is put back.
        void OnChecked(object sender, ItemCheckedEventArgs e)
        {
            var m = (ModInfo)e.Item.Tag;
            if (filling || !userToggle)
            {
                if (!filling && e.Item.Checked != m.On)
                {
                    filling = true;
                    e.Item.Checked = m.On;
                    filling = false;
                }
                return;
            }
            userToggle = false;
            if (e.Item.Checked == m.On) return;
            m.On = e.Item.Checked;
            set.Check();
            set.Save();
            BeginInvoke((Action)(() => { UpdateRows(); ShowDetails(); }));
        }

        void MoveSelected(int by)
        {
            if (list.SelectedIndices.Count == 0) return;
            int i = list.SelectedIndices[0], j = i + by;
            if (j < 0 || j >= set.Order.Count) return;
            var why = set.CanSwap(Math.Min(i, j), Math.Max(i, j));
            if (why != null) { MessageBox.Show(this, why, "Load order", MessageBoxButtons.OK, MessageBoxIcon.Information); return; }
            var m = set.Order[i];
            set.Order.RemoveAt(i);
            set.Order.Insert(j, m);
            set.Check();
            set.Save();
            UpdateRows();
            list.Items[j].Selected = true;
            list.Items[j].Focused = true;
            list.Items[j].EnsureVisible();
            ShowDetails();
        }

        void OpenSettings()
        {
            var m = Selected;
            if (m == null || !m.HasConfig) return;
            Process.Start("notepad.exe", "\"" + Path.Combine(m.Folder, "config.txt") + "\"");
        }

        void RemoveMod()
        {
            var m = Selected;
            if (m == null) return;
            if (MessageBox.Show(this, "Delete the folder Mods\\" + Path.GetFileName(m.Folder) + " (" + m.Name + ")?\n\n" +
                                "To keep it but not use it, untick it instead. A save that holds this mod's items may need it.",
                                "Remove mod", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            try { Directory.Delete(m.Folder, true); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Remove mod", MessageBoxButtons.OK, MessageBoxIcon.Error); }
            Reload();
        }

        void PickZips()
        {
            using (var d = new OpenFileDialog { Filter = "Mod zips (*.zip)|*.zip", Multiselect = true, Title = "Install mods from zips" })
                if (d.ShowDialog(this) == DialogResult.OK) InstallZips(d.FileNames);
        }

        void InstallZips(string[] zips)
        {
            var lines = new List<string>();
            foreach (var z in zips)
            {
                if (!z.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) { lines.Add("skipped (not a zip): " + Path.GetFileName(z)); continue; }
                try { lines.AddRange(ZipInstall.Install(z, set.GameDir)); }
                catch (Exception ex) { lines.Add("FAILED " + Path.GetFileName(z) + ": " + ex.Message); }
            }
            PlaceFramework(false);
            Reload();
            Report("Install", lines);
        }

        void Report(string title, List<string> lines)
        {
            if (lines.Count == 0) return;
            MessageBox.Show(this, string.Join("\n", lines), title, MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        void SaveList()
        {
            try { set.Save(); }
            catch (Exception ex) { MessageBox.Show(this, "Could not write Mods\\modlist.txt: " + ex.Message, "AttT Mod Manager"); }
        }

        void Play()
        {
            SaveList();
            if (Framework.GameRunning()) { MessageBox.Show(this, "The game is already running.", "AttT Mod Manager"); return; }
            if (Paks.Ours(set.GameDir) < 0 &&
                MessageBox.Show(this, "The framework is not installed - the game will start without mods. Start anyway?",
                                "AttT Mod Manager", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
            var problems = set.Order.Where(m => m.On && m.Why != null).ToList();
            if (problems.Count > 0 &&
                MessageBox.Show(this, problems.Count + " switched-on mod(s) will not load:\n\n" +
                                string.Join("\n", problems.Select(m => m.Name + " - " + m.Why)) + "\n\nStart anyway?",
                                "AttT Mod Manager", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            try
            {
                Process.Start(new ProcessStartInfo(Path.Combine(set.GameDir, "ATThrone.exe")) { WorkingDirectory = set.GameDir, UseShellExecute = false });
                WindowState = FormWindowState.Minimized;
            }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Could not start the game", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }
    }

    static class Program
    {
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
        [DllImport("kernel32.dll")] static extern bool AttachConsole(int pid);

        [STAThread]
        static int Main(string[] args)
        {
            var gameDir = Path.GetDirectoryName(Application.ExecutablePath);
            // "--game <folder>" for testing from elsewhere; "--scan" = apply the list rules, write modlist.txt, print, exit
            for (int i = 0; i + 1 < args.Length; i++) if (args[i] == "--game") gameDir = args[i + 1];
            if (args.Any(a => a.StartsWith("--") && a != "--game")) AttachConsole(-1);   // output into the command window
            if (args.Contains("--files") || args.Contains("--extract"))
                return GameFiles.Run(gameDir, args);
            if (args.Contains("--install"))          // --install <zip> [<zip> ...]: as the Install button, no window
            {
                foreach (var z in args.SkipWhile(a => a != "--install").Skip(1).TakeWhile(a => !a.StartsWith("--")))
                    try { foreach (var line in ZipInstall.Install(z, gameDir)) Console.WriteLine(line); }
                    catch (Exception ex) { Console.WriteLine("FAILED " + z + ": " + ex.Message); }
                try { var done = Framework.Place(gameDir); if (done != null) Console.WriteLine(done); }
                catch (Exception ex) { Console.WriteLine(ex.Message); }
                var s = new ModSet(gameDir);
                s.Scan();
                s.Save();
                return 0;
            }
            if (args.Contains("--laa"))              // --laa on|off|status: the exe's 4 GB flag
            {
                var what = args.SkipWhile(a => a != "--laa").Skip(1).FirstOrDefault() ?? "status";
                try
                {
                    if (what == "on" || what == "off") Console.WriteLine(LargeAddress.Set(gameDir, what == "on"));
                    else
                    {
                        var b = File.ReadAllBytes(LargeAddress.ExePath(gameDir));
                        Console.WriteLine((LargeAddress.IsSet(gameDir) ? "4 GB (large address aware)" : "2 GB (standard)") +
                                          string.Format(", checksum stored {0:X8} computed {1:X8}",
                                                        LargeAddress.StoredChecksum(b), LargeAddress.ComputedChecksum(b)));
                    }
                    return 0;
                }
                catch (Exception ex) { Console.WriteLine(ex.Message); return 1; }
            }
            if (args.Contains("--scan"))
            {
                var s = new ModSet(gameDir);
                s.Scan();
                s.Save();
                foreach (var m in s.Order) Console.WriteLine((m.On ? "on  " : "off ") + m.Id + (m.Why != null ? "  - " + m.Why : ""));
                return 0;
            }
            try { SetProcessDPIAware(); } catch (Exception) { }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            if (!File.Exists(Path.Combine(gameDir, "ATThrone.exe")))
            {
                MessageBox.Show("Put the AttT Mod Manager into the game's folder (the folder with ATThrone.exe).",
                                "AttT Mod Manager", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return 1;
            }
            Application.Run(new MainForm(gameDir));
            return 0;
        }
    }
}
