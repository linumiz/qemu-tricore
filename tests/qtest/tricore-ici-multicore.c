#include "qemu/osdep.h"
#include "libqtest-single.h"

/* The ICI window is generation-specific, while the register contract is
 * intentionally common.  These tests exercise each SoC's real MMIO map and
 * the corresponding IR source row, rather than probing an isolated device. */
static void test_ici_profile(const char *machine, uint64_t base, uint64_t src)
{
    g_autofree char *args = g_strdup_printf("-machine %s", machine);
    qtest_start(args);

    qtest_writel(global_qtest, base + 0x00, 0x3);
    g_assert_cmphex(qtest_readl(global_qtest, base + 0x00), ==, 0x3);
    /* Pending ICI outputs are reflected in the generation-specific SRC row. */
    g_assert_cmpuint(qtest_readl(global_qtest, src), !=, 0);

    qtest_writel(global_qtest, base + 0x0c, 0x1);
    g_assert_cmphex(qtest_readl(global_qtest, base + 0x00), ==, 0x2);
    qtest_writel(global_qtest, base + 0x0c, 0x2);
    g_assert_cmphex(qtest_readl(global_qtest, base + 0x00), ==, 0);
    qtest_quit(global_qtest);
}

static void test_tc2x(void)
{
    test_ici_profile("KIT_AURIX_TC277_TRB", 0xF003B000,
                     0xF0038000 + (200 * 4));
}

static void test_tc3x(void)
{
    test_ici_profile("KIT_AURIX_TC397B_TRB", 0xF003B000,
                     0xF0038000 + (208 * 4));
}

static void test_tc4x(void)
{
    test_ici_profile("KIT_A3G_TC4D7_LITE", 0xF000B000,
                     0xF4432000 + (720 * 4));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("tricore/ici/tc2x", test_tc2x);
    qtest_add_func("tricore/ici/tc3x", test_tc3x);
    qtest_add_func("tricore/ici/tc4x", test_tc4x);
    return g_test_run();
}
