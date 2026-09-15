#define _POSIX_C_SOURCE 200809L

/*
 * QR Code encoding, byte mode. See mesh/utils/qr.h for what this is for and what it is not.
 *
 * The steps are the standard's (ISO/IEC 18004) and are in its order: choose a version, build
 * the bit stream, split it into blocks and give each one a Reed-Solomon remainder, interleave
 * them, paint the function patterns, walk the data into the gaps, then try all eight masks and
 * keep the one the standard's four penalty rules like least.
 *
 * Two of those steps are worth knowing about before changing anything here:
 *
 * - **The mask is chosen, not configured.** A decoder reads the mask out of the format bits, so
 *   any of the eight is readable; the penalty rules pick the one least likely to contain
 *   something that looks like a finder pattern. That choice is what makes the output a pure
 *   function of the input, which is what a test can pin.
 * - **The block layout is not a division.** A version's data codewords do not divide evenly
 *   into its blocks, so the standard splits them into a short group and a long group and
 *   interleaves the result column-wise. Getting this subtly wrong produces a code that scans on
 *   a forgiving reader and not on a strict one, which is the failure this file is most likely
 *   to have and the reason the tests decode the matrix back rather than only eyeballing it.
 */

#include "mesh/utils/qr.h"

#include <string.h>

#define QR_MODULE_DARK 0x01U
#define QR_MODULE_FUNCTION 0x02U

/* The largest interleaved codeword block a code at MESH_QR_MAX_VERSION carries: version 25 has
   1588 total codewords, and every version below it fewer. */
#define QR_CODEWORDS_MAX 1588U
/* No version uses more than 30 error-correction codewords per block. */
#define QR_ECC_PER_BLOCK_MAX 30U

/*
 * The standard's two block tables, transcribed whole for versions 1-40 rather than cut to
 * MESH_QR_MAX_VERSION. A truncated table is a table nobody can check against the document it
 * came from, and the 328 bytes buy a cap that moves by editing one #define.
 *
 * Rows are the four levels in enum mesh_qr_ecc order (L, M, Q, H); index 0 of each row is a
 * hole, because there is no version 0.
 */
