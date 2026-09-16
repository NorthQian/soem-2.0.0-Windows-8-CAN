#include <iostream>
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

        /********* Change output data if need *********/
        // change by slave
        for (int i = 1; i <= ctx.slavecount; i++)
        {
            for (size_t j = 0; j < ctx.slavelist[i].Obytes; j++)
            {
                ctx.slavelist[i].outputs[j] = turn % 256;
            }
        }
        // change by group
        // for (size_t i = 0; i < ctx.grouplist[0].Obytes; i++)
        // {
        //     ctx.grouplist[0].outputs[i] = turn % 256;
        // }

        start = osal_current_time();
        ecx_send_processdata(&ctx);
        int wkc = ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
        end = osal_current_time();
        osal_time_diff(&start, &end, &diff);

        printf("Iteration %05d: ", turn + 1);
        printf("%08d usec, WKC %d", (int)(diff.tv_sec * 1000000 + diff.tv_nsec / 1000), wkc);
        printf(", O:");
        for (size_t n = 0; n < ctx.grouplist[0].Obytes; ++n)
        {
            printf(" %02X", ctx.grouplist[0].outputs[n]);
        }
        printf(", I:");
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
