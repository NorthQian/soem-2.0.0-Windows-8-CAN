#include <iostream>
#include <string.h>
#include <soem/soem.h>

using namespace std;

#define MAX_ADAPTERS_QTY (32)
#define MAX_ADAPTER_NAME (128)

/* Distributed Clocks (DC) settings. DC_CYCLE_NS is both the SYNC0 period
 * written to every DC capable slave and the period of the cyclic loop.
 * 1 ms is the usual EtherCAT default. */
#define DC_CYCLE_NS (1000000) /* 1 ms, in ns */
#define DC_SHIFT_NS (0)       /* SYNC0 shift, 0 = pulse on the cycle boundary */
#define DC_STARTUP_MS (200)   /* settle time after SYNC0 starts */

/* Process data layout, from the slave's object dictionary:
 *
 *   0x7010 "LED"     8 x BOOLEAN   <- main device outputs (RxPDO)
 *   0x6000 "SWITCH"  8 x BOOLEAN   -> main device inputs  (TxPDO)
 *
 * Both directions are one byte's worth of bits, so the LEDs are driven as a
 * running light: a single set bit walks through the 8 LED bits, one step per
 * cycle. With DC enabled a step is 1 ms, so a full rotation takes 8 ms.
 *
 * The switches are printed both as raw hex and as a bit pattern, LSB first,
 * so the leftmost printed bit is switch1 as long as the RxPDO maps subindex 1
 * to bit 0. If the printout comes out mirrored, press switch1 and see which
 * end of the line moves -- then swap SW_BIT_ORDER_LSB.
 *
 * LED_ACTIVE_LOW = 0 (the board here) means active high: the LED bit is driven
 * to 1 to light the LED and the other bits sit at 0. Set it to 1 for a board
 * that sinks current, where the LED lit bit is 0 and the rest are 1. */
#define LED_ACTIVE_LOW (0)
#define LED_BITS (8)
#define SW_BIT_ORDER_LSB (1)

/* Cycles per LED step. The process data is still exchanged every cycle, only
 * the pattern changes more slowly, so this does not affect bus timing.
 * At the 1 ms cycle below, 100 gives 100 ms per step, i.e. a full 8 LED
 * rotation every 800 ms. Raise it to slow the running light down further. */
#define LED_STEP_CYCLES (100)

/* Prints a process data bit range, one character per bit. startbit is the
 * slave's Istartbit/Ostartbit, i.e. which bit of byte 0 the range begins at,
 * so the output is correct even when the bits are packed into a shared byte.
 * lsb_first controls whether the leftmost printed character is the range's
 * first bit or its last one. */
static void print_bits(const uint8 *buf, uint32 bytes, uint32 bits, uint32 startbit, int lsb_first)
{
    for (uint32 n = 0; n < bits; ++n)
    {
        uint32 src = lsb_first ? n : (bits - 1 - n);
        uint32 abs = startbit + src;
        if (abs / 8 >= bytes)
            break; /* range runs past the buffer, stop rather than read on */
        printf("%d", (buf[abs / 8] >> (abs % 8)) & 1);
    }
}

/* Collects every slave's input bits into one pattern, bit 0 first, so a change
 * in any switch shows up as a change in the returned value. */
static uint64 read_switches(ecx_contextt *ctx)
{
    uint64 sw = 0;
    uint32 n = 0;

    for (int i = 1; i <= ctx->slavecount && n < 64; i++)
    {
        uint32 Ibits = ctx->slavelist[i].Ibits;
        uint32 Ibytes = ctx->slavelist[i].Ibytes;
        if (Ibytes == 0 || Ibits == 0)
            continue;

        const uint8 *p = ctx->slavelist[i].inputs;
        for (uint32 b = 0; b < Ibits && n < 64; ++b)
        {
            uint32 abs = ctx->slavelist[i].Istartbit + b;
            if (abs / 8 >= Ibytes)
                break;
            if ((p[abs / 8] >> (abs % 8)) & 1)
                sw |= (uint64)1 << n;
            n++;
        }
    }

    return sw;
}

