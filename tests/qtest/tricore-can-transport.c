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
    /* Reset clears the per-channel pending/status banks and MHDS state. */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C0F0), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C0F4), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C110), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C114), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C16C), ==, 0);
    qtest_writel(global_qtest, 0xF001C108, 0xffffffff);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C108), ==, 0x03ffffff);
    qtest_writel(global_qtest, 0xF001C118, 5); /* HALT is invalid in CONFIG */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C104) & 1, ==, 1);
    /* ERAY0 INT0 is routed to the documented TC27x SRC slot 160. */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0038000 + 160 * 4), !=, 0);
    qtest_writel(global_qtest, 0xF001C104, 1); /* CCEV W1C */
    qtest_writel(global_qtest, 0xF001E000, 1);
    qtest_writel(global_qtest, 0xF001C128, 1); /* commit without unlock */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C104) & 0x80, ==, 0x80);
    qtest_writel(global_qtest, 0xF001C104, 0x80);
    qtest_writel(global_qtest, 0xF001C118, 3); /* cold-start */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C100), ==, 0x0d);
    /* TC2x acceptance: slot boundaries and FIFO controls retain their masks. */
    qtest_writel(global_qtest, 0xF001C134, 4);
    qtest_writel(global_qtest, 0xF001C138, 5);
    qtest_writel(global_qtest, 0xF001C13C, 2);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C134), ==, 4);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C138), ==, 5);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C13C), ==, 2);
    qtest_writel(global_qtest, 0xF001C15C, 1); /* FIFO first buffer */
    qtest_writel(global_qtest, 0xF001C160, 2); /* FIFO depth */
    qtest_writel(global_qtest, 0xF001C174, 1); /* critical level */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C160), ==, 2);
    /* Popping an empty FIFO raises CCEV and the TC2x INT0 SRC request. */
    qtest_writel(global_qtest, 0xF001C128, 1u << 4);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C104) & (1u << 6), !=, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0038000 + 160 * 4) &
                     (1u << 24), !=, 0);
    qtest_writel(global_qtest, 0xF001C104, 1u << 6); /* CCEV W1C */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C104) & (1u << 6), ==, 0);
    qtest_writel(global_qtest, 0xF001C118, 5); /* halt */
    qtest_writel(global_qtest, 0xF001C118, 8); /* warm-start */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C100), ==, 0x0d);
    qtest_quit(global_qtest);

    qtest_start("-machine KIT_AURIX_TC397B_TRB");
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C100), ==, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0017100), ==, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF00170F0), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF00170F4), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0017110), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0017114), ==, 0);
    /* Trigger both ERAY error sources and verify the TC3x SRC rows carry the
     * pending request through the interrupt router. */
    qtest_writel(global_qtest, 0xF001C118, 5);
    qtest_writel(global_qtest, 0xF0017118, 5);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0038000 + 160 * 4), !=, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0038000 + 162 * 4), !=, 0);
    qtest_writel(global_qtest, 0xF0017180, 10);
    qtest_writel(global_qtest, 0xF0017184, 20);
    qtest_writel(global_qtest, 0xF0017188, 32);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF0017188), ==, 32);
    qtest_writel(global_qtest, 0xF001C128, 0x08);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C128), ==, 0);
    qtest_writel(global_qtest, 0xF001C118, 3);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF001C100), ==, 0x0d);
    qtest_quit(global_qtest);

    qtest_start("-machine KIT_A3G_TC4D7_LITE");
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C100), ==, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D100), ==, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C0F0), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C0F4), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C110), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C114), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D0F0), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D0F4), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D110), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D114), ==, 0);
    qtest_writel(global_qtest, 0xF441C118, 3);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C100), ==, 0x0d);
    qtest_writel(global_qtest, 0xF441D118, 3);
    /* TC4x uses the public SRC rows 720..723. Enable all four rows before
     * driving ERAY0/1 events so SRR, masking and explicit W1C/CLRR paths are
     * observable at the interrupt router. */
    for (unsigned src = 720; src < 724; src++) {
        qtest_writel(global_qtest, 0xF4432000 + src * 4,
                     1 | (1u << 23));
    }
    qtest_writel(global_qtest, 0xF441C118, 0xff);
    qtest_writel(global_qtest, 0xF441D118, 0xff);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4432000 + 720 * 4) &
                     (1u << 24), !=, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4432000 + 722 * 4) &
                     (1u << 24), !=, 0);
    /* Disable ERAY0 INT0 and clear its pending request through SRC CLRR. */
    qtest_writel(global_qtest, 0xF4432000 + 720 * 4, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4432000 + 720 * 4) &
                     (1u << 23), ==, 0);
    qtest_writel(global_qtest, 0xF4432000 + 720 * 4, (1u << 25));
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4432000 + 720 * 4) &
                     (1u << 24), ==, 0);
    /* SRC SETR is the interrupt-router W1S counterpart to CLRR. */
    qtest_writel(global_qtest, 0xF4432000 + 720 * 4,
                 1 | (1u << 23) | (1u << 26));
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4432000 + 720 * 4) &
                     (1u << 24), !=, 0);
    qtest_writel(global_qtest, 0xF4432000 + 720 * 4, (1u << 25));
    /* CCEV is W1C on both TC4x ERAY instances; clear the illegal-command
     * events before exercising the message (INT1) sources. */
    qtest_writel(global_qtest, 0xF441C104, 1);
    qtest_writel(global_qtest, 0xF441D104, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 1, ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D104) & 1, ==, 0);
    /* Startup implies sync; the valid pair is decoded into SLOTSTAT. */
    qtest_writel(global_qtest, 0xF441E000, 1);
    qtest_writel(global_qtest, 0xF441E004, 0);
    qtest_writel(global_qtest, 0xF441E008, 6); /* SYNC | STARTUP */
    qtest_writel(global_qtest, 0xF441C124, 0);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 2, ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C120) & 6, ==, 6);
    /* Null+sync and startup-without-sync are rejected as header errors. */
    qtest_writel(global_qtest, 0xF441E008, 3); /* NULL | SYNC */
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 2, !=, 0);
    qtest_writel(global_qtest, 0xF441C104, 2);
    qtest_writel(global_qtest, 0xF441E008, 4); /* STARTUP without SYNC */
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 2, !=, 0);
    qtest_writel(global_qtest, 0xF441C104, 2);
    /* A non-zero cycle selector must match the active controller cycle. */
    qtest_writel(global_qtest, 0xF441E008, (1u << 8) | 2);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 2, !=, 0);
    qtest_writel(global_qtest, 0xF441C104, 2);
    /* Header validation rejects a payload that cannot fit the public 64-byte
     * message-RAM fixture and reports the documented header error event. */
    qtest_writel(global_qtest, 0xF441E000, 0x10);
    qtest_writel(global_qtest, 0xF441E004, 65);
    qtest_writel(global_qtest, 0xF441C124, 0);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 2, ==, 2);
    qtest_writel(global_qtest, 0xF441C104, 2);
    /* Unknown header flags are rejected; a public sync flag is retained. */
    qtest_writel(global_qtest, 0xF441E008, 8);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 2, ==, 2);
    qtest_writel(global_qtest, 0xF441C104, 2);
    qtest_writel(global_qtest, 0xF441E008, 2);
    /* ERAY0 message RAM is at +0x2000; commit one buffer and observe it on
     * the second in-process node through NDAT/MBSC pending state. */
    qtest_writel(global_qtest, 0xF441E000, 0x123);
    qtest_writel(global_qtest, 0xF441E004, 0);
    qtest_writel(global_qtest, 0xF441C124, 0);
    qtest_writel(global_qtest, 0xF441C128, 2); /* unlock */
    qtest_writel(global_qtest, 0xF441C128, 1); /* commit */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D12C), ==, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4432000 + 721 * 4) &
                     (1u << 24), !=, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF4432000 + 723 * 4) &
                     (1u << 24), !=, 0);
    /* NDAT0 and MBSC0 are independent W1C status groups for channel A. */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D0F4) & 1, !=, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D0F0) & 1, !=, 0);
    qtest_writel(global_qtest, 0xF441D0F4, 1);
    qtest_writel(global_qtest, 0xF441D0F0, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D0F4) & 1, ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D0F0) & 1, ==, 0);
    /* Channel-B uses an independent unlock/commit path and sets NDAT1. */
    qtest_writel(global_qtest, 0xF441C124, 1);
    qtest_writel(global_qtest, 0xF441C128, 2 | 4);
    qtest_writel(global_qtest, 0xF441C128, 1 | 4);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D114) & 2, !=, 0);
    qtest_writel(global_qtest, 0xF441D114, 0xffffffff);
    qtest_writel(global_qtest, 0xF441D110, 0xffffffff);
    /* Exercise the public FIFO configuration fields for a dynamic frame. */
    qtest_writel(global_qtest, 0xF441D138, 0);   /* dynamic start */
    qtest_writel(global_qtest, 0xF441D15C, 2);   /* FIFO first buffer */
    qtest_writel(global_qtest, 0xF441D160, 2);   /* FIFO depth */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D160), ==, 2);
    qtest_writel(global_qtest, 0xF441E000, 100);
    qtest_writel(global_qtest, 0xF441C128, 2); /* unlock */
    qtest_writel(global_qtest, 0xF441C128, 1); /* commit */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D160), ==, 2);
    /* Scheduled commit is held until the next virtual macrocycle tick. */
    qtest_writel(global_qtest, 0xF441D114, 0xffffffff);
    qtest_writel(global_qtest, 0xF441D0f4, 0xffffffff);
    qtest_writel(global_qtest, 0xF441D0f0, 0xffffffff);
    qtest_writel(global_qtest, 0xF441C134, 1); /* one static slot */
    qtest_writel(global_qtest, 0xF441C138, 2); /* dynamic segment starts at 2 */
    qtest_writel(global_qtest, 0xF441C130, 1);
    qtest_writel(global_qtest, 0xF441C190, 3); /* dynamic action point */
    qtest_writel(global_qtest, 0xF441C124, 0); /* select buffer 0 again */
    /* Static TX is buffered until its numbered virtual slot boundary. */
    qtest_writel(global_qtest, 0xF441E000, 1);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D12c), ==, 0);
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D12c), !=, 0);
    qtest_writel(global_qtest, 0xF441D0F4, 0xffffffff);
    qtest_writel(global_qtest, 0xF441D0F0, 0xffffffff);
    qtest_writel(global_qtest, 0xF441E000, 101);
    qtest_writel(global_qtest, 0xF441C128, 2); /* unlock */
    qtest_writel(global_qtest, 0xF441C128, 1); /* commit */
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D12c), ==, 0);
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D12c), !=, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C120) & (1u << 30),
                     !=, 0);
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C120) & (1u << 30),
                     !=, 0);
    /* A static request submitted after its slot boundary is rejected. */
    qtest_writel(global_qtest, 0xF441C104, 0xff);
    qtest_writel(global_qtest, 0xF441C134, 1);
    qtest_writel(global_qtest, 0xF441C138, 2);
    qtest_writel(global_qtest, 0xF441C124, 0);
    qtest_writel(global_qtest, 0xF441E000, 1);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 8, !=, 0);
    qtest_writel(global_qtest, 0xF441C104, 8);
    /* Guardian ownership is an authorization failure, not a bus delivery. */
    qtest_writel(global_qtest, 0xF441C140, 1);
    qtest_writel(global_qtest, 0xF441E000, 101);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 8, !=, 0);
    qtest_writel(global_qtest, 0xF441C104, 8);
    qtest_writel(global_qtest, 0xF441C140, 0);
    /* A non-matching slot filter suppresses delivery at the receiver. */
    qtest_writel(global_qtest, 0xF441D0F4, 0xffffffff);
    qtest_writel(global_qtest, 0xF441D0F0, 0xffffffff);
    qtest_writel(global_qtest, 0xF441D164, 200);
    qtest_writel(global_qtest, 0xF441C124, 0);
    qtest_writel(global_qtest, 0xF441E000, 101);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D0F4), ==, 0);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D120) & (1u << 29),
                     !=, 0);
    qtest_writel(global_qtest, 0xF441D120, (1u << 29));
    qtest_writel(global_qtest, 0xF441D164, 0);
    /* A one-entry FIFO reports an overrun on the second accepted frame. */
    qtest_writel(global_qtest, 0xF441D15C, 2);
    qtest_writel(global_qtest, 0xF441D160, 1);
    qtest_writel(global_qtest, 0xF441C130, 0); /* immediate TX */
    qtest_writel(global_qtest, 0xF441E000, 10);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    qtest_writel(global_qtest, 0xF441E000, 11);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441D104) & 16, ==, 16);
    /* A frame outside both configured static and dynamic slot ranges is
     * rejected before it can become a pending transmission. */
    qtest_writel(global_qtest, 0xF441C134, 0);
    qtest_writel(global_qtest, 0xF441C138, 100);
    qtest_writel(global_qtest, 0xF441E000, 50);
    qtest_writel(global_qtest, 0xF441C124, 0);
    qtest_writel(global_qtest, 0xF441C128, 2);
    qtest_writel(global_qtest, 0xF441C128, 1);
    g_assert_cmpuint(qtest_readl(global_qtest, 0xF441C104) & 8, ==, 8);
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
