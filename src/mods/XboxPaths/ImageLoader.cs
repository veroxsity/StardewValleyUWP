using System.Reflection;
using HarmonyLib;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;
using StardewModdingAPI;

namespace XboxPaths;

// SMAPI decodes with SkiaSharp, which imports FONTSUB.dll and cannot ship, so FNA3D's decoder does it
internal static class ImageLoader
{
    private static Type rawTextureData;
    private static IMonitor monitor;
    private static bool complained;

    public static string Install(Harmony harmony, IMonitor log)
    {
        monitor = log;

        Assembly smapi = typeof(Mod).Assembly;
        Type manager = smapi.GetType("StardewModdingAPI.Framework.ContentManagers.ModContentManager");
        rawTextureData = smapi.GetType("StardewModdingAPI.Framework.Content.RawTextureData");
        if (manager == null || rawTextureData == null)
        {
            return "SMAPI's content manager is not where it was, images left alone";
        }

        MethodInfo target = manager.GetMethod("LoadRawImageData", BindingFlags.Instance | BindingFlags.NonPublic);
        if (target == null)
        {
            return "LoadRawImageData is gone, images left alone";
        }

        harmony.Patch(target, prefix: new HarmonyMethod(typeof(ImageLoader), nameof(Decode)));
        return null;
    }

    public static bool Decode(FileInfo file, ref IRawTextureData __result)
    {
        try
        {
            using FileStream stream = File.OpenRead(file.FullName);
            Texture2D.TextureDataFromStreamEXT(stream, out int width, out int height, out byte[] rgba);

            Color[] pixels = new Color[width * height];
            for (int at = 0; at < pixels.Length; at++)
            {
                int source = at * 4;
                int alpha = rgba[source + 3];
                if (alpha == 0)
                {
                    continue;
                }

                // FNA3D returns straight rgba and the content pipeline wants it premultiplied, as SkiaSharp did
                pixels[at] = new Color(
                    rgba[source] * alpha / 255,
                    rgba[source + 1] * alpha / 255,
                    rgba[source + 2] * alpha / 255,
                    alpha);
            }

            __result = (IRawTextureData)Activator.CreateInstance(rawTextureData, width, height, pixels);
            return false;
        }
        catch (Exception error)
        {
            if (!complained)
            {
                complained = true;
                monitor?.Log($"FNA could not decode {file.Name}, falling back to SMAPI: {error.GetType().Name}: {error.Message}", LogLevel.Warn);
            }
            return true;
        }
    }
}
