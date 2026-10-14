# Remaster material data (Descent 1)

`textures/*.material` and `textures/*_emissive.png` come from
[DXX-Raytracer](https://github.com/BredaUniversityGames/DXX-Raytracer)
(Breda University of Applied Sciences, commit 2bb92462), released under the
same D1X-Rebirth / Parallax licence terms as this project.  Files are
unchanged; five `*_emissive.png` files in the source that are actually
Photoshop documents were left out.

The remaster renderer reads, per PIG bitmap name (`#` replaced by `_`):

- `metalness`, `roughness`: drive a subtle view-aligned specular highlight
- `emissive_strength`, `is_light`: how strongly a surface glows into the bloom
- `<name>_emissive.png`: the red channel masks which texels glow

Everything else in the `.material` files is ignored.

## Where this folder goes

Anywhere on the game's search path: next to `d1x-rebirth.exe` on Windows,
in `D1X-Rebirth.app/Contents/Resources/` on macOS (the CI packages do both),
or next to the binary on Linux.
