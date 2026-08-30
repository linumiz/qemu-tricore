#include "qemu/osdep.h"
#include "net/can_emu.h"

static unsigned lost_count;
static void lost(CanBusClientState *client)
{
    lost_count++;
}

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

static void test_timed_send(void)
{
    CanBusClientState client = { 0 };
    qemu_can_frame frame = { 0 };
    g_assert_cmpint(can_bus_client_send_timed(&client, &frame, 1, 1000), ==, -1);
}

static void test_arbitration_callback(void)
{
    CanBusClientState first = { 0 }, second = { 0 };
    CanBusBitClientInfo info = { .arbitration_lost = lost };
    first.bit_info = &info;
    second.bit_info = &info;
    const CanBusClientState *senders[] = { &first, &second };
    const uint8_t streams[] = { 0x07, 0x03 };
    lost_count = 0;
    g_assert_cmpint(can_bus_arbitrate_clients((CanBusClientState *const *)senders,
                                               streams, 2, 1, 3), ==, 1);
    g_assert_cmpuint(lost_count, ==, 1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/tricore/can/wired-and", test_wired_and);
    g_test_add_func("/tricore/can/arbitration", test_arbitration);
    g_test_add_func("/tricore/can/timed-send", test_timed_send);
    g_test_add_func("/tricore/can/arbitration-callback", test_arbitration_callback);
    return g_test_run();
}
