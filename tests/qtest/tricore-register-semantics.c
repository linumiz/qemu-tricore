#include "qemu/osdep.h"
#include "libqtest-single.h"

static void test_profile(const char *machine, uint64_t dma, uint64_t ici,
                         uint64_t gate)
{
    g_autofree char *args = g_strdup_printf("-machine %s", machine);
    qtest_start(args);

    /* Reset values and reserved-bit masking. */
    g_assert_cmphex(qtest_readl(global_qtest, dma + 0x10), ==, 0);
    qtest_writel(global_qtest, dma + 0x0c, 0xffffffff);
    g_assert_cmphex(qtest_readl(global_qtest, dma + 0x0c), ==, 0x0f);

    /* Starting without ACCEN must latch ACCESS_ERROR and exercise W1C. */
    qtest_writel(global_qtest, dma + 0x1c, 0);
    qtest_writel(global_qtest, dma + 0x0c, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, dma + 0x10) & (1u << 4), !=, 0);
    qtest_writel(global_qtest, dma + 0x24, 1u << 4);
    g_assert_cmphex(qtest_readl(global_qtest, dma + 0x10) & (1u << 4), ==, 0);

    /* ICI pending bits are W1S; the clear bank is W1C. */
    qtest_writel(global_qtest, ici + 0x00, 0x3);
    g_assert_cmphex(qtest_readl(global_qtest, ici + 0x00), ==, 0x3);
    qtest_writel(global_qtest, ici + 0x0c, 0x1);
    g_assert_cmphex(qtest_readl(global_qtest, ici + 0x00), ==, 0x2);

    /* Gate status is W1C and reset requests are write-one pulses. */
    qtest_writel(global_qtest, gate + 0x04, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, gate + 0x08) & 1, ==, 1);
    qtest_writel(global_qtest, gate + 0x08, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, gate + 0x08) & 1, ==, 0);
    qtest_quit(global_qtest);
}

static void test_tc2x(void) { test_profile("KIT_AURIX_TC277_TRB", 0xF0010000, 0xF003B000, 0xF003B400); }
static void test_tc3x(void) { test_profile("KIT_AURIX_TC397B_TRB", 0xF0010000, 0xF003B000, 0xF003B400); }
static void test_tc4x(void) { test_profile("KIT_A3G_TC4D7_LITE", 0xF0000000, 0xF000B000, 0xF000B400); }

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("tricore/registers/tc2x", test_tc2x);
    qtest_add_func("tricore/registers/tc3x", test_tc3x);
    qtest_add_func("tricore/registers/tc4x", test_tc4x);
    return g_test_run();
}
