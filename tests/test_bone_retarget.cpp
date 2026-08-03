// Cross-skeleton bone matching: the retargeter can only transfer a clip's joint
// onto the target rig if both names reduce to the same canonical key. This is
// what lets Mixamo, UE4-mannequin, and UE5-mannequin animations all drive one
// canonical (Mixamo) skeleton with no per-asset configuration.

#include "Harness.h"

#include "engine/asset/SkeletalModel.h"

#include <cstdio>
#include <string>

namespace {

using meattest::check;

void testUeMapsToMixamoCanonical() {
    std::printf("UE4/UE5 mannequin joints canonicalize to their Mixamo counterparts\n");
    // A UE-authored clip drives e.g. "thigh_l"; the Mixamo rig has "LeftUpLeg".
    // If these don't reduce to the same key, the retarget maps that joint to
    // nothing. UE4 and UE5 mannequins share these core names, so this table
    // covers both. (Mixamo sides given with and without the mixamorig: prefix.)
    struct Pair {
        const char* ue;
        const char* mixamo;
    };
    const Pair pairs[] = {
        {"pelvis", "mixamorig:Hips"},        {"spine_01", "Spine"},
        {"spine_03", "mixamorig:Spine2"},    {"neck_01", "Neck"},
        {"clavicle_l", "LeftShoulder"},      {"upperarm_l", "mixamorig:LeftArm"},
        {"lowerarm_r", "RightForeArm"},      {"hand_l", "mixamorig:LeftHand"},
        {"thigh_l", "LeftUpLeg"},            {"calf_r", "mixamorig:RightLeg"},
        {"foot_r", "RightFoot"},             {"ball_l", "LeftToeBase"},
    };
    bool allMatch = true;
    for (const Pair& p : pairs) {
        if (meat::canonicalBoneName(p.ue) != meat::canonicalBoneName(p.mixamo)) {
            std::printf("        [mismatch] %s(%s) != %s(%s)\n", p.ue,
                        meat::canonicalBoneName(p.ue).c_str(), p.mixamo,
                        meat::canonicalBoneName(p.mixamo).c_str());
            allMatch = false;
        }
    }
    check(allMatch, "every UE mannequin joint bridges to its Mixamo joint");
}

void testMixamoIsUnchanged() {
    std::printf("Mixamo names reduce to themselves (Mixamo<->Mixamo unaffected)\n");
    check(meat::canonicalBoneName("mixamorig:LeftUpLeg") == meat::canonicalBoneName("LeftUpLeg"),
          "the mixamorig: namespace is stripped to the same key");
    check(meat::canonicalBoneName("mixamorig:Hips") == "hips",
          "a Mixamo joint maps to its lowercase key");
}

void testDistinctAndUnknownBones() {
    std::printf("distinct joints stay distinct; unknown bones pass through\n");
    // A custom / non-humanoid bone must not collide with a mapped joint.
    check(meat::canonicalBoneName("weapon_socket") == "weapon_socket",
          "an unmapped bone passes through unchanged");
    check(meat::canonicalBoneName("thigh_l") != meat::canonicalBoneName("calf_l"),
          "different joints do not collapse to the same key");
    check(meat::canonicalBoneName("thigh_l") != meat::canonicalBoneName("thigh_r"),
          "left and right stay distinct");
}

} // namespace

namespace meattest {

void runBoneRetarget() {
    testUeMapsToMixamoCanonical();
    testMixamoIsUnchanged();
    testDistinctAndUnknownBones();
}

} // namespace meattest
