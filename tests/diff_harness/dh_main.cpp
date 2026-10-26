/* Differential-harness runner:
 *   dh_diff --out report.json [--image relocated.bin] [--filter substr] [-v]
 *           [--disable-dimension NAME]...
 * The report is one JSON document (schema ps3recomp.diffharness/1), written
 * through a .partial file and renamed into place. */
#include "dh_engine.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct DimName { const char* name; uint32_t bit; };
const DimName kDims[] = {
    {"return", dh::DIM_RETURN}, {"writes", dh::DIM_WRITES}, {"extra", dh::DIM_EXTRA},
    {"imports", dh::DIM_IMPORTS}, {"faults", dh::DIM_FAULTS}, {"fault-span", dh::DIM_FAULT_SPAN},
    {"hang", dh::DIM_HANG},
};
}  // namespace

int main(int argc, char** argv)
{
    dh::Options opt;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) opt.out = argv[++i];
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) opt.filter = argv[++i];
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) opt.image = argv[++i];
        else if (!strcmp(argv[i], "-v")) opt.verbose = true;
        else if (!strcmp(argv[i], "--disable-dimension") && i + 1 < argc) {
            const char* name = argv[++i];
            bool found = false;
            for (const auto& d : kDims)
                if (!strcmp(d.name, name)) { opt.disable_dimensions |= d.bit; found = true; break; }
            if (!found) {
                fprintf(stderr, "unknown comparator dimension '%s'\n", name);
                return 2;
            }
        } else {
            fprintf(stderr,
                    "usage: %s --out report.json [--filter substr] [--image relocated.bin] [-v]\n"
                    "          [--disable-dimension return|writes|extra|imports|faults|fault-span|hang]\n",
                    argv[0]);
            return 2;
        }
    }
    if (opt.out.empty()) {
        fprintf(stderr, "--out is required: a run writes one JSON report\n");
        return 2;
    }
    std::vector<dh::Case> cases;
    dh::register_cases(cases);
    return dh::run(dh_module_desc, cases, dh::import_policy(), opt);
}