static const uint8_t k_ecc_per_block[4][41] = {
    {0,  7,  10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
     28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {0,  10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
     26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
    {0,  13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30,
     28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {0,  17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28,
     30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
};

static const uint8_t k_blocks[4][41] = {
    {0, 1, 1, 1, 1, 1, 2,  2,  2,  2,  4,  4,  4,  4,  4,  6,  6,  6,  6,  7, 8,
     8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {0,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5,  5,  8,  9,  9,  10, 10, 11, 13, 14, 16,
     17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {0,  1,  1,  2,  2,  4,  4,  6,  6,  8,  8,  8,  10, 12, 16, 12, 17, 16, 18, 21, 20,
     23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {0,  1,  1,  2,  4,  4,  4,  5,  6,  8,  8,  11, 11, 16, 16, 18, 16, 19, 21, 25, 25,
     25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},
};

/* The format bits' own encoding of the four levels, which is not enum order: the standard
   numbers them by how much correction they carry, and the bit pattern by something else. */
static const uint8_t k_ecc_format_bits[4] = {1U, 0U, 3U, 2U};

/* ---- the version's shape ------------------------------------------------------------------ */

/* How many alignment patterns a version has along one edge (none below version 2). */
static int align_count(int version) {
    return version == 1 ? 0 : version / 7 + 2;
}

/* Where they sit. The first is always at 6 and the last at size - 7; the rest are evenly spaced
   between, rounded to an even step. Returns how many were written. */
static int align_positions(int version, uint8_t *out) {
    const int count = align_count(version);
    if (count == 0) {
        return 0;
    }
    const int size = version * 4 + 17;
    /* Version 32 is the one the general rule gets wrong, and the standard tabulates it. */
    const int step = (version == 32) ? 26 : (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
    out[0] = 6U;
    int pos = size - 7;
    for (int i = count - 1; i >= 1; --i) {
        out[i] = (uint8_t)pos;
        pos -= step;
    }
    return count;
}

/* Every codeword a version holds, data and error correction together. Computed from the module
   count rather than tabulated, so it cannot disagree with align_positions() above. */
static size_t total_codewords(int version) {
    int modules = (16 * version + 128) * version + 64;
    const int count = align_count(version);
    if (count > 0) {
        modules -= (25 * count - 10) * count - 55;
    }
    if (version >= 7) {
        modules -= 36; /* the two version-information blocks */
    }
    return (size_t)modules / 8U;
}

static size_t data_codewords(int version, enum mesh_qr_ecc ecc) {
    return total_codewords(version) -
           (size_t)k_ecc_per_block[ecc][version] * (size_t)k_blocks[ecc][version];
}

/* Byte mode's character-count field widens once, at version 10. */
static int count_bits(int version) {
    return version < 10 ? 8 : 16;
}

/* ---- GF(256) and Reed-Solomon -------------------------------------------------------------- */

/* Multiplication in the field the standard uses: GF(2^8) modulo x^8 + x^4 + x^3 + x^2 + 1.
   Russian-peasant rather than log tables - it is called a few thousand times per code, and a
   pair of 256-byte tables to initialise is more state than that saves. */
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t result = 0U;
    for (int i = 7; i >= 0; --i) {
        result = (uint8_t)((result << 1) ^ ((result >> 7) * 0x1DU));
        result = (uint8_t)(result ^ (((b >> i) & 1U) * a));
    }
    return result;
}

/* The generator polynomial of the given degree, as coefficients with the leading 1 implied. */
static void rs_generator(uint8_t degree, uint8_t *out) {
    memset(out, 0, degree);
    out[degree - 1U] = 1U;
    uint8_t root = 1U;
    for (uint8_t i = 0; i < degree; ++i) {
        for (uint8_t j = 0; j < degree; ++j) {
            out[j] = gf_mul(out[j], root);
            if (j + 1U < degree) {
                out[j] = (uint8_t)(out[j] ^ out[j + 1U]);
            }
        }
        root = gf_mul(root, 0x02U);
    }
}

/* The remainder of `data` divided by that polynomial: the block's error-correction codewords. */
static void rs_remainder(const uint8_t *data, size_t len, const uint8_t *generator, uint8_t degree,
                         uint8_t *out) {
    memset(out, 0, degree);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t factor = (uint8_t)(data[i] ^ out[0]);
        memmove(out, out + 1, (size_t)degree - 1U);
        out[degree - 1U] = 0U;
        for (uint8_t j = 0; j < degree; ++j) {
            out[j] = (uint8_t)(out[j] ^ gf_mul(generator[j], factor));
        }
    }
}

/* ---- the bit stream ------------------------------------------------------------------------ */

struct bit_writer {
    uint8_t *bytes;
    size_t capacity; /* in bytes */
    size_t bits;
};

static void bits_put(struct bit_writer *writer, uint32_t value, int width) {
    for (int i = width - 1; i >= 0; --i) {
        const size_t index = writer->bits >> 3;
        if (index >= writer->capacity) {
            return; /* the caller sized the buffer from the version; this cannot be reached */
        }
        writer->bytes[index] =
            (uint8_t)(writer->bytes[index] | (((value >> i) & 1U) << (7 - (writer->bits & 7U))));
        writer->bits++;
    }
}

/* ---- the matrix ---------------------------------------------------------------------------- */

static void set_module(struct mesh_qr *qr, int x, int y, bool dark, bool function) {
    if (x < 0 || y < 0 || x >= (int)qr->size || y >= (int)qr->size) {
        return;
    }
    uint8_t value = dark ? QR_MODULE_DARK : 0U;
    if (function) {
        value = (uint8_t)(value | QR_MODULE_FUNCTION);
    }
    qr->modules[(size_t)y * qr->size + (size_t)x] = value;
}

static bool is_dark(const struct mesh_qr *qr, int x, int y) {
    if (x < 0 || y < 0 || x >= (int)qr->size || y >= (int)qr->size) {
        return false;
    }
    return (qr->modules[(size_t)y * qr->size + (size_t)x] & QR_MODULE_DARK) != 0U;
}

static bool is_function(const struct mesh_qr *qr, int x, int y) {
    return (qr->modules[(size_t)y * qr->size + (size_t)x] & QR_MODULE_FUNCTION) != 0U;
}

static void draw_finder(struct mesh_qr *qr, int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const int ax = dx < 0 ? -dx : dx;
            const int ay = dy < 0 ? -dy : dy;
            const int distance = ax > ay ? ax : ay;
            const int x = cx + dx;
            const int y = cy + dy;
            if (x >= 0 && y >= 0 && x < (int)qr->size && y < (int)qr->size) {
                /* Rings at 0-1 and 3 are dark, 2 and 4 light: the eye, the gap, the square and
                   the separator that keeps it off whatever is next to it. */
                set_module(qr, x, y, distance != 2 && distance != 4, true);
            }
        }
    }
}

static void draw_alignment(struct mesh_qr *qr, int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int ax = dx < 0 ? -dx : dx;
            const int ay = dy < 0 ? -dy : dy;
            set_module(qr, cx + dx, cy + dy, (ax > ay ? ax : ay) != 1, true);
        }
    }
}

/* The fifteen format bits: five of level and mask, ten of BCH, masked with 0x5412 so that the
   all-zero case still has dark modules in it. Written twice, in the two places a decoder that
   found only one finder pattern can still reach. */
static void draw_format(struct mesh_qr *qr, enum mesh_qr_ecc ecc, int mask) {
    const uint32_t data = (uint32_t)((k_ecc_format_bits[ecc] << 3) | (unsigned)mask);
    uint32_t rem = data;
    for (int i = 0; i < 10; ++i) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537U);
    }
    const uint32_t bits = ((data << 10) | rem) ^ 0x5412U;
    const int size = (int)qr->size;

    for (int i = 0; i <= 5; ++i) {
        set_module(qr, 8, i, ((bits >> i) & 1U) != 0U, true);
    }
    set_module(qr, 8, 7, ((bits >> 6) & 1U) != 0U, true);
    set_module(qr, 8, 8, ((bits >> 7) & 1U) != 0U, true);
    set_module(qr, 7, 8, ((bits >> 8) & 1U) != 0U, true);
    for (int i = 9; i < 15; ++i) {
        set_module(qr, 14 - i, 8, ((bits >> i) & 1U) != 0U, true);
    }

    for (int i = 0; i < 8; ++i) {
        set_module(qr, size - 1 - i, 8, ((bits >> i) & 1U) != 0U, true);
    }
    for (int i = 8; i < 15; ++i) {
        set_module(qr, 8, size - 15 + i, ((bits >> i) & 1U) != 0U, true);
    }
    /* The one module that is dark in every code ever made. */
    set_module(qr, 8, size - 8, true, true);
}

