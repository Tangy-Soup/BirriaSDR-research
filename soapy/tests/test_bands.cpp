/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 * Band walk and gap check of the SoapySDR module (soapy/bands.hpp), against a
 * fake birriasdr_get_tuning_band. No SoapySDR runtime or device is used.
 */
#include "../bands.hpp"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>

#define CHECK(t) do { if (!(t)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #t); std::exit(1); } } while (0)

using birria_soapy::Band;
using birria_soapy::collect_bands;
using birria_soapy::in_band;

/* Answers like the library: listed bands, UNSUPPORTED where marked, then INVALID. */
struct Fake {
    std::vector<Band> bands;
    std::vector<int> results;   /* per index; 0 means report bands[index] */
    unsigned calls = 0;
    int operator()(uint32_t index, uint64_t *minimum, uint64_t *maximum)
    {
        ++calls;
        if (index >= results.size())
            return BIRRIASDR_ERROR_INVALID;
        if (results[index] < 0)
            return results[index];
        *minimum = bands[index].minimum;
        *maximum = bands[index].maximum;
        return 0;
    }
};

int main()
{
    const Band low{UINT64_C(2156000000), UINT64_C(2610000000)};
    const Band high{UINT64_C(4500000000), UINT64_C(6200000000)};

    Fake mt7921{{low, high}, {0, 0}};
    std::vector<Band> bands = collect_bands(std::ref(mt7921));
    CHECK(bands.size() == 2 && mt7921.calls == 3);
    CHECK(bands[0].minimum == low.minimum && bands[0].maximum == low.maximum);
    CHECK(bands[1].minimum == high.minimum && bands[1].maximum == high.maximum);

    /* Edges are inclusive and requests round to whole hertz first. */
    CHECK(in_band(bands, 2437e6) == UINT64_C(2437000000));
    CHECK(in_band(bands, 2437123456.4) == UINT64_C(2437123456));
    CHECK(in_band(bands, 2156e6) == low.minimum && in_band(bands, 2610e6) == low.maximum);
    CHECK(in_band(bands, 4500e6) == high.minimum && in_band(bands, 6200e6) == high.maximum);
    CHECK(in_band(bands, 6200000000.4) == high.maximum);
    CHECK(in_band(bands, 2155999999.4) == 0);
    CHECK(in_band(bands, 6200000000.6) == 0);
    /* The gap between the bands, and requests no tuner can take. */
    CHECK(in_band(bands, 2610000001.0) == 0);
    CHECK(in_band(bands, 3000e6) == 0);
    CHECK(in_band(bands, 4499999999.0) == 0);
    CHECK(in_band(bands, 100e6) == 0);
    CHECK(in_band(bands, 0.0) == 0 && in_band(bands, -2437e6) == 0);
    CHECK(in_band(bands, std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(in_band(bands, std::numeric_limits<double>::infinity()) == 0);
    CHECK(in_band(bands, 1e300) == 0);

    /* A band this board cannot reach is skipped, not the end of the list. */
    Fake capped{{low, high}, {0, BIRRIASDR_ERROR_UNSUPPORTED}};
    bands = collect_bands(std::ref(capped));
    CHECK(bands.size() == 1 && bands[0].maximum == low.maximum);
    CHECK(in_band(bands, 5180e6) == 0);
    Fake lower{{low, high}, {BIRRIASDR_ERROR_UNSUPPORTED, 0}};
    bands = collect_bands(std::ref(lower));
    CHECK(bands.size() == 1 && bands[0].minimum == high.minimum);

    /* Another error ends the walk with what was found; none found means empty. */
    Fake broken{{low, high}, {0, BIRRIASDR_ERROR_IO, 0}};
    bands = collect_bands(std::ref(broken));
    CHECK(bands.size() == 1 && broken.calls == 2);
    Fake closed{{}, {BIRRIASDR_ERROR_INVALID}};
    CHECK(collect_bands(std::ref(closed)).empty());
    CHECK(in_band({}, 2437e6) == 0);

    /* A backend that never says INVALID stops at the cap. */
    unsigned calls = 0;
    bands = collect_bands([&](uint32_t, uint64_t *minimum, uint64_t *maximum) {
        ++calls;
        *minimum = low.minimum;
        *maximum = low.maximum;
        return 0;
    });
    CHECK(calls == 64 && bands.size() == 64);

    std::puts("Soapy band walk and gap checks passed");
    return 0;
}
