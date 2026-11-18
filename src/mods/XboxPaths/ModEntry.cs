using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using StardewModdingAPI;

namespace XboxPaths;

public class ModEntry : Mod
{
    private static readonly string[] Wanted = { "GetAppDataFolder", "GetLocalAppDataFolder", "GetSavesFolder" };

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

        var patched = 0;

        foreach (var method in program.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static))
        {
            if (method.ReturnType != typeof(string) || !Wanted.Contains(method.Name))
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

        AnchorSaveSearch(harmony, program.Assembly);

        string imageProblem = ImageLoader.Install(harmony, Monitor);
        Monitor.Log(imageProblem ?? "png loading routed through FNA rather than SkiaSharp", imageProblem == null ? LogLevel.Info : LogLevel.Error);
    }

    // once another mod patches FindSaveGames, Harmony recompiles it optimised and GetSavesFolder
    // gets inlined past the postfix, so the call sites in its IL are wrapped as well
    private void AnchorSaveSearch(Harmony harmony, Assembly game)
    {
        var findSaveGames = game.GetType("StardewValley.Menus.LoadGameMenu")?
            .GetMethod("FindSaveGames", BindingFlags.NonPublic | BindingFlags.Static);
        if (findSaveGames == null)
        {
            Monitor.Log("LoadGameMenu.FindSaveGames not found, saves may not list if another mod patches it", LogLevel.Error);
            return;
        }

        try
        {
            harmony.Patch(findSaveGames, transpiler: new HarmonyMethod(typeof(ModEntry), nameof(WrapPathCalls)));
        }
        catch (Exception error)
        {
            Monitor.Log($"could not patch LoadGameMenu.FindSaveGames: {error.GetType().Name}: {error.Message}", LogLevel.Error);
        }
    }

    public static IEnumerable<CodeInstruction> WrapPathCalls(IEnumerable<CodeInstruction> instructions)
    {
        var absolute = AccessTools.Method(typeof(ModEntry), nameof(Absolute));
        foreach (var instruction in instructions)
        {
            yield return instruction;
            if (instruction.opcode == OpCodes.Call
                && instruction.operand is MethodInfo called
                && called.DeclaringType?.FullName == "StardewValley.Program"
                && called.ReturnType == typeof(string)
                && Wanted.Contains(called.Name))
            {
                yield return new CodeInstruction(OpCodes.Call, absolute);
            }
        }
    }

    public static string Absolute(string path)
    {
        MakeAbsolute(ref path);
        return path;
    }

    public static void MakeAbsolute(ref string __result)
    {
        if (!string.IsNullOrEmpty(__result) && !Path.IsPathRooted(__result))
        {
            __result = Path.GetFullPath(__result);
        }
    }
}