/* Version 7 and up carry their own number, twice, so a reader does not have to infer it from
   the module count of a code it has only partly found. */
static void draw_version(struct mesh_qr *qr, int version) {
    if (version < 7) {
        return;
    }
    uint32_t rem = (uint32_t)version;
    for (int i = 0; i < 12; ++i) {
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25U);
    }
    const uint32_t bits = ((uint32_t)version << 12) | rem;
    for (int i = 0; i < 18; ++i) {
        const bool bit = ((bits >> i) & 1U) != 0U;
        const int a = (int)qr->size - 11 + i % 3;
        const int b = i / 3;
        set_module(qr, a, b, bit, true);
        set_module(qr, b, a, bit, true);
    }
}

static void draw_function_patterns(struct mesh_qr *qr, int version, enum mesh_qr_ecc ecc) {
    const int size = (int)qr->size;
    for (int i = 0; i < size; ++i) {
        set_module(qr, 6, i, i % 2 == 0, true);
        set_module(qr, i, 6, i % 2 == 0, true);
    }
    draw_finder(qr, 3, 3);
    draw_finder(qr, size - 4, 3);
    draw_finder(qr, 3, size - 4);

    uint8_t positions[7];
    const int count = align_positions(version, positions);
    for (int i = 0; i < count; ++i) {
        for (int j = 0; j < count; ++j) {
            /* The three corners already hold a finder pattern. */
            const bool corner = (i == 0 && j == 0) || (i == 0 && j == count - 1) ||
                                (i == count - 1 && j == 0);
            if (!corner) {
                draw_alignment(qr, positions[i], positions[j]);
            }
        }
    }

    /* A placeholder, so the modules it occupies are marked as function before the data walk;
       the real bits go in once a mask has been chosen. */
    draw_format(qr, ecc, 0);
    draw_version(qr, version);
}

/* The data walk: two-module-wide columns from the right, alternating up and down, skipping the
   column the vertical timing pattern sits in. */
