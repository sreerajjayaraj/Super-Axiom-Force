#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace axiom::surface {
using Point=std::array<float,3>;
inline constexpr unsigned kLocalProbeCount=16;
inline constexpr unsigned kProbeCount=24;
inline constexpr unsigned kContinuationProbe=kProbeCount;
inline constexpr unsigned kWindowProbe=kProbeCount+1;
inline constexpr unsigned kAllHitsProbe=kProbeCount+2;
inline constexpr unsigned kNearCameraProbe=kProbeCount+3;
inline constexpr unsigned kNoProbe=kProbeCount+4;
inline constexpr float kContinuationWindow=384.f;
inline constexpr float kSearchRadius=2.f;
inline constexpr float kWideSearchDistance=60.f;
inline constexpr float kNearSearchRadius=0.8f;
inline constexpr float kProbeDepth=0.7f;
inline constexpr float kGridSize=32.f;
inline constexpr float kGridTolerance=0.002f;

enum class Recovery : uint8_t { None, Nearby, OriginalRay, ExtendedRay, ContinuedRay, WindowRay, AllHitsRay, NearCameraRay };

inline const char* RecoveryName(Recovery mode) {
    switch (mode) {
    case Recovery::Nearby: return "nearby";
    case Recovery::OriginalRay: return "original-ray";
    case Recovery::ExtendedRay: return "extended-ray";
    case Recovery::ContinuedRay: return "continued-ray";
    case Recovery::WindowRay: return "camera-window-ray";
    case Recovery::AllHitsRay: return "all-hits-camera-ray";
    case Recovery::NearCameraRay: return "near-camera-ray";
    default: return "none";
    }
}

inline Point Difference(const Point& a,const Point& b) {
    return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};
}
inline float Length(const Point& v) {
    return std::hypot(v[0],v[1],v[2]);
}
inline float Distance(const Point& a,const Point& b) {
    return Length(Difference(a,b));
}
inline Point Cross(const Point& a,const Point& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
inline bool GridBoundary(const Point& point) {
    return std::isfinite(point[1]) &&
        (std::abs(std::remainder(point[0],kGridSize))<=kGridTolerance ||
         std::abs(std::remainder(point[2],kGridSize))<=kGridTolerance);
}

inline float Dot(const Point& a,const Point& b) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}

inline float GridDepth(float distance) {
    return (std::min)(12.f,0.10f*distance);
}

inline float GridWidth(float distance) {
    return (std::min)(4.f,0.028f*distance);
}

inline bool EligibleBeyondGrid(const Point& origin,const Point& end,const Point& red,
                               const Point& hit,float limit,bool originalRay) {
    const float depth=Distance(origin,red);
    const float distance=Distance(origin,hit);
    const auto ray=Difference(end,origin);
    const float length=Length(ray);
    if (!GridBoundary(red) || !(depth>=kWideSearchDistance && distance<=limit &&
                                std::isfinite(length) && length>=depth)) return false;
    const Point direction{ray[0]/length,ray[1]/length,ray[2]/length};
    if (!(Length(Cross(Difference(red,origin),direction))<=0.15f)) return false;
    const auto offset=Difference(hit,origin);
    const float advance=Dot(Difference(hit,red),direction);
    const float lateral=Length(Cross(offset,direction));
    const float lateralLimit=originalRay?0.15f:GridWidth(depth)+0.02f;
    // An existing hit on the exact aim ray is bounded by the verified native
    // ray and SAF range. The short depth applies only to off-axis retries.
    return advance>=0.025f && (originalRay || advance<=GridDepth(depth)+0.01f) &&
           lateral<=lateralLimit;
}

inline bool DirectionalGridMarker(const Point& origin,const Point& end,const Point& point,
                                  uint32_t shapeKey,float limit) {
    // The native results use an unresolved child key and a directional
    // face. Observed faces 0/2 are -X/+X and 1/3 are +Z/-Z.
    if ((shapeKey&0x1FFFFFFFu)!=0x1FFFFFFFu || shapeKey>=0x80000000u) return false;
    const auto face=shapeKey>>29;
    const unsigned axis=(face&1u)?2u:0u;
    const bool positive=face==1u || face==2u;
    const auto delta=Difference(end,origin);
    if (!(positive?delta[axis]>0.f:delta[axis]<0.f) ||
        !(std::abs(std::remainder(point[axis],kGridSize))<=kGridTolerance)) return false;

    const float length=Length(delta);
    const auto offset=Difference(point,origin);
    const float distance=Length(offset);
    if (!(std::isfinite(length) && distance>=kWideSearchDistance && distance<=limit &&
          distance<=length)) return false;
    const Point direction{delta[0]/length,delta[1]/length,delta[2]/length};
    return Dot(offset,direction)>0.f && Length(Cross(offset,direction))<=0.15f;
}

