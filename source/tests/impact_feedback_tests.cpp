#include "wardogs/impact_feedback.hpp"
#include "wardogs/firing_analysis.hpp"
#include <array>
#include <iostream>
#include <limits>

int main() {
    using namespace wardogs;
    int failures{};
    const auto check = [&](bool good, const char* name) { if (!good) { ++failures; std::cerr << name << '\n'; } };
    const auto close = [&](double a, double b, const char* name) { check(std::abs(a-b) < 1e-8, name); };
    const Point base{80, 80};
    for (const Point target : std::array<Point, 8>{{{80,95},{95,95},{95,80},{95,65},{80,65},{65,65},{65,80},{65,95}}}) {
        const auto command = corrected_solution(base, target, {identity_rotation(),0}, Arc::low);
        const FiringSnapshot firing{target, Arc::low, command.bearing_deg, command.mil, 0};
        const auto report = impact_feedback(base, firing, offset_target(base, target, 30, -40), command);
        close(report.right_m,30,"rightward miss uses the gun-target frame at every azimuth");
        close(report.far_m,-40,"short miss is negative in metres at every azimuth");
        close(report.miss_m,50,"total miss is Euclidean metres");
        close(report.bearing_change_deg,0,"unchanged command has no azimuth delta");
        close(report.mil_change,0,"unchanged command has no MIL delta");
    }
    for (const Arc arc : {Arc::low,Arc::high}) {
        const Point target{80,100};
        const auto initial = corrected_solution(base,target,{identity_rotation(),0},arc);
        FiringSnapshot firing{target,arc,359.9,initial.mil,0};
        auto next = initial;
        next.bearing_deg = 0.1;
        next.mil = sph2_mil_for_distance(2100,arc);
        const auto report = impact_feedback(base,firing,target,next);
        close(report.bearing_change_deg,.2,"azimuth delta wraps through north by the shortest direction");
        close(report.table_range_change_m,100,"table metres correspond to the final MIL rather than target movement");
        check(arc == Arc::low ? report.mil_change>0 : report.mil_change<0,
              "farther uses increasing low-arc MIL and decreasing high-arc MIL");
    }
    bool rejected{};
    try { const auto next=corrected_solution(base,{80,100},{identity_rotation(),0},Arc::low);
        (void)impact_feedback(base,{{80,100},Arc::low,0,next.mil,0},
                             {std::numeric_limits<double>::quiet_NaN(),80},next); }
    catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"non-finite observations cannot enter the visible ranging report");
    rejected=false;
    try { auto next=corrected_solution(base,{80,100},{identity_rotation(),0},Arc::low);
        next.bearing_deg=-1e308;
        (void)impact_feedback(base,{{80,100},Arc::low,1e308,next.mil,0},{80,100},next); }
    catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"overflow of finite bearing differences is rejected");
    std::cout << "Impact feedback geometry, arc direction and north-wrap checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
