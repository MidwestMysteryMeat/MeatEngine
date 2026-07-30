# Third-party libraries

All fetched at configure time via CMake FetchContent (see `cmake/Dependencies.cmake`);
none are vendored into this repository except where noted.

| Library | Use | License |
|---|---|---|
| [GLFW](https://github.com/glfw/glfw) 3.4 | Window, input, GL context | zlib |
| [glad](https://github.com/Dav1dde/glad) 2.x | OpenGL 4.5 core loader (generated) | MIT (generator); generated code CC0/Apache-2.0 |
| [GLM](https://github.com/g-truc/glm) 1.0.1 | Math | MIT |
| [Assimp](https://github.com/assimp/assimp) 5.4.x | FBX/OBJ/GLB import | BSD-3 |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) 5.x | Physics, character controller | MIT |
| [Lua](https://www.lua.org/) 5.4 | Scripting runtime | MIT |
| [sol2](https://github.com/ThePhD/sol2) 3.x | Lua bindings | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | Editor + debug UI | MIT |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | Editor transform gizmos | MIT |
| [stb](https://github.com/nothings/stb) (stb_image) | PNG/JPG loading | Public domain / MIT |
| [miniaudio](https://github.com/mackron/miniaudio) | Audio playback | Public domain / MIT-0 |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11 | Save files, configs | MIT |
| [Pinocchio](https://github.com/pmolodo/Pinocchio) | tools/autorig (planned, will be vendored under `third_party/`) | MIT (verify at vendor time) |

## Techniques adapted (no source copied)

- **Delta-snapshot technique** — the baseline-relative "diff a state against a prior baseline,
  transmit only the differences, send a full keyframe when there is no baseline" pattern is adapted
  from **Cafu Engine** (`Libs/Network/State.cpp`, MIT, © Carsten Fuchs and contributors,
  https://bitbucket.org/cafu/cafu). MeatEngine's implementation (`src/engine/net/DeltaSnapshot.{h,cpp}`)
  is an independent reimplementation — a per-field changed-bitmask over its own `ByteStream` — not a
  copy of Cafu's byte-level XOR + RLE; no Cafu source is copied. See
  `docs/NETCODE_DELTA_COMPRESSION.md`.
