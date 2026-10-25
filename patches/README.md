# Patches

`fna/stardew-fna.patch` applies to commit `2d16be6c66ffa9a157ff1bb50ffe215558b40394` of [tModLoader's FNA fork](https://github.com/tModLoader/FNA). It builds FNA under the name Stardew expects, `MonoGame.Framework` version `3.8.0.1641`, and adds the parts of the API that Stardew 1.6.15 uses and FNA doesn't have. It also lets the game load its content from wherever the game folder is, and handles SDL input inside a UWP app.

Build it with `packaging/build-fna.ps1`. The script clones that commit, checks the patch applies cleanly, applies it and writes `MonoGame.Framework.dll` into the work folder you give it.

`fna3d/uwp-corewindow-swapchain.patch` applies to FNA3D commit `a5b6c07b89fd48cf64e310d86f0be3108e76f21b`. It lets FNA3D draw into the app's window on the Xbox, and stops it rebuilding the swapchain when the window is resized to the size it already has.

`packaging/build-native.ps1` clones SDL2 `e1bc7d6de9d78df354807327785703c4044e807b`, FNA3D, and FAudio `221bfe4383542f5e8ccc7bc97db968c7068f86e3` from [tModLoader's fork](https://github.com/tModLoader/FAudio), then builds all three for UWP x64 against the same SDL2.
