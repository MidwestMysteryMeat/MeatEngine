#include "engine/script/NodeGraph.h"
#include "engine/core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace meat {
namespace {

using json = nlohmann::json;

// ---- pin layouts ----------------------------------------------------------
// Indices are part of the on-disk format — append only; never renumber.

const GraphPinDesc kEventInit[] = {
    {"then", PinKind::Exec, false},
    {"seed", PinKind::Int, false},
};
const GraphPinDesc kEventTick[] = {
    {"then", PinKind::Exec, false},
    {"tick", PinKind::Float, false},
};
const GraphPinDesc kEventPeer[] = {
    {"then", PinKind::Exec, false},
    {"peer", PinKind::Int, false},
};
const GraphPinDesc kActionLog[] = {
    {"exec", PinKind::Exec, true},
    {"then", PinKind::Exec, false},
    {"msg", PinKind::String, true},
};
const GraphPinDesc kActionSetBlock[] = {
    {"exec", PinKind::Exec, true},
    {"then", PinKind::Exec, false},
    {"x", PinKind::Int, true},
    {"y", PinKind::Int, true},
    {"z", PinKind::Int, true},
    {"block", PinKind::Int, true},
};
const GraphPinDesc kActionSpawn[] = {
    {"exec", PinKind::Exec, true},
    {"then", PinKind::Exec, false},
    {"x", PinKind::Float, true},
    {"y", PinKind::Float, true},
    {"z", PinKind::Float, true},
    {"item", PinKind::Int, true},
    {"count", PinKind::Int, true},
};
const GraphPinDesc kGetPlayers[] = {{"count", PinKind::Int, false}};
const GraphPinDesc kGetItemId[] = {
    {"name", PinKind::String, true},
    {"id", PinKind::Int, false},
};
const GraphPinDesc kRandi[] = {
    {"lo", PinKind::Int, true},
    {"hi", PinKind::Int, true},
    {"value", PinKind::Int, false},
};
const GraphPinDesc kConstInt[] = {{"value", PinKind::Int, false}};
const GraphPinDesc kConstFloat[] = {{"value", PinKind::Float, false}};
const GraphPinDesc kConstString[] = {{"value", PinKind::String, false}};
const GraphPinDesc kBranch[] = {
    {"exec", PinKind::Exec, true},
    {"cond", PinKind::Bool, true},
    {"true", PinKind::Exec, false},
    {"false", PinKind::Exec, false},
};
const GraphPinDesc kMathAdd[] = {
    {"a", PinKind::Float, true},
    {"b", PinKind::Float, true},
    {"sum", PinKind::Float, false},
};
const GraphPinDesc kMathGreater[] = {
    {"a", PinKind::Float, true},
    {"b", PinKind::Float, true},
    {"gt", PinKind::Bool, false},
};
const GraphPinDesc kGetWorldObject[] = {
    {"object", PinKind::Object, false},
    {"id", PinKind::Int, false},
    {"name", PinKind::String, false},
};
const GraphPinDesc kHighlightObject[] = {
    {"exec", PinKind::Exec, true},
    {"then", PinKind::Exec, false},
    {"object", PinKind::Object, true},
};
const GraphPinDesc kPrintObject[] = {
    {"exec", PinKind::Exec, true},
    {"then", PinKind::Exec, false},
    {"object", PinKind::Object, true},
};
const GraphPinDesc kGetPropPos[] = {
    {"object", PinKind::Object, true},
    {"x", PinKind::Float, false},
    {"y", PinKind::Float, false},
    {"z", PinKind::Float, false},
};
const GraphPinDesc kGetBlock[] = {
    {"x", PinKind::Int, true},
    {"y", PinKind::Int, true},
    {"z", PinKind::Int, true},
    {"block", PinKind::Int, false},
};
const GraphPinDesc kSequence[] = {
    {"exec", PinKind::Exec, true},
    {"then0", PinKind::Exec, false},
    {"then1", PinKind::Exec, false},
};
const GraphPinDesc kMathSub[] = {
    {"a", PinKind::Float, true},
    {"b", PinKind::Float, true},
    {"diff", PinKind::Float, false},
};
const GraphPinDesc kMathMul[] = {
    {"a", PinKind::Float, true},
    {"b", PinKind::Float, true},
    {"prod", PinKind::Float, false},
};
const GraphPinDesc kMathEqual[] = {
    {"a", PinKind::Float, true},
    {"b", PinKind::Float, true},
    {"eq", PinKind::Bool, false},
};
const GraphPinDesc kGetTick[] = {{"tick", PinKind::Float, false}};
const GraphPinDesc kGetPropCount[] = {{"count", PinKind::Int, false}};
const GraphPinDesc kGetPlayerHealth[] = {
    {"peer", PinKind::Int, true},
    {"health", PinKind::Float, false},
};
const GraphPinDesc kActionDamagePlayer[] = {
    {"exec", PinKind::Exec, true},
    {"then", PinKind::Exec, false},
    {"peer", PinKind::Int, true},
    {"amount", PinKind::Float, true},
};
const GraphPinDesc kActionAnnounce[] = {
    {"exec", PinKind::Exec, true},
    {"then", PinKind::Exec, false},
    {"msg", PinKind::String, true},
};

struct LayoutRef {
    const GraphPinDesc* pins;
    int count;
};

bool pinsCompatible(PinKind a, PinKind b) {
    if (a == b) return true;
    // Object ids are integers at runtime — allow Object↔Int wiring (UE soft-object feel).
    if ((a == PinKind::Object && b == PinKind::Int) || (a == PinKind::Int && b == PinKind::Object))
        return true;
    if ((a == PinKind::Float && b == PinKind::Int) || (a == PinKind::Int && b == PinKind::Float))
        return true;
    return false;
}

LayoutRef layoutOf(NodeKind k) {
    switch (k) {
    case NodeKind::EventOnInit: return {kEventInit, 2};
    case NodeKind::EventOnTick: return {kEventTick, 2};
    case NodeKind::EventOnPlayerJoin:
    case NodeKind::EventOnPlayerDeath: return {kEventPeer, 2};
    case NodeKind::ActionLog: return {kActionLog, 3};
    case NodeKind::ActionSetBlock: return {kActionSetBlock, 6};
    case NodeKind::ActionSpawnPickup: return {kActionSpawn, 7};
    case NodeKind::GetPlayerCount: return {kGetPlayers, 1};
    case NodeKind::GetItemId: return {kGetItemId, 2};
    case NodeKind::Randi: return {kRandi, 3};
    case NodeKind::ConstInt: return {kConstInt, 1};
    case NodeKind::ConstFloat: return {kConstFloat, 1};
    case NodeKind::ConstString: return {kConstString, 1};
    case NodeKind::Branch: return {kBranch, 4};
    case NodeKind::MathAdd: return {kMathAdd, 3};
    case NodeKind::MathGreater: return {kMathGreater, 3};
    case NodeKind::GetWorldObject: return {kGetWorldObject, 3};
    case NodeKind::HighlightObject: return {kHighlightObject, 3};
    case NodeKind::PrintObject: return {kPrintObject, 3};
    case NodeKind::GetPropPosition: return {kGetPropPos, 4};
    case NodeKind::GetBlock: return {kGetBlock, 4};
    case NodeKind::Sequence: return {kSequence, 3};
    case NodeKind::MathSubtract: return {kMathSub, 3};
    case NodeKind::MathMultiply: return {kMathMul, 3};
    case NodeKind::MathEqual: return {kMathEqual, 3};
    case NodeKind::GetTick: return {kGetTick, 1};
    case NodeKind::GetPropCount: return {kGetPropCount, 1};
    case NodeKind::GetPlayerHealth: return {kGetPlayerHealth, 2};
    case NodeKind::ActionDamagePlayer: return {kActionDamagePlayer, 4};
    case NodeKind::ActionAnnounce: return {kActionAnnounce, 3};
    }
    return {kActionLog, 3};
}

std::string escapeLuaString(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    o.push_back('"');
    for (char c : s) {
        if (c == '\\' || c == '"') {
            o.push_back('\\');
            o.push_back(c);
        } else if (c == '\n') {
            o += "\\n";
        } else {
            o.push_back(c);
        }
    }
    o.push_back('"');
    return o;
}

std::string kindToString(NodeKind k) {
    switch (k) {
    case NodeKind::EventOnInit: return "EventOnInit";
    case NodeKind::EventOnTick: return "EventOnTick";
    case NodeKind::EventOnPlayerJoin: return "EventOnPlayerJoin";
    case NodeKind::EventOnPlayerDeath: return "EventOnPlayerDeath";
    case NodeKind::ActionLog: return "ActionLog";
    case NodeKind::ActionSetBlock: return "ActionSetBlock";
    case NodeKind::ActionSpawnPickup: return "ActionSpawnPickup";
    case NodeKind::GetPlayerCount: return "GetPlayerCount";
    case NodeKind::GetItemId: return "GetItemId";
    case NodeKind::Randi: return "Randi";
    case NodeKind::ConstInt: return "ConstInt";
    case NodeKind::ConstFloat: return "ConstFloat";
    case NodeKind::ConstString: return "ConstString";
    case NodeKind::Branch: return "Branch";
    case NodeKind::MathAdd: return "MathAdd";
    case NodeKind::MathGreater: return "MathGreater";
    case NodeKind::GetWorldObject: return "GetWorldObject";
    case NodeKind::HighlightObject: return "HighlightObject";
    case NodeKind::PrintObject: return "PrintObject";
    case NodeKind::GetPropPosition: return "GetPropPosition";
    case NodeKind::GetBlock: return "GetBlock";
    case NodeKind::Sequence: return "Sequence";
    case NodeKind::MathSubtract: return "MathSubtract";
    case NodeKind::MathMultiply: return "MathMultiply";
    case NodeKind::MathEqual: return "MathEqual";
    case NodeKind::GetTick: return "GetTick";
    case NodeKind::GetPropCount: return "GetPropCount";
    case NodeKind::GetPlayerHealth: return "GetPlayerHealth";
    case NodeKind::ActionDamagePlayer: return "ActionDamagePlayer";
    case NodeKind::ActionAnnounce: return "ActionAnnounce";
    }
    return "ActionLog";
}

NodeKind kindFromString(const std::string& s) {
    if (s == "EventOnInit") return NodeKind::EventOnInit;
    if (s == "EventOnTick") return NodeKind::EventOnTick;
    if (s == "EventOnPlayerJoin") return NodeKind::EventOnPlayerJoin;
    if (s == "EventOnPlayerDeath") return NodeKind::EventOnPlayerDeath;
    if (s == "ActionLog") return NodeKind::ActionLog;
    if (s == "ActionSetBlock") return NodeKind::ActionSetBlock;
    if (s == "ActionSpawnPickup") return NodeKind::ActionSpawnPickup;
    if (s == "GetPlayerCount") return NodeKind::GetPlayerCount;
    if (s == "GetItemId") return NodeKind::GetItemId;
    if (s == "Randi") return NodeKind::Randi;
    if (s == "ConstInt") return NodeKind::ConstInt;
    if (s == "ConstFloat") return NodeKind::ConstFloat;
    if (s == "ConstString") return NodeKind::ConstString;
    if (s == "Branch") return NodeKind::Branch;
    if (s == "MathAdd") return NodeKind::MathAdd;
    if (s == "MathGreater") return NodeKind::MathGreater;
    if (s == "GetWorldObject") return NodeKind::GetWorldObject;
    if (s == "HighlightObject") return NodeKind::HighlightObject;
    if (s == "PrintObject") return NodeKind::PrintObject;
    if (s == "GetPropPosition") return NodeKind::GetPropPosition;
    if (s == "GetBlock") return NodeKind::GetBlock;
    if (s == "Sequence") return NodeKind::Sequence;
    if (s == "MathSubtract") return NodeKind::MathSubtract;
    if (s == "MathMultiply") return NodeKind::MathMultiply;
    if (s == "MathEqual") return NodeKind::MathEqual;
    if (s == "GetTick") return NodeKind::GetTick;
    if (s == "GetPropCount") return NodeKind::GetPropCount;
    if (s == "GetPlayerHealth") return NodeKind::GetPlayerHealth;
    if (s == "ActionDamagePlayer") return NodeKind::ActionDamagePlayer;
    if (s == "ActionAnnounce") return NodeKind::ActionAnnounce;
    return NodeKind::ActionLog;
}

// Codegen: emit a Lua expression for a data output pin (walks inputs as needed).
struct EmitCtx {
    const NodeGraph& g;
    std::unordered_set<int> visiting; // cycle guard
    int tmp = 0;
};

std::string emitDataExpr(EmitCtx& ctx, int nodeId, int outPin);

std::string emitInputExpr(EmitCtx& ctx, const GraphNode& n, int inPin, const std::string& fallback) {
    if (const GraphLink* L = ctx.g.findLinkTo(n.id, inPin))
        return emitDataExpr(ctx, L->fromNode, L->fromPin);
    return fallback;
}

std::string emitDataExpr(EmitCtx& ctx, int nodeId, int outPin) {
    if (!ctx.visiting.insert(nodeId).second) return "0 --[[cycle]]";
    const GraphNode* np = ctx.g.findNode(nodeId);
    if (!np) {
        ctx.visiting.erase(nodeId);
        return "0";
    }
    const GraphNode& n = *np;
    std::string r;
    switch (n.kind) {
    case NodeKind::EventOnInit:
        r = (outPin == 1) ? "seed" : "0";
        break;
    case NodeKind::EventOnTick:
        r = (outPin == 1) ? "t" : "0";
        break;
    case NodeKind::EventOnPlayerJoin:
    case NodeKind::EventOnPlayerDeath:
        r = (outPin == 1) ? "peer" : "0";
        break;
    case NodeKind::GetPlayerCount:
        r = "game.player_count()";
        break;
    case NodeKind::GetItemId: {
        const std::string name =
            emitInputExpr(ctx, n, 0, escapeLuaString(n.strA.empty() ? "ammo9mm" : n.strA));
        r = "game.item_id(" + name + ")";
        break;
    }
    case NodeKind::Randi: {
        const std::string lo = emitInputExpr(ctx, n, 0, std::to_string(n.intA));
        const std::string hi = emitInputExpr(ctx, n, 1, std::to_string(n.intB != 0 ? n.intB : 10));
        r = "game.randi(" + lo + ", " + hi + ")";
        break;
    }
    case NodeKind::ConstInt:
        r = std::to_string(n.intA);
        break;
    case NodeKind::ConstFloat: {
        std::ostringstream os;
        os << n.floatA;
        r = os.str();
        break;
    }
    case NodeKind::ConstString:
        r = escapeLuaString(n.strA);
        break;
    case NodeKind::MathAdd: {
        const std::string a = emitInputExpr(ctx, n, 0, std::to_string(n.floatA));
        const std::string b = emitInputExpr(ctx, n, 1, std::to_string(static_cast<float>(n.intB)));
        r = "((" + a + ") + (" + b + "))";
        break;
    }
    case NodeKind::MathGreater: {
        const std::string a = emitInputExpr(ctx, n, 0, "0");
        const std::string b = emitInputExpr(ctx, n, 1, "0");
        r = "((" + a + ") > (" + b + "))";
        break;
    }
    case NodeKind::GetWorldObject:
        if (outPin == 0 || outPin == 1)
            r = std::to_string(n.intA); // object id / id
        else
            r = escapeLuaString(n.strA.empty() ? "object" : n.strA);
        break;
    case NodeKind::GetPropPosition: {
        // Emit a local table lookup once per unique source; simple form per pin.
        const std::string obj = emitInputExpr(ctx, n, 0, std::to_string(n.intA));
        const char* key = outPin == 1 ? "1" : outPin == 2 ? "2" : "3";
        r = "((function() local p=game.prop_pos(" + obj + "); return p and p[" + key +
            "] or 0 end)())";
        break;
    }
    case NodeKind::GetBlock: {
        const std::string x = emitInputExpr(ctx, n, 0, std::to_string(n.intA));
        const std::string y = emitInputExpr(ctx, n, 1, std::to_string(n.intB));
        const std::string z = emitInputExpr(ctx, n, 2, std::to_string(n.intC));
        r = "game.get_block(" + x + ", " + y + ", " + z + ")";
        break;
    }
    case NodeKind::MathSubtract: {
        const std::string a = emitInputExpr(ctx, n, 0, "0");
        const std::string b = emitInputExpr(ctx, n, 1, "0");
        r = "((" + a + ") - (" + b + "))";
        break;
    }
    case NodeKind::MathMultiply: {
        const std::string a = emitInputExpr(ctx, n, 0, "1");
        const std::string b = emitInputExpr(ctx, n, 1, "1");
        r = "((" + a + ") * (" + b + "))";
        break;
    }
    case NodeKind::MathEqual: {
        const std::string a = emitInputExpr(ctx, n, 0, "0");
        const std::string b = emitInputExpr(ctx, n, 1, "0");
        r = "((" + a + ") == (" + b + "))";
        break;
    }
    case NodeKind::GetTick:
        r = "game.tick()";
        break;
    case NodeKind::GetPropCount:
        r = "game.prop_count()";
        break;
    case NodeKind::GetPlayerHealth: {
        const std::string peer = emitInputExpr(ctx, n, 0, std::to_string(n.intA));
        r = "game.player_health(" + peer + ")";
        break;
    }
    default:
        r = "0";
        break;
    }
    ctx.visiting.erase(nodeId);
    return r;
}

void emitExecChain(std::ostringstream& out, EmitCtx& ctx, int nodeId, int indent,
                   std::unordered_set<int>& path) {
    if (nodeId == 0) return;
    if (!path.insert(nodeId).second) {
        out << std::string(indent, ' ') << "-- cycle in exec graph\n";
        return;
    }
    const GraphNode* np = ctx.g.findNode(nodeId);
    if (!np) {
        path.erase(nodeId);
        return;
    }
    const GraphNode& n = *np;
    const std::string pad(indent, ' ');

    auto nextExec = [&](int outPin) {
        if (const GraphLink* L = ctx.g.findExecOut(nodeId, outPin))
            emitExecChain(out, ctx, L->toNode, indent, path);
    };

    switch (n.kind) {
    case NodeKind::ActionLog: {
        const std::string msg =
            emitInputExpr(ctx, n, 2, escapeLuaString(n.strA.empty() ? "log" : n.strA));
        out << pad << "game.log(tostring(" << msg << "))\n";
        nextExec(1);
        break;
    }
    case NodeKind::ActionSetBlock: {
        const std::string x = emitInputExpr(ctx, n, 2, std::to_string(n.intA));
        const std::string y = emitInputExpr(ctx, n, 3, std::to_string(n.intB));
        const std::string z = emitInputExpr(ctx, n, 4, std::to_string(n.intC));
        const std::string b = emitInputExpr(ctx, n, 5, std::to_string(n.intD != 0 ? n.intD : 1));
        out << pad << "game.set_block(" << x << ", " << y << ", " << z << ", " << b << ")\n";
        nextExec(1);
        break;
    }
    case NodeKind::ActionSpawnPickup: {
        const std::string x = emitInputExpr(ctx, n, 2, std::to_string(n.floatA != 0 ? n.floatA : 8.f));
        const std::string y = emitInputExpr(ctx, n, 3, "5.0");
        const std::string z = emitInputExpr(ctx, n, 4, std::to_string(static_cast<float>(n.intA)));
        const std::string item = emitInputExpr(ctx, n, 5, "game.item_id(\"ammo9mm\")");
        const std::string count = emitInputExpr(ctx, n, 6, std::to_string(n.intD != 0 ? n.intD : 24));
        out << pad << "game.spawn_pickup(" << x << ", " << y << ", " << z << ", " << item << ", "
            << count << ")\n";
        nextExec(1);
        break;
    }
    case NodeKind::Branch: {
        const std::string cond = emitInputExpr(ctx, n, 1, "false");
        out << pad << "if " << cond << " then\n";
        if (const GraphLink* L = ctx.g.findExecOut(nodeId, 2)) {
            std::unordered_set<int> sub = path;
            emitExecChain(out, ctx, L->toNode, indent + 2, sub);
        }
        out << pad << "else\n";
        if (const GraphLink* L = ctx.g.findExecOut(nodeId, 3)) {
            std::unordered_set<int> sub = path;
            emitExecChain(out, ctx, L->toNode, indent + 2, sub);
        }
        out << pad << "end\n";
        break;
    }
    case NodeKind::HighlightObject: {
        const std::string obj = emitInputExpr(ctx, n, 2, std::to_string(n.intA));
        const float dur = n.floatA > 0.05f ? n.floatA : 2.0f;
        out << pad << "game.highlight_prop(" << obj << ", " << dur << ")\n";
        nextExec(1);
        break;
    }
    case NodeKind::PrintObject: {
        const std::string obj = emitInputExpr(ctx, n, 2, std::to_string(n.intA));
        out << pad << "game.log(\"[nodegraph] object \" .. tostring(" << obj << "))\n";
        nextExec(1);
        break;
    }
    case NodeKind::Sequence: {
        // Fan-out: run then0 then then1 (UE Sequence-lite, two outputs).
        if (const GraphLink* L = ctx.g.findExecOut(nodeId, 1)) {
            std::unordered_set<int> sub = path;
            emitExecChain(out, ctx, L->toNode, indent, sub);
        }
        if (const GraphLink* L = ctx.g.findExecOut(nodeId, 2)) {
            std::unordered_set<int> sub = path;
            emitExecChain(out, ctx, L->toNode, indent, sub);
        }
        break;
    }
    case NodeKind::ActionDamagePlayer: {
        const std::string peer = emitInputExpr(ctx, n, 2, std::to_string(n.intA));
        const std::string amt =
            emitInputExpr(ctx, n, 3, std::to_string(n.floatA > 0.0f ? n.floatA : 25.0f));
        out << pad << "game.damage_player(" << peer << ", " << amt << ")\n";
        nextExec(1);
        break;
    }
    case NodeKind::ActionAnnounce: {
        const std::string msg =
            emitInputExpr(ctx, n, 2, escapeLuaString(n.strA.empty() ? "announcement" : n.strA));
        out << pad << "game.announce(tostring(" << msg << "))\n";
        nextExec(1);
        break;
    }
    default:
        // Pure / event nodes shouldn't be on an exec chain as targets except events' then.
        nextExec(0);
        break;
    }
    path.erase(nodeId);
}

void emitEventFunction(std::ostringstream& out, const NodeGraph& g, NodeKind eventKind,
                       const char* luaName, const char* args) {
    const GraphNode* event = nullptr;
    for (const GraphNode& n : g.nodes) {
        if (n.kind == eventKind) {
            event = &n;
            break;
        }
    }
    if (!event) return;

    out << "function " << luaName << "(" << args << ")\n";
    EmitCtx ctx{g, {}, 0};
    std::unordered_set<int> path;
    if (const GraphLink* L = g.findExecOut(event->id, 0))
        emitExecChain(out, ctx, L->toNode, 2, path);
    out << "end\n\n";
}

} // namespace