static void draw_codewords(struct mesh_qr *qr, const uint8_t *data, size_t len) {
    const int size = (int)qr->size;
    size_t bit = 0U;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5;
        }
        for (int vert = 0; vert < size; ++vert) {
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? size - 1 - vert : vert;
                if (!is_function(qr, x, y) && bit < len * 8U) {
                    const bool dark = ((data[bit >> 3] >> (7 - (bit & 7U))) & 1U) != 0U;
                    set_module(qr, x, y, dark, false);
                    bit++;
                }
            }
        }
    }
}

static bool mask_condition(int mask, int x, int y) {
    switch (mask) {
    case 0:
        return (x + y) % 2 == 0;
    case 1:
        return y % 2 == 0;
    case 2:
        return x % 3 == 0;
    case 3:
        return (x + y) % 3 == 0;
    case 4:
        return (x / 3 + y / 2) % 2 == 0;
    case 5:
        return x * y % 2 + x * y % 3 == 0;
    case 6:
        return (x * y % 2 + x * y % 3) % 2 == 0;
    default:
        return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

/* Flips the data modules the mask names. Its own inverse, which is what lets the caller try all
   eight against one matrix. */
static void apply_mask(struct mesh_qr *qr, int mask) {
    const int size = (int)qr->size;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!is_function(qr, x, y) && mask_condition(mask, x, y)) {
                qr->modules[(size_t)y * qr->size + (size_t)x] ^= QR_MODULE_DARK;
            }
        }
    }
}

/*
 * The standard's four penalty rules, which are all about how easily a reader can be fooled.
 *
 * 1. A run of five or more of one colour looks like part of a timing or finder pattern.
 * 2. A solid 2x2 block makes it hard to tell where a module boundary is.
 * 3. The 1:1:3:1:1 finder ratio appearing in the data is the expensive one - it can point a
 *    reader at a corner that is not there - so it costs forty.
 * 4. A code that is mostly dark or mostly light gives an exposure a poor threshold to find.
 */
static long penalty_runs(const struct mesh_qr *qr, bool horizontal) {
    const int size = (int)qr->size;
    long penalty = 0;
    for (int outer = 0; outer < size; ++outer) {
        bool colour = false;
        int run = 0;
        /* The last eleven modules of the run, as a sliding window, for rule 3. */
        unsigned window = 0U;
        for (int inner = 0; inner < size; ++inner) {
            const int x = horizontal ? inner : outer;
            const int y = horizontal ? outer : inner;
            const bool dark = is_dark(qr, x, y);
            if (inner > 0 && dark == colour) {
                run++;
                if (run == 5) {
                    penalty += 3;
                } else if (run > 5) {
                    penalty += 1;
                }
            } else {
                colour = dark;
                run = 1;
            }
            window = ((window << 1) | (dark ? 1U : 0U)) & 0x7FFU;
            if (inner >= 10) {
                /* 00001011101 and 10111010000: the finder ratio with its quiet run either side. */
                if (window == 0x05DU || window == 0x5D0U) {
                    penalty += 40;
                }
            }
        }
    }
    return penalty;
}

static long penalty_score(const struct mesh_qr *qr) {
    const int size = (int)qr->size;
    long penalty = penalty_runs(qr, true) + penalty_runs(qr, false);

    for (int y = 0; y + 1 < size; ++y) {
        for (int x = 0; x + 1 < size; ++x) {
            const bool colour = is_dark(qr, x, y);
            if (colour == is_dark(qr, x + 1, y) && colour == is_dark(qr, x, y + 1) &&
                colour == is_dark(qr, x + 1, y + 1)) {
                penalty += 3;
            }
        }
    }

    long dark = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            dark += is_dark(qr, x, y) ? 1 : 0;
        }
    }
    const long total = (long)size * size;
    /* How many whole five-percent steps the dark share is away from half. */
    long deviation = (dark * 20 - total * 10) / total;
    if (deviation < 0) {
        deviation = -deviation;
    }
    return penalty + deviation * 10;
}

/* ---- the whole thing ----------------------------------------------------------------------- */

/* Splits the data codewords into the version's blocks, gives each its Reed-Solomon remainder,
   and interleaves the lot - data column-wise first, then the remainders. */
