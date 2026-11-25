
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace axiom {
inline uint32_t FloatBits(float value) {
    uint32_t bits=0;
    std::memcpy(&bits,&value,sizeof(bits));
    return bits;
}

inline constexpr uint32_t kNativeDistance=0x40800000u; // 4.0

struct Settings {
    uint32_t enabled=0, unlimited=0;
    float range=100.0f;
};

struct Operation {
    uint32_t expected=0, desired=0;
    bool write=false, stop=false;
};

class DistanceOwner {
    uint32_t last_=0;
    bool stopped_=false;
public:
    Operation Decide(uint32_t current,const Settings& settings,bool providersReady) {
        if (stopped_) return {0,0,false,true};
        if (last_ && current!=last_) {
            last_=0;
            stopped_=true;
            return {0,0,false,true};
        }
        const bool valid=providersReady && settings.enabled==1 && settings.unlimited<=1 &&
                         std::isfinite(settings.range);
        if (!valid) return {current,kNativeDistance,last_!=0,false};
        if (!last_ && current!=kNativeDistance) return {};
        const float distance=settings.unlimited ? 50000.f :
            (settings.range>=1.f && settings.range<=100000.f ? settings.range : 100.f);
        const auto desired=FloatBits(distance);
        return {current,desired,current!=desired,false};
    }

    void Complete(const Operation& operation,bool success) {
        if (operation.write && success)
            last_=operation.desired==kNativeDistance ? 0 : operation.desired;
    }
    bool Owned() const { return last_!=0; }
};
}

