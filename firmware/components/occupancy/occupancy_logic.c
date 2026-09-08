/* occupancy_logic.c — pure vacancy-timeout logic (no hardware, host-testable). */
#include "occupancy.h"

bool occupancy_from_motion(uint64_t now_ms, uint64_t last_motion_ms, uint32_t timeout_s)
{
    if (now_ms < last_motion_ms) return true;         /* clock anomaly -> occupied */
    return (now_ms - last_motion_ms) < (uint64_t)timeout_s * 1000ULL;
}
