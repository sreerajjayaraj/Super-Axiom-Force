#pragma once
#include "NearbySurface.h"

namespace axiom::stall {
inline constexpr float kProgressDistance=0.35f;
inline constexpr float kReleaseDelaySeconds=0.65f;
inline constexpr uint64_t kMaxSampleGapMs=200;
inline constexpr float kArrivalDistance=3.0f;

inline bool RetractionRequested(uint16_t lengthRate,uint16_t reelRate) {
    const auto positive=[](uint16_t half){return half>0 && half<0x7C00;};
    return positive(lengthRate) || positive(reelRate);
}

struct MotionWindow {
    surface::Point start{};
    uint64_t startedTick=0,latestTick=0;
    float elapsed=0;
    unsigned samples=0;

    bool Observe(uint64_t tick,float delta,const surface::Point& position,
                 const surface::Point& anchor,bool retracting,unsigned waypoints) {
        const float distance=surface::Distance(position,anchor);
        if (!retracting || !waypoints || !(delta>0 && delta<=0.1f) ||
            !std::isfinite(distance) || distance<=kArrivalDistance) {
            *this={};
            return false;
        }
        if (!samples || tick<latestTick || tick-latestTick>kMaxSampleGapMs ||
            surface::Distance(position,start)>=kProgressDistance) {
            *this={position,tick,tick,0,1};
            return false;
        }
        latestTick=tick;
        elapsed+=delta;
        ++samples;
        return samples>=8 && elapsed>=kReleaseDelaySeconds && tick-startedTick>=650;
    }
};
}
