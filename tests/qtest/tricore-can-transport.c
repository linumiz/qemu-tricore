#include "qemu/osdep.h"
#include "libqtest-single.h"

static void test_tc277_can_machine(void)
{
    qtest_start("-object can-bus,id=canbus "
                "-machine KIT_AURIX_TC277_TRB,canbus=canbus");
    qtest_quit(global_qtest);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/tricore/can-machine", test_tc277_can_machine);
    return g_test_run();
}
