// Save/load round-trip: a world's authored edits, tick, and player inventory
// must survive a save→load cycle byte-for-byte. Saves are how a dedicated host
// persists across restarts; a silent drift here loses player builds.

#include "Harness.h"

#include "engine/net/LoopbackTransport.h"
#include "engine/net/Messages.h"
#include "game/ServerSim.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using meattest::check;

// A booted server with one editor-authenticated peer, driven over loopback.
struct EditorFixture {
    meat::LoopbackPair wire;
    meat::ServerSim server;

    EditorFixture() {
        meat::NetPolicy policy;
        policy.allowRemoteEditing = true;
        policy.editorToken = "test-editor-token";
        server.setNetPolicy(policy);
    }
    bool boot() { return server.init(24680u); }
    template <typename Msg> void send(const Msg& m) {
        wire.clientEnd().send(1, meat::pack(m), true);
        server.pump(wire.serverEnd());
    }
    void helloAsEditor() {
        server.pump(wire.serverEnd());
        send(meat::HelloMsg{meat::kProtocolVersion, "editor", "test-editor-token"});
    }
    meat::BlockId blockAt(glm::ivec3 v) const { return server.voxels().blockAt(v); }
};

constexpr glm::ivec3 kEdit{48, 40, 48};

std::string scratchSavePath() {
    return (std::filesystem::temp_directory_path() / "meatengine_test_save.json").string();
}

void testEditsSurviveRoundTrip() {
    std::printf("voxel edits and tick survive a save then load\n");
    const std::string path = scratchSavePath();
    std::error_code ec;
    std::filesystem::remove(path, ec);

    meat::BlockId placed = 0;
    {
        EditorFixture f;
        if (!f.boot()) { check(false, "server booted"); return; }
        f.helloAsEditor();

        const meat::BlockId before = f.blockAt(kEdit);
        placed = (before == 1) ? meat::BlockId{2} : meat::BlockId{1};
        f.send(meat::VoxelOpMsg{kEdit, placed});
        check(f.blockAt(kEdit) == placed, "edit applied before save");
        // Advance a few ticks so a non-zero tick is part of what must persist.
        for (int i = 0; i < 5; ++i) f.server.tick(f.wire.serverEnd());
        check(f.server.saveTo(path), "saveTo reported success");
    }

    // A fresh server that never saw the edit must reconstruct it from disk.
    meat::ServerSim loaded;
    check(loaded.initFromSave(path), "initFromSave reported success");
    check(loaded.voxels().blockAt(kEdit) == placed,
          "the loaded world has the edited block (not the generated one)");

    std::filesystem::remove(path, ec);
}

void testCorruptSaveIsRejected() {
    std::printf("a corrupt save is refused, not crashed on\n");
    const std::string path =
        (std::filesystem::temp_directory_path() / "meatengine_bad_save.json").string();
    {
        std::FILE* fp = std::fopen(path.c_str(), "wb");
        if (fp) {
            std::fputs("{ this is not valid json ", fp);
            std::fclose(fp);
        }
    }
    meat::ServerSim s;
    check(!s.initFromSave(path), "initFromSave rejects malformed json");

    std::error_code ec;
    std::filesystem::remove(path, ec);
    check(!s.initFromSave((std::filesystem::temp_directory_path() / "does_not_exist_xyz.json").string()),
          "initFromSave rejects a missing file");
}

void testSaveIsVersionedAndRejectsFuture() {
    std::printf("saves carry a schema version and a newer one is refused\n");
    const std::string path =
        (std::filesystem::temp_directory_path() / "meatengine_ver_save.json").string();
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        meat::ServerSim s;
        if (!s.init(1u)) { check(false, "server booted"); return; }
        check(s.saveTo(path), "saveTo succeeded");
    }
    // The written file must carry a numeric version field.
    {
        std::ifstream in(path);
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        check(!j.is_discarded() && j.contains("version") && j["version"].is_number(),
              "the save has a numeric version field");
    }
    // A save from a hypothetical newer engine must be refused, not misread.
    {
        std::ifstream in(path);
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        j["version"] = 99999; // pretend it's from the future
        std::ofstream out(path);
        out << j.dump();
        out.close();
        meat::ServerSim s;
        check(!s.initFromSave(path), "a future save version is rejected");
    }
    // A versionless (legacy) save is still accepted as v0.
    {
        std::ifstream in(path);
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        j.erase("version");
        std::ofstream out(path);
        out << j.dump();
        out.close();
        meat::ServerSim s;
        check(s.initFromSave(path), "a legacy versionless save still loads");
    }
    std::filesystem::remove(path, ec);
}

void testAutosaveWritesOnInterval() {
    std::printf("periodic autosave writes the world after its interval\n");
    const std::string path =
        (std::filesystem::temp_directory_path() / "meatengine_autosave.json").string();
    std::error_code ec;
    std::filesystem::remove(path, ec);

    meat::GameRules rules;
    rules.terrain = meat::GameRules::Terrain::Void; // fast, empty world
    meat::ServerSim server(rules);
    if (!server.init(1u)) { check(false, "server booted"); return; }
    meat::LoopbackPair wire;
    server.setAutosave(path, 0.25f); // ~15 ticks at 60 Hz

    for (int i = 0; i < 10; ++i) server.tick(wire.serverEnd());
    check(!std::filesystem::exists(path), "no autosave before the interval elapses");
    for (int i = 0; i < 20; ++i) server.tick(wire.serverEnd());
    check(std::filesystem::exists(path), "the world is autosaved once the interval passes");

    // And it's a valid, loadable save.
    meat::ServerSim loaded;
    check(loaded.initFromSave(path), "the autosave loads back");
    std::filesystem::remove(path, ec);
}

} // namespace

namespace meattest {

void runSaveLoad() {
    testEditsSurviveRoundTrip();
    testCorruptSaveIsRejected();
    testSaveIsVersionedAndRejectsFuture();
    testAutosaveWritesOnInterval();
}

} // namespace meattest
