#pragma once

// Single point of contact between project code and Tracy.
// When TRACY_ENABLE is undefined (the default build), every macro
// below expands to a no-op so instrumentation sites cost nothing.

#ifdef TRACY_ENABLE
    #include "../../../deps/tracy/Tracy.hpp"

    #define PROFILE_FRAME_MARK()        FrameMark
    #define PROFILE_ZONE()              ZoneScoped
    #define PROFILE_ZONE_N(name)        ZoneScopedN(name)
    #define PROFILE_PLOT_I64(name, v)   TracyPlot(name, int64_t(v))
    #define PROFILE_PLOT_F32(name, v)   TracyPlot(name, float(v))
    #define PROFILE_THREAD_NAME(name)   tracy::SetThreadName(name)
#else
    #define PROFILE_FRAME_MARK()        ((void)0)
    #define PROFILE_ZONE()              ((void)0)
    #define PROFILE_ZONE_N(name)        ((void)0)
    #define PROFILE_PLOT_I64(name, v)   ((void)0)
    #define PROFILE_PLOT_F32(name, v)   ((void)0)
    #define PROFILE_THREAD_NAME(name)   ((void)0)
#endif
