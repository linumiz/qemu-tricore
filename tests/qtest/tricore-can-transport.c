#include "qemu/osdep.h"
#include "libqtest-single.h"

static void test_tc277_can_machine(void)
{
    qtest_start("-object can-bus,id=canbus "
                "-machine KIT_AURIX_TC277_TRB,canbus=canbus");
    qtest_quit(global_qtest);
}

static void test_tc397_can_machine(void)
{
    qtest_start("-object can-bus,id=canbus "
                "-machine KIT_AURIX_TC397B_TRB,canbus=canbus");
    /* TC3xx MCMCAN0 control space is present at the documented base. */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0200000), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0220000), ==, 0);
    qtest_quit(global_qtest);
}

static void test_tc4d7_can_machine(void)
{
    qtest_start("-object can-bus,id=canbus "
                "-machine KIT_A3G_TC4D7_LITE,canbus=canbus");
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4710000), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4790000), ==, 0);
    qtest_writel(global_qtest, 0xF4700000, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4700004), ==, 1);
    qtest_quit(global_qtest);
}

static void test_eray_profiles(void)
{
    qtest_start("-machine KIT_AURIX_TC277_TRB");
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C100), ==, 1);
    qtest_writel(global_qtest, 0xF001C118, 5); /* HALT is invalid in CONFIG */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C104) & 1, ==, 1);
    qtest_writel(global_qtest, 0xF001C104, 1); /* CCEV W1C */
    qtest_writel(global_qtest, 0xF001C118, 3); /* cold-start */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C100), ==, 0x0d);
    qtest_quit(global_qtest);

    qtest_start("-machine KIT_AURIX_TC397B_TRB");
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C100), ==, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0017100), ==, 1);
    qtest_writel(global_qtest, 0xF001C118, 3);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C100), ==, 0x0d);
    qtest_quit(global_qtest);

    qtest_start("-machine KIT_A3G_TC4D7_LITE");
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C100), ==, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D100), ==, 1);
    qtest_writel(global_qtest, 0xF441C118, 3);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C100), ==, 0x0d);
    qtest_writel(global_qtest, 0xF441D118, 3);
    /* ERAY0 message RAM is at +0x2000; commit one buffer and observe it on
     * the second in-process node through NDAT/MBSC pending state. */
    qtest_writel(global_qtest, 0xF441E000, 0x123);
    qtest_writel(global_qtest, 0xF441C124, 0);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D12C), ==, 1);
    /* Exercise the public FIFO configuration fields for a dynamic frame. */
    qtest_writel(global_qtest, 0xF441D138, 0);   /* dynamic start */
    qtest_writel(global_qtest, 0xF441D15C, 2);   /* FIFO first buffer */
    qtest_writel(global_qtest, 0xF441D160, 2);   /* FIFO depth */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D160), ==, 2);
    qtest_writel(global_qtest, 0xF441E000, 100);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D160), ==, 2);
    /* Scheduled commit is held until the next virtual macrocycle tick. */
    qtest_writel(global_qtest, 0xF441D114, 0xffffffff);
    qtest_writel(global_qtest, 0xF441C130, 1);
    qtest_writel(global_qtest, 0xF441E000, 101);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D114), ==, 0);
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D114), !=, 0);
    qtest_quit(global_qtest);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/tricore/can-machine", test_tc277_can_machine);
    g_test_add_func("/tricore/can-machine-tc397", test_tc397_can_machine);
    g_test_add_func("/tricore/can-machine-tc4d7", test_tc4d7_can_machine);
    g_test_add_func("/tricore/eray-profiles", test_eray_profiles);
    return g_test_run();
}
