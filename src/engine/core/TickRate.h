#pragma once

namespace meat {

// One tick rate everywhere: server sim, client prediction, replay. Snapshot
// cadence (20 Hz) derives from it in ServerSim.
inline constexpr float kFixedDt = 1.0f / 60.0f;

} // namespace meat
