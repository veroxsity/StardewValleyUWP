using System.Reflection;
using HarmonyLib;
using StardewModdingAPI;

namespace XboxPaths;

public class ModEntry : Mod
{
    public override void Entry(IModHelper helper)
    {
        var harmony = new Harmony("BanditVault.XboxPaths");
        // found at run time so the mod builds without the game's own assembly
        var program = AppDomain.CurrentDomain.GetAssemblies()
            .FirstOrDefault(assembly => assembly.GetName().Name == "Stardew Valley")?
            .GetType("StardewValley.Program");
        if (program == null)
        {
            Monitor.Log("StardewValley.Program not found, save paths left alone", LogLevel.Error);
            return;
        }

        var wanted = new[] { "GetAppDataFolder", "GetLocalAppDataFolder", "GetSavesFolder" };
        var patched = 0;

        foreach (var method in program.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static))
        {
            if (method.ReturnType != typeof(string) || !wanted.Contains(method.Name))
            {
                continue;
            }

            try
            {
                harmony.Patch(method, postfix: new HarmonyMethod(typeof(ModEntry), nameof(MakeAbsolute)));
                patched++;
            }
            catch (Exception error)
            {
                Monitor.Log($"could not patch Program.{method.Name}: {error.GetType().Name}: {error.Message}", LogLevel.Error);
            }
        }

        Monitor.Log($"anchored {patched} data path methods to {Environment.CurrentDirectory}", LogLevel.Info);

        string imageProblem = ImageLoader.Install(harmony, Monitor);
        Monitor.Log(imageProblem ?? "png loading routed through FNA rather than SkiaSharp", imageProblem == null ? LogLevel.Info : LogLevel.Error);
    }

    public static void MakeAbsolute(ref string __result)
    {
        if (!string.IsNullOrEmpty(__result) && !Path.IsPathRooted(__result))
        {
            __result = Path.GetFullPath(__result);
        }
    }
}