const GraphPinDesc* nodePinLayout(NodeKind kind, int& outCount) {
    const LayoutRef L = layoutOf(kind);
    outCount = L.count;
    return L.pins;
}

const char* nodeKindName(NodeKind kind) {
    switch (kind) {
    case NodeKind::EventOnInit: return "Event BeginPlay";
    case NodeKind::EventOnTick: return "Event Tick";
    case NodeKind::EventOnPlayerJoin: return "Event Player Join";
    case NodeKind::EventOnPlayerDeath: return "Event Player Death";
    case NodeKind::ActionLog: return "Print String";
    case NodeKind::ActionSetBlock: return "Set Block";
    case NodeKind::ActionSpawnPickup: return "Spawn Pickup";
    case NodeKind::GetPlayerCount: return "Get Player Count";
    case NodeKind::GetItemId: return "Get Item Id";
    case NodeKind::Randi: return "Random Integer";
    case NodeKind::ConstInt: return "Integer";
    case NodeKind::ConstFloat: return "Float";
    case NodeKind::ConstString: return "String";
    case NodeKind::Branch: return "Branch";
    case NodeKind::MathAdd: return "Add";
    case NodeKind::MathGreater: return "Greater";
    case NodeKind::GetWorldObject: return "Get World Object";
    case NodeKind::HighlightObject: return "Highlight Object";
    case NodeKind::PrintObject: return "Print Object";
    case NodeKind::GetPropPosition: return "Get Prop Position";
    case NodeKind::GetBlock: return "Get Block";
    case NodeKind::Sequence: return "Sequence";
    case NodeKind::MathSubtract: return "Subtract";
    case NodeKind::MathMultiply: return "Multiply";
    case NodeKind::MathEqual: return "Equal";
    case NodeKind::GetTick: return "Get Tick";
    case NodeKind::GetPropCount: return "Get Prop Count";
    case NodeKind::GetPlayerHealth: return "Get Player Health";
    case NodeKind::ActionDamagePlayer: return "Damage Player";
    case NodeKind::ActionAnnounce: return "Announce";
    }
    return "Node";
}

