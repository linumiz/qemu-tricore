#include "qemu/osdep.h"
#include "libqtest-single.h"

static void dma_profile(const char *machine, uint64_t base)
{
    uint32_t src[4] = { 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00 };
    uint32_t dst[4] = { 0 };
    g_autofree char *args = g_strdup_printf("-machine %s", machine);
    qtest_start(args);
    qtest_memwrite(global_qtest, 0x70001000, src, sizeof(src));
    qtest_memwrite(global_qtest, 0x70001100, dst, sizeof(dst));
    qtest_writel(global_qtest, base + 0x00, 0x70001000);
    qtest_writel(global_qtest, base + 0x04, 0x70001100);
    qtest_writel(global_qtest, base + 0x08, sizeof(src));
    qtest_writel(global_qtest, base + 0x1c, 1); /* ACCEN: supervisor access */
    qtest_writel(global_qtest, base + 0x0c, 1); /* START */
    g_assert_cmphex(qtest_readl(global_qtest, base + 0x10) & 1, ==, 1);
    qtest_memread(global_qtest, 0x70001100, dst, sizeof(dst));
    g_assert_cmpmem(src, sizeof(src), dst, sizeof(dst));
    qtest_quit(global_qtest);
}

static void test_tc2x(void) { dma_profile("KIT_AURIX_TC277_TRB", 0xF0010000); }
static void test_tc3x(void) { dma_profile("KIT_AURIX_TC397B_TRB", 0xF0010000); }
static void test_tc4x(void) { dma_profile("KIT_A3G_TC4D7_LITE", 0xF0000000); }

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("tricore/dma/tc2x", test_tc2x);
    qtest_add_func("tricore/dma/tc3x", test_tc3x);
    qtest_add_func("tricore/dma/tc4x", test_tc4x);
    return g_test_run();
}
