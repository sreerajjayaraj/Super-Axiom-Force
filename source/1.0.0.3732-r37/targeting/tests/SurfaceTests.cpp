#include <cstdio>
#include <limits>
#include "../src/NearbySurface.h"

int main() {
    using namespace axiom::surface;
    unsigned checks=0, failures=0;
    const auto check=[&](bool condition,const char* detail) {
        ++checks;
        if (!condition) { ++failures; std::printf("FAIL %s\n",detail); }
    };
    const Point origin{-9831.f,665.f,226.f}, end{39169.f,665.f,226.f};
    const Point red{-9760.f,665.f,226.f};
    Point probe{};
    check(ProbeEnd(origin,end,red,0,probe) && probe[0]>red[0] && probe[1]!=red[1],
          "first probe offsets the ray near the red point");
    check(ProbeEnd(origin,end,red,1,probe) && probe[1]>red[1],
          "second probe covers the opposite side");
    check(ProbeEnd(origin,Point{-9760.f,665.f,226.f},red,7,probe) &&
          Distance(origin,probe)<=71.001f,"probe stays within configured ray");
    check(ProbeEnd(Point{0,0,0},Point{0,500,0},Point{0,80,0},4,probe),
          "vertical aim has a defined tangent");
    check(!ProbeEnd(origin,origin,red,0,probe) &&
          !ProbeEnd(origin,end,origin,0,probe),"invalid or too-close hits do not retry");
    auto invalid=red;
    invalid[0]=std::numeric_limits<float>::infinity();
    check(!ProbeEnd(origin,end,invalid,0,probe),"non-finite native point refuses");
    check(Eligible(origin,red,Point{-9760.1f,665.3f,226.f},true,50000.f),
          "nearby first-hit with native body can be retried by game");
    check(!Eligible(origin,red,Point{-9760.f,665.f,226.f},false,50000.f),
          "bodyless probe keeps the red candidate");
    check(!Eligible(origin,red,Point{-9756.f,665.f,226.f},true,50000.f),
          "rejects a hit beyond the local search volume");
    check(!Eligible(origin,red,Point{-9761.f,665.f,226.f},true,69.f),
          "rejects a hit beyond the original ray budget");
    check(ProbeEnd(origin,end,red,8,probe) && Distance(probe,red)>1.f,
          "distant retry reaches a wider native collision sample");
    check(Eligible(origin,red,Point{-9760.f,666.25f,226.f},true,50000.f),
          "distant retry can recover adjacent collision outside the old search radius");
    check(!ProbeEnd(origin,end,Point{-9800.f,665.f,226.f},8,probe),
          "expanded recovery preserves stock close aiming behavior");
    check(!Eligible(origin,red,Point{-9760.f,667.1f,226.f},true,50000.f),
          "expanded recovery rejects targets farther than two units from red");
    const Point grid{-9696.f,665.f,226.f};
    check(ProbeEnd(origin,end,grid,16,probe) && probe[0]>grid[0]+8.f &&
          Distance(origin,probe)<150.f,"distant grid retry reaches behind the old two-unit search");
    check(!ProbeEnd(origin,end,Point{-9695.5f,665.f,226.f},16,probe),
          "wider ray cannot open an offset search around ordinary geometry");
    check(ProbeEnd(origin,Point{-9693.f,665.f,226.f},grid,16,probe) &&
          Distance(origin,probe)<=138.001f,"longer offset ray stays within the active search budget");
    check(SameAnchor(red,Point{-9760.f,665.01f,226.f}) &&
          !SameAnchor(red,Point{-9759.8f,665.f,226.f}),"committed anchor match tolerates only native rounding");
    check(SurfaceAttached(2,3,1,88.5f) && SurfaceAttached(2,3,2,88.5f),
          "new surface state with finite native distance logs once");
    check(!SurfaceAttached(3,3,1,88.5f) && !SurfaceAttached(2,3,6,88.5f) &&
          !SurfaceAttached(2,3,1,0.f),"unchanged stage and invalid surface observations decline");
    std::printf("Nearby Axiom surface %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}


