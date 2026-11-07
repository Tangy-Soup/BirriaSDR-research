/* Host-only: the automatic-mode gain loop's decisions, on a simulated clock. */
#include "../HostAgc.hpp"
#include <cstdlib>
#include <iostream>
#define CHECK(t) do { if (!(t)) { std::cerr << "line " << __LINE__ << ": " << #t << "\n"; std::exit(1); } } while (0)
using birria_soapy::AgcStats;
using birria_soapy::HostAgc;

/* The steps of chain 0 at 2.4 GHz, analog only. */
static const std::vector<int> STEPS = {0, 81, 138, 197, 258, 318, 339, 358, 377, 398, 415, 435, 457, 474};
constexpr double TICK = 0.25;
constexpr uint64_t VALUES = 20000000; /* 40 MSps, I and Q, per tick */

/* A signal at `level_tenths` (above the lowest code's input) through gain step g. */
static AgcStats input(int gain_tenths, int level_tenths, double clip_fraction_if_over = 0.01)
{
    const double peak = 8191.0 * std::pow(10.0, (gain_tenths + level_tenths) / 200.0);
    AgcStats s;
    s.values = VALUES;
    if (peak >= 8191.0) {
        s.peak = 8191;
        s.clipped = (uint64_t)(clip_fraction_if_over * (double)VALUES);
    } else {
        s.peak = (int)peak;
    }
    return s;
}

struct Run { HostAgc agc; double now = 0; int changes = 0; };

static void advance(Run &r, double seconds, int level_tenths)
{
    for (double end = r.now + seconds; r.now < end; r.now += TICK)
        if (r.agc.update(input(r.agc.tenths(), level_tenths), r.now))
            ++r.changes;
}

int main()
{
    /* Weak signals: stays at the top and never moves. */
    Run quiet;
    quiet.agc.start(STEPS, STEPS.size() - 1, 0);
    advance(quiet, 600, -600);
    CHECK(quiet.agc.index() == STEPS.size() - 1 && quiet.changes == 0);

    /* A signal 10 dB above full scale at the top: drops within a few seconds and settles unclipped. */
    Run strong;
    strong.agc.start(STEPS, STEPS.size() - 1, 0);
    advance(strong, 10, -374);
    CHECK(strong.agc.tenths() <= 474 - 60 && strong.changes >= 1);
    const int settled = strong.agc.tenths();
    CHECK(settled - 374 < 0);

    /* Hysteresis: the same steady signal for ten minutes causes no further change. */
    const int changes = strong.changes;
    advance(strong, 600, -374);
    CHECK(strong.changes - changes <= 1);
    const int after = strong.changes;
    advance(strong, 600, -374);
    CHECK(strong.changes == after);
    CHECK(strong.agc.tenths() >= settled);

    /* The signal goes away: climbs back, but not before the up window. */
    const int before_up = strong.agc.tenths();
    advance(strong, 20, -700);
    CHECK(strong.agc.tenths() == before_up);
    advance(strong, 200, -700);
    CHECK(strong.agc.index() == STEPS.size() - 1);

    /* Rate limit: an always-clipping input changes at most once per down window. */
    Run hard;
    hard.agc.start(STEPS, STEPS.size() - 1, 0);
    advance(hard, 9, 300);
    CHECK(hard.changes >= 2 && hard.changes <= 5);
    advance(hard, 30, 300);
    CHECK(hard.agc.index() == 0);

    /* Clipping just below the threshold does not move it; just above does. */
    {
        HostAgc agc;
        agc.start(STEPS, STEPS.size() - 1, 0);
        double t = 0;
        bool moved = false;
        for (; t < 5; t += TICK) {
            AgcStats s;
            s.values = VALUES;
            s.peak = 8191;
            s.clipped = VALUES / 2000; /* 5e-4 */
            moved = agc.update(s, t) || moved;
        }
        CHECK(!moved);
        for (; t < 10; t += TICK) {
            AgcStats s;
            s.values = VALUES;
            s.peak = 8191;
            s.clipped = VALUES / 500; /* 2e-3 */
            moved = agc.update(s, t) || moved;
        }
        CHECK(moved);
    }

    /* A stalled stream reports nothing and must not raise the gain. */
    {
        HostAgc agc;
        agc.start(STEPS, 3, 0);
        for (double t = 0; t < 120; t += TICK)
            CHECK(!agc.update(AgcStats{}, t));
        CHECK(agc.index() == 3);
    }

    /* Empty tables are inert. */
    {
        HostAgc agc;
        agc.start({}, 0, 0);
        CHECK(!agc.update(input(0, 0), 10));
        CHECK(agc.tenths() == 0);
    }

    /* The scan: magnitudes, including the most negative value. */
    {
        const int16_t iq[] = {0, 5, -8190, 12, 8180, -32768, 7, -3};
        AgcStats s;
        birria_soapy::agc_scan(iq, 8, 8180, s);
        CHECK(s.values == 8 && s.clipped == 3 && s.peak == 32768);
    }
    std::cout << "PASS host AGC decisions\n";
}
