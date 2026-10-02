# MirasView

**A Vulkan BIM/IFC viewer and scene editor with a physically based, high-fidelity renderer. Based on [MirasEngine](https://github.com/Damir564/MirasEngine)**

Load several IFC models and glTF/GLB files into one scene, browse their spatial structure and properties, annotate elements, fly a camera through the model, and record camera animations. Everything renders in real time with cascaded soft shadows, ambient occlusion, a physically based sky and filmic tone mapping.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Vulkan](https://img.shields.io/badge/Vulkan-1.2%2B-red)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey)
![License](https://img.shields.io/badge/license-PolyForm%20Strict%201.0.0-orange)

## Features

### Model loading
- **IFC (BIM)** parsed in-process with [web-ifc](https://github.com/ThatOpen/engine_web-ifc): no converters, no external tools. Each element is tessellated (one submesh per colour) and the full model data is kept: spatial tree, attributes, type info, property and quantity sets, materials.
- **glTF / GLB** through [fastgltf](https://github.com/spnda/fastgltf), including textures and translucent materials.
- **Multiple models at once.** Load as many IFC and glTF files as you like, instance them, and arrange them in one scene (a typical setup is architecture, structure and MEP of the same building side by side).
- **Binary model cache.** The first import of a model writes a `.cache` file (decoded textures and IFC metadata). Later loads are a disk read instead of a re-import.
- Built-in primitives (cube) for quick blocking out.

### BIM tools
- **Hierarchy panel** with the IFC spatial structure (project, site, building, storey, element), per-element and per-branch show, hide, isolate and "show only this branch".
- **Inspector** with attributes, type info, property sets, quantities and materials of the selected element. Values are copyable, as are GUIDs.
- **Pick elements** by clicking in the viewport, with an outline highlight.
- **Annotations**: attach notes to IFC elements (`M`), shown as labels in the viewport and listed in a dedicated panel.
- **Statistics panel** for scene and render information.

### Camera and animation
- Free-fly camera (WASD + mouse look) with a speed boost, focus-on-selection and a clickable axis view gizmo.
- **Camera animation editor**: record keyframes from the current view, choose linear or Catmull-Rom curved segments per keyframe, set playback speed, loop, and preview the path in the viewport. Paths can be saved and loaded.

### Scene editing
- Move / Rotate / Scale gizmos with grid and step snapping (hold `Ctrl`).
- Rename, duplicate, delete, focus. Save and load scenes (`.scn`); see [Controls](#controls) for shortcuts.
- Dockable ImGui workspace with a resettable layout and a distraction-free **fly mode** that hides the UI.

### Rendering
| Area | What you get |
| --- | --- |
| Lighting | Physically based sky (atmosphere LUT), sun with adjustable azimuth, elevation, colour and intensity, haze and height fog |
| Shadows | Up to 4 cascaded shadow maps (1k to 4k), PCSS soft shadows, screen-space contact shadows |
| Ambient occlusion | Half-resolution AO at off / low / medium / high with radius and intensity controls |
| Post-processing | Bloom, FXAA, MSAA (1x to 8x), vignette, contrast, saturation, exposure |
| Tone mapping | ACES, AgX, Khronos PBR Neutral, Reinhard |
| Background | Realistic sky or a solid colour |
| Display | VSync toggle, FPS cap, adjustable view distance |

All settings are live in **Settings > Graphics** and persist to `settings.json`.

### Engine design
- **Vulkan 1.2 baseline** with dynamic rendering, synchronization2 and shader objects. Newer 1.3 features are optional.
- **GPU-driven draws**: indirect draw calls with per-draw data in SSBOs, frustum culling for the camera and for each shadow cascade, and shadow cascades re-rendered only when their content changes.
- **Automatic backend fallback**: native GPU driver, then emulation layers, then a bundled software (lavapipe) renderer, so it still runs on machines without proper Vulkan support. Force one with `--vulkan`.
- All GPU memory through [VMA](https://github.com/GPUOpen-LibrariesAndPlugins/VulkanMemoryAllocator). Swapchain resize and minimize are handled.
- **Two modes** on one engine: the full **Editor** and a **Game/player** mode (main menu, level, fly camera, pause and graphics settings).

## Controls

| Input | Action |
| --- | --- |
| Left click | Select object / IFC element |
| Right mouse + drag | Look around |
| `W` `A` `S` `D` | Move camera |
| `Shift` (hold) | Move 4x faster |
| `F` | Focus selection |
| `Q` / `1` / `2` / `3` | Select / Move / Rotate / Scale tool |
| `Ctrl` while dragging | Snap to grid / steps |
| Click view-gizmo axis | Look along that axis |
| `F2` | Rename |
| `M` | Annotate selected IFC element |
| `Ctrl+D` / `Del` | Duplicate / delete |
| `Esc` | Deselect / leave fly mode |
| `Shift+`` ` | Toggle fly mode (hide UI) |
| `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` | Open / save / save as scene |
| `Ctrl+I` | Import model |
| `F1` | Controls reference |

## Getting started

### Run a release build
1. Unzip `MirasEngine-win64.zip`.
2. Put your `.ifc`, `.glb` or `.gltf` files in the `models/` folder, or use **File > Import Model...** (`Ctrl+I`).
3. Run `engine.exe`.

The package contains the executable, compiled shaders, the VC++ runtime and the fallback Vulkan runtime, so no installation is needed.

### Command line

```
engine.exe [options]

  --editor                 Start the editor (default)
  --game                   Start the game (main menu, plays level1.scn)
  --scene <path>           Editor: open this .scn at startup; game: use it as the level
  --settings <path>        Load and save graphics settings here instead of settings.json
  --validation             Enable the Vulkan validation layers
  --vulkan <mode>          auto (default) | emulated | software
  --exit-after-frames <N>  Quit after N rendered frames (for automated runs)
  --help, -h               Show this help
```

Example: `engine.exe --editor --scene level1.scn`

## Building from source

**Requirements:** Windows 10/11, Visual Studio with the C++ desktop workload (MSVC, CMake, Ninja), and the [Vulkan SDK](https://vulkan.lunarg.com/) (`glslangValidator` must be on `PATH`).

Dependencies are fetched automatically with CMake FetchContent: Vulkan-Hpp, volk, VMA, vk-bootstrap, SDL3, glm, stb, fastgltf, Dear ImGui (docking), nlohmann_json and web-ifc.

```powershell
git clone <repo-url>
cd MirasView

cmake --preset x64-release
cmake --build out/build/x64-release
# -> out/build/x64-release/src/engine.exe
```

A debug build uses the `x64-debug` preset. Shaders are compiled to SPIR-V at build time and loaded relative to the executable.

### Packaging

```powershell
cmake --build --preset x64-release-package
# -> out/build/x64-release/MirasEngine-win64.zip
```

| CMake option | Effect |
| --- | --- |
| `MIRAS_PACKAGE_SCENES` | Include scenes in the package |
| `MIRAS_PACKAGE_MODEL_CACHES` | Include pre-built model caches for faster first load |
| `MIRAS_BUNDLE_VULKAN_RUNTIME` | Download and ship the fallback Vulkan runtime |

## Project layout

```
src/
  main.cpp          entry point
  app/              Application, launch options, shared settings UI
  engine/           Vulkan context, swapchain, renderer, shadows, atmosphere,
                    model loaders (glTF / IFC), model cache, scenes, camera animation
  editor/           Editor mode: viewport, hierarchy, inspector, annotations, animation, menus
  game/             Game/player mode
  shaders/          GLSL 460 (compiled to SPIR-V at build time)
external/           FetchContent dependency declarations
cmake/              Packaging scripts
```

`engine/` never depends on `editor/` or `game/`. The renderer's pass order is documented in `src/engine/Renderer.h`:

> sky LUT -> shadow cascades -> depth prepass + half-res AO / contact shadows -> HDR scene pass -> selection mask -> bloom -> composite (tone map) -> FXAA -> outline + UI

## Roadmap

Ideas under consideration:
- Clash detection between models (for example MEP against structure)
- Section planes and clipping boxes
- Measurement tools (distance, area, angle)
- Property-based filtering and colouring (by IFC class, storey or any attribute)
- Camera animation export to video or an image sequence
- Saved viewpoints and BCF issue import/export
- Screenshot capture at arbitrary resolution
- Model federation with per-model transform offsets and alignment helpers

## License

Licensed under the [PolyForm Strict License 1.0.0](LICENSE). Noncommercial use is permitted; distribution and derivative works are not. For anything beyond that, contact the author.

Third-party libraries keep their own licenses.
