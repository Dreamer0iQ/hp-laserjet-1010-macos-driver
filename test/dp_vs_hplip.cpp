// Byte-for-byte comparison of dp_compress() against HPLIP's ModeDeltaPlus::compress().
// Build: see test/run_dp_test.sh (needs an extracted HPLIP source tree).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>

#define private public
#include "ModeDeltaPlus.h"
#undef private

extern "C" {
#include "../src/deltaplus.h"
}

static int compare(const std::vector<uint8_t> &band, uint32_t row_width, uint32_t rows, const char *what)
{
    ModeDeltaPlus hp(row_width);          // inputsize = row_width rounded up to 8
    hp.Init();
    uint32_t hp_len = 2 * 128 * row_width;
    std::vector<uint8_t> hp_out(hp_len), my_out(hp_len);
    bool hp_ok = hp.compress(hp_out.data(), &hp_len, band.data(), row_width, rows, 16);
    uint32_t my_len = (uint32_t) my_out.size();
    int my_ok = dp_compress(my_out.data(), &my_len, band.data(), row_width, rows, 16);
    if (!hp_ok || !my_ok || hp_len != my_len || memcmp(hp_out.data(), my_out.data(), hp_len)) {
        printf("MISMATCH %s: hp_ok=%d len=%u  my_ok=%d len=%u\n", what, hp_ok, hp_len, my_ok, my_len);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    std::mt19937 rng(1010);
    int fails = 0, cases = 0;
    const uint32_t widths[] = { 8, 16, 24, 616, 4768, 4904 };

    // synthetic bands: random noise at several densities, stripes, sparse text-like blobs
    for (uint32_t w : widths) {
        for (int kind = 0; kind < 6; kind++) {
            for (int rep = 0; rep < 20; rep++) {
                std::vector<uint8_t> band(128 * w, 0);
                uint32_t data_bytes = w / 8 ? w / 8 : 1;   // HPLIP: only the first 1/8 carries pixels
                for (uint32_t r = 0; r < 128; r++)
                    for (uint32_t c = 0; c < (kind == 5 ? w : data_bytes); c++) {
                        uint8_t &b = band[r * w + c];
                        switch (kind) {
                        case 0: b = rng(); break;
                        case 1: b = (rng() % 8 == 0) ? rng() : 0; break;
                        case 2: b = (rng() % 64 == 0) ? 0xFF : 0; break;
                        case 3: b = ((c / 16 + r / 4) & 1) ? 0xAA : 0x55; break;
                        case 4: b = (r % 7 < 3 && c % 37 < 9) ? (uint8_t)(0xF0 >> (r % 4)) : 0; break;
                        case 5: b = (rng() % 3) ? band[r ? (r - 1) * w + c : c] : rng(); break;
                        }
                    }
                uint32_t rows = (rep % 4 == 3) ? 1 + rng() % 128 : 128;
                char what[64];
                snprintf(what, sizeof what, "w=%u kind=%d rep=%d rows=%u", w, kind, rep, rows);
                fails += compare(band, w, rows, what);
                cases++;
            }
        }
    }

    // real bands dumped by the filter (file of 128*slot-byte bands), if given
    for (int i = 1; i + 1 < argc; i += 2) {
        uint32_t slot = (uint32_t) atoi(argv[i + 1]);
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 2; }
        std::vector<uint8_t> band(128 * slot);
        int n = 0;
        while (fread(band.data(), 1, band.size(), f) == band.size()) {
            char what[64];
            snprintf(what, sizeof what, "%s band %d", argv[i], n++);
            fails += compare(band, slot, 128, what);
            cases++;
        }
        fclose(f);
    }

    printf("%d cases, %d mismatches\n", cases, fails);
    return fails != 0;
}
