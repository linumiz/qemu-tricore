#include "qemu/osdep.h"
#include "libqtest-single.h"

static uint32_t next_rand(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void fuzz_profile(const char *machine, uint64_t dma, uint64_t mcan,
                         uint64_t eth, uint64_t eray)
{
    g_autofree char *args = g_strdup_printf("-machine %s", machine);
    uint32_t state = 0x51f00d42;
    qtest_start(args);

    /* Bounded writes cover control/status and descriptor-facing windows. */
    for (unsigned i = 0; i < 256; i++) {
        uint32_t v = next_rand(&state);
        switch (i & 3) {
        case 0:
            qtest_writel(global_qtest, dma + ((i & 7) * 4), v & 0x3f);
            break;
        case 1:
            qtest_writel(global_qtest, mcan + ((i & 0xf) * 4), v);
            break;
        case 2:
            qtest_writel(global_qtest, eth + ((i & 0xf) * 4), v);
            break;
        default:
            qtest_writel(global_qtest, eray + ((i & 0x3f) * 4), v);
            break;
        }
    }

    /* An invalid chained descriptor must terminate with DESC_ERROR rather
     * than dereferencing arbitrary guest memory. */
    qtest_writel(global_qtest, dma + 0x14, 0x70001f00);
    qtest_writel(global_qtest, dma + 0x1c, 1);
    qtest_writel(global_qtest, dma + 0x0c, 1 | (1u << 2));
    g_assert_cmpuint(qtest_readl(global_qtest, dma + 0x10) & (1u << 2), !=, 0);
    qtest_writel(global_qtest, dma + 0x24, 1u << 2);
    qtest_quit(global_qtest);
}

static void test_tc2x(void) { fuzz_profile("KIT_AURIX_TC277_TRB", 0xF0010000, 0xF0018000, 0xF001D000, 0xF001C000); }
static void test_tc3x(void) { fuzz_profile("KIT_AURIX_TC397B_TRB", 0xF0010000, 0xF0200000, 0xF001D000, 0xF001C000); }
static void test_tc4x(void) { fuzz_profile("KIT_A3G_TC4D7_LITE", 0xF0000000, 0xF4710000, 0xF9000000, 0xF441C000); }

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("tricore/fuzz/tc2x", test_tc2x);
    qtest_add_func("tricore/fuzz/tc3x", test_tc3x);
    qtest_add_func("tricore/fuzz/tc4x", test_tc4x);
    return g_test_run();
}
