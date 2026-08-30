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
    qtest_quit(global_qtest);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/tricore/can-machine", test_tc277_can_machine);
    g_test_add_func("/tricore/can-machine-tc397", test_tc397_can_machine);
    g_test_add_func("/tricore/can-machine-tc4d7", test_tc4d7_can_machine);
    return g_test_run();
}
