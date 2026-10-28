#include <atomic>

#ifndef LTXSHARED_STATE
#define LTXSHARED_STATE

/* Legacy fallback for Bonsai streams without camera_tracker metadata. The
   viewer supplies these settings; metadata-aware streams are read directly by
   the recorder and do not write these globals. */
namespace LTX {
    namespace SharedState {
        extern std::atomic<int> window_min_x;
        extern std::atomic<int> window_max_x;
        extern std::atomic<int> window_max_y;
        extern std::atomic<int> window_min_y;
        extern std::atomic<float> pixels_per_metre;
    }
}

#endif