static size_t interleave(const uint8_t *data, int version, enum mesh_qr_ecc ecc, uint8_t *out) {
    const size_t blocks = k_blocks[ecc][version];
    const size_t ecc_len = k_ecc_per_block[ecc][version];
    const size_t total = total_codewords(version);
    const size_t data_len = total - ecc_len * blocks;
    /* The short blocks come first; the long ones carry one codeword more. */
    const size_t short_len = data_len / blocks;
    const size_t long_blocks = data_len % blocks;

    uint8_t generator[QR_ECC_PER_BLOCK_MAX];
    rs_generator((uint8_t)ecc_len, generator);

    /* Every remainder end to end, which is ecc_len * blocks - part of the version's total, so
       the whole-code bound is also the bound on this and moves with the cap by itself. */
    uint8_t remainders[QR_CODEWORDS_MAX];
    size_t offset = 0U;
    for (size_t b = 0; b < blocks; ++b) {
        const size_t len = short_len + (b >= blocks - long_blocks ? 1U : 0U);
        rs_remainder(data + offset, len, generator, (uint8_t)ecc_len, remainders + b * ecc_len);
        offset += len;
    }

    size_t written = 0U;
    for (size_t i = 0; i <= short_len; ++i) {
        offset = 0U;
        for (size_t b = 0; b < blocks; ++b) {
            const size_t len = short_len + (b >= blocks - long_blocks ? 1U : 0U);
            if (i < len) {
                out[written++] = data[offset + i];
            }
            offset += len;
        }
    }
    for (size_t i = 0; i < ecc_len; ++i) {
        for (size_t b = 0; b < blocks; ++b) {
            out[written++] = remainders[b * ecc_len + i];
        }
    }
    return written;
}

bool mesh_qr_encode(const uint8_t *data, size_t len, enum mesh_qr_ecc ecc, struct mesh_qr *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if ((data == NULL && len > 0U) || ecc > MESH_QR_ECC_HIGH) {
        return false;
    }

    int version = 0;
    for (int v = 1; v <= (int)MESH_QR_MAX_VERSION; ++v) {
        const size_t capacity_bits = data_codewords(v, ecc) * 8U;
        const size_t needed = 4U + (size_t)count_bits(v) + len * 8U;
        if (needed <= capacity_bits) {
            version = v;
            break;
        }
    }
    if (version == 0) {
        return false;
    }

    /* The bit stream: mode, length, the bytes, a terminator of up to four zeroes, zeroes to the
       next byte boundary, then the standard's two alternating pad bytes. */
    uint8_t codewords[QR_CODEWORDS_MAX];
    memset(codewords, 0, sizeof codewords);
    const size_t capacity = data_codewords(version, ecc);
    struct bit_writer writer = {.bytes = codewords, .capacity = capacity, .bits = 0U};
    bits_put(&writer, 0x4U, 4);
    bits_put(&writer, (uint32_t)len, count_bits(version));
    for (size_t i = 0; i < len; ++i) {
        bits_put(&writer, data[i], 8);
    }
    size_t remaining = capacity * 8U - writer.bits;
    bits_put(&writer, 0U, remaining < 4U ? (int)remaining : 4);
    bits_put(&writer, 0U, (int)((8U - (writer.bits & 7U)) & 7U));
    for (uint8_t pad = 0xECU; writer.bits < capacity * 8U; pad ^= 0xECU ^ 0x11U) {
        bits_put(&writer, pad, 8);
    }

    uint8_t interleaved[QR_CODEWORDS_MAX];
    const size_t total = interleave(codewords, version, ecc, interleaved);

    out->size = (uint8_t)(version * 4 + 17);
    draw_function_patterns(out, version, ecc);
    draw_codewords(out, interleaved, total);

    /* Every mask is readable; the rules pick the one that fools a reader least. Ties go to the
       lower mask number, which is what keeps this deterministic. */
    int best_mask = 0;
    long best_penalty = 0;
    for (int mask = 0; mask < 8; ++mask) {
        apply_mask(out, mask);
        draw_format(out, ecc, mask);
        const long penalty = penalty_score(out);
        if (mask == 0 || penalty < best_penalty) {
            best_penalty = penalty;
            best_mask = mask;
        }
        apply_mask(out, mask); /* its own inverse */
    }
    apply_mask(out, best_mask);
    draw_format(out, ecc, best_mask);
    return true;
}

bool mesh_qr_dark(const struct mesh_qr *qr, int x, int y) {
    return qr != NULL && is_dark(qr, x, y);
}