inline bool ContinueRay(const Point& origin,const Point& end,const Point& boundary,float limit,
                        Point& start,Point& finish) {
    const auto ray=Difference(end,origin);
    const float length=Length(ray);
    if (!(std::isfinite(length) && length>0.f && limit<=length+0.01f)) return false;
    const Point direction{ray[0]/length,ray[1]/length,ray[2]/length};
    const auto offset=Difference(boundary,origin);
    const float after=Dot(offset,direction)+0.03125f;
    if (!(after>=kWideSearchDistance && after<limit &&
          Length(Cross(offset,direction))<=0.15f)) return false;
    for (size_t i=0;i<start.size();++i) {
        start[i]=origin[i]+direction[i]*after;
        finish[i]=origin[i]+direction[i]*limit;
    }
    return true;
}

// Keep every recovered anchor within the native aim-ray precision.
inline bool OnAimRay(const Point& origin,const Point& end,const Point& hit,float limit) {
    const auto ray=Difference(end,origin);
    const auto offset=Difference(hit,origin);
    const float length=Length(ray),depth=Length(offset);
    if (!(length>0.f && std::isfinite(length) && depth>=2.f && depth<=limit+0.005f))
        return false;
    const Point direction{ray[0]/length,ray[1]/length,ray[2]/length};
    return Dot(offset,direction)>=2.f && Length(Cross(offset,direction))<=0.15f;
}

inline bool ProbeEnd(const Point& origin,const Point& end,const Point& red,unsigned probe,Point& result) {
    const auto ray=Difference(end,origin);
    const float limit=Length(ray);
    const float depth=Distance(origin,red);
    if (!std::isfinite(limit) || !std::isfinite(depth) || depth<2.f || depth>limit ||
        probe>=kProbeCount || (probe>=8 && depth<kWideSearchDistance)) return false;
    const bool extended=probe>=kLocalProbeCount;
    if (extended && !GridBoundary(red)) return false;

    const Point direction{ray[0]/limit,ray[1]/limit,ray[2]/limit};
    // World Y is up. Pick another axis when looking vertically.
    const Point axis=std::abs(direction[1])<0.95f?Point{0,1,0}:Point{0,0,1};
    auto tangent=Cross(direction,axis);
    const float tangentLength=Length(tangent);
    for (auto& axisValue : tangent) axisValue/=tangentLength;
    const auto other=Cross(direction,tangent);
    const bool wide=probe>=8;
    const auto& offsetAxis=(probe&2)?tangent:other;
    const float width=extended?GridWidth(depth):probe<4?0.10f:probe<8?0.42f:1.25f;
    const bool diagonal=extended?probe>=20:probe>=12;
    const float lateral=extended && diagonal?width*0.70710678f:width;
    const float offset=lateral*(probe&1?-1.f:1.f);
    const float cross=diagonal?((probe&2)?-lateral:lateral):0.f;
    const float forward=(std::min)(depth+(extended?GridDepth(depth):wide?1.15f:kProbeDepth),limit);
    Point candidate{};
    for (unsigned i=0;i<3;++i) candidate[i]=direction[i]*forward+offsetAxis[i]*offset+
        (probe&2?other[i]:tangent[i])*cross;
    const float cap=(std::min)(1.f,limit/Length(candidate));
    for (unsigned i=0;i<3;++i) result[i]=origin[i]+candidate[i]*cap;
    return result!=end;
}
inline bool Eligible(const Point& origin,const Point& red,const Point& hit,bool body,float limit) {
    const float depth=Distance(origin,red);
    const float radius=depth>=kWideSearchDistance?kSearchRadius:kNearSearchRadius;
    if (!body || !(Distance(red,hit)<=radius)) return false;
    const float distance=Distance(origin,hit);
    return distance>=2.f && distance<=limit;
}
inline bool SameAnchor(const Point& recovered,const Point& native) {
    // The stationary world anchor is reconstructed from native packed
    // transforms. Allow float rounding, with a five-centimeter ceiling.
    return Distance(recovered,native)<=0.05f;
}
inline bool SurfaceAttached(unsigned before,unsigned after,unsigned kind,float distance) {
    return before==2 && after==3 &&
           (kind==1 || kind==2) && std::isfinite(distance) && distance>0.f;
}
}


