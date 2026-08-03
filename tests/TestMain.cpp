// Single entry point for the headless test binary. Each suite lives in its own
// file and exposes one run function; add new suites to the call list below.
#include "Harness.h"

#include <cstdio>
#include <filesystem>

namespace meattest {

// ServerSim loads content (e.g. prop_crate.obj) through paths relative to the
// process CWD, so the prop suites only pass when run from a directory that has
// assets/ under it. Anchor to the repo root by walking up for that asset, so the
// tests pass whether launched from the repo root, build/, or CI.
void anchorToAssetsRoot() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int up = 0; up < 8; ++up) {
        if (fs::exists(dir / "assets" / "models" / "prop_crate.obj")) {
            fs::current_path(dir);
            return;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    std::printf("  [warn] assets/ not found from CWD — prop tests may fail\n");
}

// Suites, defined in their own translation units.
void runNetPermissions();
void runDeltaSnapshot();
void runEntityRegistry();
void runWorldgen();
void runSaveLoad();
void runBoneRetarget();
void runInventory();
void runDungeon();
void runGameMode();
void runCrypto();

} // namespace meattest

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not eat the tail
    meattest::anchorToAssetsRoot();
    std::printf("MeatEngine headless tests\n\n");

    meattest::runNetPermissions();
    meattest::runDeltaSnapshot();
    meattest::runEntityRegistry();
    meattest::runWorldgen();
    meattest::runSaveLoad();
    meattest::runBoneRetarget();
    meattest::runInventory();
    meattest::runDungeon();
    meattest::runGameMode();
    meattest::runCrypto();

    std::printf("\n%d checks, %d failures\n", meattest::g_checks, meattest::g_failures);
    return meattest::g_failures == 0 ? 0 : 1;
}
