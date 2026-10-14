# Remaster renderer

An optional upgrade to the OpenGL renderer, aimed at making Descent 1 look
modern on any machine with OpenGL 2.1, including Apple Silicon Macs.

## What it does

- **Hi-res textures**: replacement textures are loaded by bitmap name from
  `hires/<name>.png` (4x the original size), with trilinear and anisotropic
  filtering.
- **Material-aware world shader**: light fixtures, screens and robot eyes
  glow (using DXX-Raytracer's emissive masks), and metal surfaces get a
  subtle specular glint.  A faint detail grain keeps magnified walls from
  looking smeared up close.
- **Post-processing on the 3D view**: ambient occlusion, bloom on lights,
  lasers and explosions, a highlight-preserving tonemap, vignette and FXAA.
  The HUD, cockpit and menus are untouched.

If the GPU lacks anything required, the game silently falls back to the
classic renderer.

## Playing on a Mac

1. Download `D1X-Rebirth-macOS-arm64.zip` from the build artifacts and unzip.
2. Put `descent.hog` and `descent.pig` (from the full game, e.g. the GOG
   release) in `~/Library/Preferences/D1X Rebirth/`.  In Finder use
   Go > Go to Folder... and paste that path; create the folder if missing.
3. Put the hi-res pack `d1x-hires.dxa` in the same folder.
4. First launch: right-click the app and choose **Open**, then **Open**
   again.  The build is not notarised, so macOS asks once.

## Testing on Windows

The same renderer runs on Windows; it is a quick way to check the look
before setting up the Mac.

1. Download **DXX-Rebirth-Windows-x86_64** from the build artifacts and
   unzip.  Use the `D1X-Rebirth` folder.
2. Put `descent.hog`, `descent.pig` and `d1x-hires.dxa` in that folder,
   next to `d1x-rebirth.exe`.
3. Run `d1x-rebirth.exe`.  Add `-window` for windowed mode.

## Building the hi-res pack

The pack is generated from your own copy of the game data and is not
distributed with the source.

```sh
pip install pillow numpy realesrgan-ncnn-py      # Python 3.8-3.12
python3 contrib/remaster/extract_d1_pig.py descent.pig descent.hog extracted
python3 contrib/remaster/upscale_textures.py extracted pack   # --gpu 0 to use a GPU
cd pack && zip -0 -r ../d1x-hires.dxa hires
```

## Switches

Environment variables, all on by default: `DXX_REMASTER=0` (classic
renderer), `DXX_REMASTER_POST=0` (no post-processing), `DXX_REMASTER_AO=0`
(no ambient occlusion), `DXX_REMASTER_HIRES=0` (original textures).
