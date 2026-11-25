#pragma once
#include <cstddef>
#include <cstdint>

namespace saf_targeting {

using LogSink=void(*)(const char*);
struct State {
    uintptr_t support=0;
    float range=0.f;
    bool straightPull=false;
};
enum class Result { Inactive, Active, Retry, Unavailable, Conflict };

// Owned by the existing serial SAF worker. A zero state disables searching
// and restores the four extra distance references before SAF releases reach.
bool Prepare(LogSink sink);
#ifdef SAF_MAPPED_IMAGE_TESTING
void PrepareMappedImage(LogSink sink,uintptr_t base,size_t size,uintptr_t pointerBase);
bool MappedImageFeaturesReady();
#endif
Result Sync(const State& state);

}
