/* N64Recomp trace_mode hook (recomp/thps.toml): each recompiled function records itself as its thread's
   latest entry; port/src/glue.cpp's watchdog (THPS_WATCH=1) prints them, which locates spins and stalls. */
#ifndef THPS_TRACE_H
#define THPS_TRACE_H
#ifdef __cplusplus
extern "C" {
#endif
void thps_trace(const char* name, void* ctx);
#ifdef __cplusplus
}
#endif
#define TRACE_ENTRY() thps_trace(__func__, ctx);
#define TRACE_RETURN()
#endif
