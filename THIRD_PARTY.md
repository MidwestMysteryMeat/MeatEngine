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