int basic_examples(char *ifname)
{
    printf("SOEM baisc examples start: \n");

    ecx_contextt ctx;
    uint8 IOmap[4096] = {0};
    int expectedWKC;
    int dc_slaves = 0;
    int64 dc_offset = 0; /* DC time minus local monotonic time, in ns */

    /************************* Init BUS *************************/

    if (!ecx_init(&ctx, ifname))
    {
        printf("Init adapter failed.\n");
        return -1;
    }

    if (ecx_config_init(&ctx) <= 0)
    {
        printf("Enum and init slaves failed.\n");
        return -1;
    }
    else
    {
        /* slavelist[0] is the broadcast entry, the real slaves are 1..slavecount */
        for (int i = 1; i <= ctx.slavecount; i++)
        {
            printf("Slave %03d: ", i);
            printf("Name %s, ", ctx.slavelist[i].name);
            printf("Output %d bytes %d bits, ", ctx.slavelist[i].Obytes, ctx.slavelist[i].Obits);
            printf("Input %d bytes %d bits, ", ctx.slavelist[i].Ibytes, ctx.slavelist[i].Ibits);
            printf("Delay %d ns, Has DC %d\n", ctx.slavelist[i].pdelay, ctx.slavelist[i].hasdc);
            if (ctx.slavelist[i].hasdc)
                dc_slaves++;
        }
    }

    ecx_config_map_group(&ctx, IOmap, 0);
    expectedWKC = ctx.grouplist[0].outputsWKC * 2 + ctx.grouplist[0].inputsWKC;

    ecx_configdc(&ctx);

    ecx_statecheck(&ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

    /* Put every DC capable slave into DC-Synchron mode before requesting OP.
     * A slave whose ESI sync mode is DC-Synchron will not reach OP until it
     * is actually receiving SYNC0, so this has to happen while in SAFE_OP. */
    if (dc_slaves > 0)
    {
        for (int i = 1; i <= ctx.slavecount; i++)
        {
            if (!ctx.slavelist[i].hasdc)
                continue;
            ecx_dcsync0(&ctx, i, TRUE, DC_CYCLE_NS, DC_SHIFT_NS);
            printf("Slave %03d: DC-Synchron, SYNC0 period %d ns, shift %d ns\n",
                   i, DC_CYCLE_NS, DC_SHIFT_NS);
        }
        /* let SYNC0 stabilise before the OP transition */
        osal_usleep(DC_STARTUP_MS * 1000);
    }
    else
    {
        printf("No DC capable slaves found, running free-run.\n");
    }

    ecx_send_processdata(&ctx);
    ecx_receive_processdata(&ctx, EC_TIMEOUTRET);

    // using slave 0, which will broadcast the request to all slaves on the network
    ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&ctx, 0);

    for (size_t i = 0; i < 10; i++)
    {
        ecx_send_processdata(&ctx);
        ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
        ecx_statecheck(&ctx, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 10);
        if (ctx.slavelist[0].state == EC_STATE_OPERATIONAL)
            break;
    }
    if (ctx.slavelist[0].state != EC_STATE_OPERATIONAL)
    {
        printf("Set operational state failed.\n");
        return -1;
    }
        
    /* Anchor the slaves' DC time to the local monotonic clock so the loop can
     * sleep until the next SYNC0 boundary instead of free-running. */
    if (dc_slaves > 0)
    {
        for (int i = 0; i < 100 && ctx.DCtime == 0; i++)
            osal_usleep(1000);
        if (ctx.DCtime == 0)
            printf("Warning: DC time never became valid, loop timing will drift.\n");
        ec_timet now = osal_current_time();
        dc_offset = ctx.DCtime - ((int64)now.tv_sec * 1000000000LL + now.tv_nsec);
    }

    /************************* Main loop *************************/
    /* Sentinel so the first cycle always reports the initial switch state. */
    uint64 last_sw = (uint64)-1;

    for (size_t turn = 0; turn < 10000; turn++)
    {
        ec_timet start, end, diff;

        if (dc_slaves > 0)
        {
            ec_timet now = osal_current_time();
            int64 local_ns = (int64)now.tv_sec * 1000000000LL + now.tv_nsec;

            /* re-anchor once per 1000 cycles to track any clock drift */
            if ((turn % 1000) == 0 && ctx.DCtime > 0)
                dc_offset = ctx.DCtime - local_ns;

            /* sleep until just past the next SYNC0 pulse */
            int64 dc_now = local_ns + dc_offset;
            int64 next = ((dc_now - DC_SHIFT_NS) / DC_CYCLE_NS + 1) * DC_CYCLE_NS + DC_SHIFT_NS;
            int64 wait_ns = next - dc_now;
            if (wait_ns > 0)
                osal_usleep((uint32)(wait_ns / 1000));
        }
        else
        {
            osal_usleep(5000);
        }

        /********* Drive the LEDs: one walking bit per slave *********/
        for (int i = 1; i <= ctx.slavecount; i++)
        {
            uint32 Obits = ctx.slavelist[i].Obits;
            uint32 Obytes = ctx.slavelist[i].Obytes;
            if (Obytes == 0 || Obits == 0)
                continue; /* slave has no byte addressable output */

            uint32 led_bits = (LED_BITS > 0) ? (uint32)LED_BITS : Obits;
            if (led_bits > Obits)
                led_bits = Obits;
            uint32 bit = (uint32)((turn / LED_STEP_CYCLES) % led_bits);

            /* Write bit by bit rather than memsetting the whole byte: the byte
             * may be shared with a neighbouring bit oriented slave, and this
             * slave's bits only start at Ostartbit. */
            uint8 *p = ctx.slavelist[i].outputs;
            uint32 sb = ctx.slavelist[i].Ostartbit;
            for (uint32 b = 0; b < Obits; ++b)
            {
                uint32 abs = sb + b;
                if (abs / 8 >= Obytes)
                    break; /* partial byte at the end of the range */
                uint8 mask = (uint8)(1u << (abs % 8));
                if ((b == bit) != LED_ACTIVE_LOW)
                    p[abs / 8] |= mask;
                else
                    p[abs / 8] &= (uint8)~mask;
            }
        }

        start = osal_current_time();
        ecx_send_processdata(&ctx);
        int wkc = ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
        end = osal_current_time();
        osal_time_diff(&start, &end, &diff);

        uint64 sw = read_switches(&ctx);

        /* Printed on its own line so a press is readable: the status line below
         * is rewritten every cycle with \r and is unreadable at 1 ms. */
        if (sw != last_sw)
        {
            printf("\nSW at iteration %05d:  ", turn + 1);
            for (int i = 1; i <= ctx.slavecount; i++)
            {
                if (ctx.slavelist[i].Ibytes == 0)
                    continue;
                print_bits(ctx.slavelist[i].inputs, ctx.slavelist[i].Ibytes,
                           ctx.slavelist[i].Ibits, ctx.slavelist[i].Istartbit,
                           SW_BIT_ORDER_LSB);
                printf("  ");
            }
            printf("switch1..switch8, ");
            for (int i = 1; i <= ctx.slavecount; i++)
            {
                for (uint32 n = 0; n < ctx.slavelist[i].Ibytes; ++n)
                    printf(" %02X", ctx.slavelist[i].inputs[n]);
            }
            printf("\n");
            last_sw = sw;
        }

        printf("Iteration %05d: ", turn + 1);
        printf("%08d usec, WKC %d", (int)(diff.tv_sec * 1000000 + diff.tv_nsec / 1000), wkc);
        printf(", OUT:");
        for (size_t n = 0; n < ctx.grouplist[0].Obytes; ++n)
        {
            printf(" %02X", ctx.grouplist[0].outputs[n]);
        }
        printf(", LED ");
        for (int i = 1; i <= ctx.slavecount; i++)
        {
            if (ctx.slavelist[i].Obytes == 0)
                continue;
            print_bits(ctx.slavelist[i].outputs, ctx.slavelist[i].Obytes,
                       ctx.slavelist[i].Obits, ctx.slavelist[i].Ostartbit, 1);
            printf(" ");
        }
        printf(", SW ");
        for (int i = 1; i <= ctx.slavecount; i++)
        {
            if (ctx.slavelist[i].Ibytes == 0)
                continue;
            print_bits(ctx.slavelist[i].inputs, ctx.slavelist[i].Ibytes,
                       ctx.slavelist[i].Ibits, ctx.slavelist[i].Istartbit,
                       SW_BIT_ORDER_LSB);
            printf(" ");
        }
        printf(", IN:");
        for (size_t n = 0; n < ctx.grouplist[0].Ibytes; ++n)
        {
            printf(" %02X", ctx.grouplist[0].inputs[n]);
        }
        printf(", T: %lld", (long long)ctx.DCtime);
        if (((turn + 1) % 1000))
            printf("\r");
        else
            printf("\n");

        if (wkc != expectedWKC)
            break; // error: stop or reinit slave
    }

    /************************* Stop BUS *************************/
    ctx.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&ctx, 0);
    ecx_close(&ctx);

    printf("SOEM basic examples end.\n");

    return 0;
}

char *find_and_select_adapters(void)
{
    char adapter_name[MAX_ADAPTERS_QTY][MAX_ADAPTER_NAME] = {0};
    int index = 0;
    ec_adaptert *adapter = NULL;
    ec_adaptert *head = NULL;
    printf("\nAvailable adapters:\n");
    head = adapter = ec_find_adapters();
    while (adapter != NULL)
    {
        printf("%02d - %s (%s)\n", index, adapter->name, adapter->desc);
        strncpy(adapter_name[index], adapter->name, MAX_ADAPTER_NAME);
        index++;
        adapter = adapter->next;
    }
    ec_free_adapters(head);

    printf("Please input the network adapter number: ");
    scanf("%d", &index);

    static char selected_name[MAX_ADAPTER_NAME];
    strncpy(selected_name, adapter_name[index], MAX_ADAPTER_NAME);

    return selected_name;
}

int main()
{
    char *ifname = find_and_select_adapters();

    basic_examples(ifname);

    return 0;
}
