// libultra internals N64Recomp routes to the runtime (its "ignored" list) that the runtime does not define.
// Only replaced libultra code calls them; each returns 0. Regenerated from link errors (see tools/README).
#include "recomp.h"

extern "C" {
void __osCheckPackId_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 0; }
void __osEnqueueThread_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 0; }
void __osPfsRWInode_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 0; }
void __osPopThread_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 0; }
void __osRepairPackId_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 0; }
void __osViSwapContext_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 0; }
void osMapTLBRdb_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 0; }
}