const char* nodeKindCategory(NodeKind kind) {
    if (nodeKindIsEvent(kind)) return "Event";
    switch (kind) {
    case NodeKind::ActionLog:
    case NodeKind::ActionSetBlock:
    case NodeKind::ActionSpawnPickup:
    case NodeKind::HighlightObject:
    case NodeKind::PrintObject:
    case NodeKind::ActionDamagePlayer:
    case NodeKind::ActionAnnounce: return "Action";
    case NodeKind::Branch:
    case NodeKind::Sequence: return "Flow";
    case NodeKind::MathAdd:
    case NodeKind::MathGreater:
    case NodeKind::MathSubtract:
    case NodeKind::MathMultiply:
    case NodeKind::MathEqual: return "Math";
    case NodeKind::GetWorldObject:
    case NodeKind::GetPropPosition: return "Object";
    default: return "Data";
    }
}

bool nodeKindIsEvent(NodeKind kind) {
    return kind == NodeKind::EventOnInit || kind == NodeKind::EventOnTick ||
           kind == NodeKind::EventOnPlayerJoin || kind == NodeKind::EventOnPlayerDeath;
}

GraphNode* NodeGraph::findNode(int id) {
    for (GraphNode& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
const GraphNode* NodeGraph::findNode(int id) const {
    for (const GraphNode& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const GraphLink* NodeGraph::findLinkTo(int nodeId, int pinIndex) const {
    for (const GraphLink& L : links)
        if (L.toNode == nodeId && L.toPin == pinIndex) return &L;
    return nullptr;
}

const GraphLink* NodeGraph::findExecOut(int nodeId, int outPinIndex) const {
    for (const GraphLink& L : links)
        if (L.fromNode == nodeId && L.fromPin == outPinIndex) return &L;
    return nullptr;
}

int NodeGraph::addNode(NodeKind kind, float x, float y) {
    GraphNode n;
    n.id = nextNodeId++;
    n.kind = kind;
    n.posX = x;
    n.posY = y;
    if (kind == NodeKind::ConstString) n.strA = "hello";
    if (kind == NodeKind::GetItemId) n.strA = "ammo9mm";
    if (kind == NodeKind::ActionLog) n.strA = "graph log";
    if (kind == NodeKind::Randi) {
        n.intA = -6;
        n.intB = 6;
    }
    if (kind == NodeKind::ActionSpawnPickup) {
        n.floatA = 8.0f;
        n.intA = 8;
        n.intD = 24;
    }
    if (kind == NodeKind::ActionSetBlock) {
        n.intA = 4;
        n.intB = 8;
        n.intC = 4;
        n.intD = 1;
    }
    if (kind == NodeKind::GetWorldObject) {
        n.strA = "None";
        n.intA = 0;
    }
    if (kind == NodeKind::HighlightObject) n.floatA = 2.0f;
    if (kind == NodeKind::GetBlock) {
        n.intA = 4;
        n.intB = 8;
        n.intC = 4;
    }
    if (kind == NodeKind::ActionDamagePlayer) {
        n.intA = 0; // peer
        n.floatA = 25.0f;
    }
    if (kind == NodeKind::ActionAnnounce) n.strA = "Hello world";
    if (kind == NodeKind::GetPlayerHealth) n.intA = 0;
    nodes.push_back(n);
    return n.id;
}

void NodeGraph::removeNode(int id) {
    std::erase_if(links, [id](const GraphLink& L) { return L.fromNode == id || L.toNode == id; });
    std::erase_if(nodes, [id](const GraphNode& n) { return n.id == id; });
}

bool NodeGraph::addLink(int fromNode, int fromPin, int toNode, int toPin) {
    if (fromNode == toNode) return false;
    const GraphNode* a = findNode(fromNode);
    const GraphNode* b = findNode(toNode);
    if (!a || !b) return false;
    int ca = 0, cb = 0;
    const GraphPinDesc* pa = nodePinLayout(a->kind, ca);
    const GraphPinDesc* pb = nodePinLayout(b->kind, cb);
    if (fromPin < 0 || fromPin >= ca || toPin < 0 || toPin >= cb) return false;
    if (pa[fromPin].isInput || !pb[toPin].isInput) return false;
    if (!pinsCompatible(pa[fromPin].kind, pb[toPin].kind)) return false;
    // Single link per input pin.
    std::erase_if(links, [toNode, toPin](const GraphLink& L) {
        return L.toNode == toNode && L.toPin == toPin;
    });
    GraphLink L;
    L.id = nextLinkId++;
    L.fromNode = fromNode;
    L.fromPin = fromPin;
    L.toNode = toNode;
    L.toPin = toPin;
    links.push_back(L);
    return true;
}

void NodeGraph::removeLink(int id) {
    std::erase_if(links, [id](const GraphLink& L) { return L.id == id; });
}

NodeGraph NodeGraph::makeExample() {
    NodeGraph g;
    g.name = "main";
    // On Init → Log → Spawn Pickup (x5 via loop is freehand; one spawn + pillar)
    const int eInit = g.addNode(NodeKind::EventOnInit, 40, 40);
    const int log1 = g.addNode(NodeKind::ActionLog, 280, 40);
    g.findNode(log1)->strA = "graph world init";
    const int item = g.addNode(NodeKind::GetItemId, 280, 200);
    g.findNode(item)->strA = "ammo9mm";
    const int spawn = g.addNode(NodeKind::ActionSpawnPickup, 520, 80);
    g.findNode(spawn)->floatA = 8.0f;
    g.findNode(spawn)->intA = 8;
    g.findNode(spawn)->intD = 24;
    const int setb = g.addNode(NodeKind::ActionSetBlock, 760, 80);
    g.findNode(setb)->intA = 4;
    g.findNode(setb)->intB = 10;
    g.findNode(setb)->intC = 4;
    g.findNode(setb)->intD = 1;
    g.addLink(eInit, 0, log1, 0);
    g.addLink(log1, 1, spawn, 0);
    g.addLink(item, 1, spawn, 5); // item id → spawn.item
    g.addLink(spawn, 1, setb, 0);

    const int eTick = g.addNode(NodeKind::EventOnTick, 40, 360);
    const int players = g.addNode(NodeKind::GetPlayerCount, 280, 400);
    const int gt = g.addNode(NodeKind::MathGreater, 440, 400);
    const int zero = g.addNode(NodeKind::ConstInt, 280, 480);
    g.findNode(zero)->intA = 0;
    const int branch = g.addNode(NodeKind::Branch, 620, 360);
    const int logTick = g.addNode(NodeKind::ActionLog, 840, 340);
    g.findNode(logTick)->strA = "players online (graph tick)";
    g.addLink(eTick, 0, branch, 0);
    g.addLink(players, 0, gt, 0);
    g.addLink(zero, 0, gt, 1);
    g.addLink(gt, 2, branch, 1);
    g.addLink(branch, 2, logTick, 0);

    const int eJoin = g.addNode(NodeKind::EventOnPlayerJoin, 40, 600);
    const int logJoin = g.addNode(NodeKind::ActionLog, 280, 600);
    g.findNode(logJoin)->strA = "player joined (graph)";
    g.addLink(eJoin, 0, logJoin, 0);
    return g;
}

std::string saveGraphJson(const NodeGraph& g) {
    json j;
    j["version"] = 1;
    j["name"] = g.name;
    j["nextNodeId"] = g.nextNodeId;
    j["nextLinkId"] = g.nextLinkId;
    json nodes = json::array();
    for (const GraphNode& n : g.nodes) {
        nodes.push_back({{"id", n.id},
                         {"kind", kindToString(n.kind)},
                         {"x", n.posX},
                         {"y", n.posY},
                         {"strA", n.strA},
                         {"intA", n.intA},
                         {"intB", n.intB},
                         {"intC", n.intC},
                         {"intD", n.intD},
                         {"floatA", n.floatA}});
    }
    j["nodes"] = nodes;
    json links = json::array();
    for (const GraphLink& L : g.links) {
        links.push_back({{"id", L.id},
                         {"fromNode", L.fromNode},
                         {"fromPin", L.fromPin},
                         {"toNode", L.toNode},
                         {"toPin", L.toPin}});
    }
    j["links"] = links;
    return j.dump(2);
}

bool loadGraphJson(NodeGraph& out, const std::string& jsonText) {
    try {
        const json j = json::parse(jsonText);
        NodeGraph g;
        g.name = j.value("name", "main");
        g.nextNodeId = j.value("nextNodeId", 1);
        g.nextLinkId = j.value("nextLinkId", 1);
        if (j.contains("nodes")) {
            for (const auto& jn : j["nodes"]) {
                GraphNode n;
                n.id = jn.value("id", 0);
                n.kind = kindFromString(jn.value("kind", "ActionLog"));
                n.posX = jn.value("x", 0.0f);
                n.posY = jn.value("y", 0.0f);
                n.strA = jn.value("strA", "");
                n.intA = jn.value("intA", 0);
                n.intB = jn.value("intB", 0);
                n.intC = jn.value("intC", 0);
                n.intD = jn.value("intD", 0);
                n.floatA = jn.value("floatA", 0.0f);
                if (n.id > 0) g.nodes.push_back(n);
            }
        }
        if (j.contains("links")) {
            for (const auto& jl : j["links"]) {
                GraphLink L;
                L.id = jl.value("id", 0);
                L.fromNode = jl.value("fromNode", 0);
                L.fromPin = jl.value("fromPin", 0);
                L.toNode = jl.value("toNode", 0);
                L.toPin = jl.value("toPin", 0);
                if (L.id > 0) g.links.push_back(L);
            }
        }
        out = std::move(g);
        return true;
    } catch (const std::exception& e) {
        log::error("NodeGraph: JSON parse failed: {}", e.what());
        return false;
    }
}

std::string emitGraphLua(const NodeGraph& g) {
    std::ostringstream out;
    out << "-- AUTO-GENERATED from node graph '" << g.name
        << "' — edit the graph in the Node Graph panel, not this file.\n";
    out << "-- C6 visual scripting (node graph) → sandboxed Lua (ScriptHost game.* API).\n\n";
    emitEventFunction(out, g, NodeKind::EventOnInit, "on_init", "seed");
    emitEventFunction(out, g, NodeKind::EventOnTick, "on_tick", "t");
    emitEventFunction(out, g, NodeKind::EventOnPlayerJoin, "on_player_join", "peer");
    emitEventFunction(out, g, NodeKind::EventOnPlayerDeath, "on_player_death", "peer");
    return out.str();
}

} // namespace meat
