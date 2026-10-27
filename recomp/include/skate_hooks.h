// thpSkate3 hooks injected into recompiled THPS functions (recomp/thps.toml [[patches.hook]]).
// Each returns non-zero when it handled the call, and the hooked function then returns at once.
#pragma once
#include <stdint.h>
#include "recomp.h"
#ifdef __cplusplus
extern "C" {
#endif
// func_80059BEC (skater physics/state machine), a0 = skater: skatecore steps the skater instead.
int thps_skate_physics(uint8_t* rdram, recomp_context* ctx);
// func_80073EA0 (camera -> view record D_800DE1A4), a0 = camera object: the Skate 3 camera instead.
int thps_skate_camera(uint8_t* rdram, recomp_context* ctx);
// func_80084CB8 (play the animation, write the bone matrices), at entry and just before it returns: the THPS
// character is posed from skatecore's skeleton.
void thps_skate_pose_begin(uint8_t* rdram, recomp_context* ctx);
void thps_skate_pose_end(uint8_t* rdram, recomp_context* ctx);
#ifdef __cplusplus
}
#endif
