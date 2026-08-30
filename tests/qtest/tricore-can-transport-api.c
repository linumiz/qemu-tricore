#include "qemu/osdep.h"
#include "net/can_emu.h"

static void test_wired_and(void)
{
    const bool drives[] = { true, true, false };
    g_assert_false(can_bus_wired_and(drives, G_N_ELEMENTS(drives)));
}

static void test_arbitration(void)
{
    /* Sender 1 transmits dominant zero at bit 2 and therefore wins. */
    const uint8_t streams[] = { 0x07, 0x03 };
    g_assert_cmpint(can_bus_arbitrate_bits(streams, 2, 1, 3), ==, 1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/tricore/can/wired-and", test_wired_and);
    g_test_add_func("/tricore/can/arbitration", test_arbitration);
    return g_test_run();
}
