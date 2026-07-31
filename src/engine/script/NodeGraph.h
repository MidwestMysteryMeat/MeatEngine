#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace meat {

// C6 visual scripting: a UE-style node graph that compiles to sandboxed Lua
// (on_init / on_tick / on_player_join / on_player_death + game.* API).
// Graph JSON is the authoring source; emitted .lua is what ScriptHost loads.

enum class NodeKind : std::uint8_t {
    // Events (red) — entry points; only these emit top-level Lua functions.
    EventOnInit = 0,
    EventOnTick,
    EventOnPlayerJoin,
    EventOnPlayerDeath,
    // Actions (blue) — side effects via game.*
    ActionLog,
    ActionSetBlock,
    ActionSpawnPickup,
    // Data / pure (green)
    GetPlayerCount,
    GetItemId,
    Randi,
    ConstInt,
    ConstFloat,
    ConstString,
    // Flow
    Branch,
    // Math
    MathAdd,
    MathGreater,
    // Object / world (editor highlight + prop id into scripts)
    GetWorldObject,   // pure: objectId + name from strA/intA (world prop id)
    HighlightObject,  // action: log + editor selects/highlights prop by id
    PrintObject,      // action: log object id/name
};

enum class PinKind : std::uint8_t {
    Exec = 0,
    Int,
    Float,
    String,
    Bool,
    Object, // world prop / actor id (wire-compatible with Int for codegen)
};

struct GraphPinDesc {
    const char* name = "";
    PinKind kind = PinKind::Exec;
    bool isInput = true;
};

// Fixed pin layouts per NodeKind — index is stable for links + imnodes ids.
// Pin attribute id = nodeId * 64 + pinIndex (see pinAttrId).
const GraphPinDesc* nodePinLayout(NodeKind kind, int& outCount);
const char* nodeKindName(NodeKind kind);
const char* nodeKindCategory(NodeKind kind); // "Event" / "Action" / "Data" / "Flow" / "Math"
bool nodeKindIsEvent(NodeKind kind);

struct GraphNode {
    int id = 0;
    NodeKind kind = NodeKind::ActionLog;
    float posX = 0.0f;
    float posY = 0.0f;
    // Literal defaults when a data input is not wired.
    std::string strA = "hello";
    int intA = 0;
    int intB = 0;
    int intC = 0;
    int intD = 1;
    float floatA = 0.0f;
};

struct GraphLink {
    int id = 0;
    int fromNode = 0;
    int fromPin = 0; // output pin index on fromNode
    int toNode = 0;
    int toPin = 0; // input pin index on toNode
};

struct NodeGraph {
    std::string name = "main";
    std::vector<GraphNode> nodes;
    std::vector<GraphLink> links;
    int nextNodeId = 1;
    int nextLinkId = 1;

    GraphNode* findNode(int id);
    const GraphNode* findNode(int id) const;
    // Link whose toNode/toPin match (data or exec input).
    const GraphLink* findLinkTo(int nodeId, int pinIndex) const;
    // First exec out link from node (pin 0 for actions is usually exec-in; events use out 0).
    const GraphLink* findExecOut(int nodeId, int outPinIndex) const;

    int addNode(NodeKind kind, float x, float y);
    void removeNode(int id);
    bool addLink(int fromNode, int fromPin, int toNode, int toPin);
    void removeLink(int id);

    // Starter graph matching assets/scripts/example.lua behaviour (ammo scatter + log).
    static NodeGraph makeExample();
};

// imnodes attribute id helpers.
inline int pinAttrId(int nodeId, int pinIndex) { return nodeId * 64 + pinIndex; }
inline int pinNodeId(int attrId) { return attrId / 64; }
inline int pinIndexOf(int attrId) { return attrId % 64; }

// JSON serialize (nlohmann). Empty string on failure.
std::string saveGraphJson(const NodeGraph& g);
bool loadGraphJson(NodeGraph& out, const std::string& jsonText);

// Compile graph → sandboxed Lua source that ScriptHost can load.
// Generated file should be named so loadDir picks it up (e.g. scripts/bp_main.lua).
std::string emitGraphLua(const NodeGraph& g);

} // namespace meat
