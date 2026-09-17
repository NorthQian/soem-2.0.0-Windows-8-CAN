#include <iostream>
#include <math.h>
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
 *   0x7010 "SEND_DATA" 17 subindices   <- main device outputs (RxPDO)
 *        0x01..0x08  led1..led8        8 x BOOLEAN   -> output bits  0..7
 *        0x09        send_id           UNSIGNED8     -> output byte  1
 *        0x0a..0x11  can_txdata0..7    8 x UNSIGNED8 -> output bytes 2..9
 *
 *   0x6000 "REC_DATA"  17 subindices   -> main device inputs (TxPDO)
 *        0x01..0x08  switch1..switch8  8 x BOOLEAN   -> input bits   0..7
 *        0x09        rec_id            UNSIGNED8     -> input byte   1
 *        0x0a..0x11  can_rxdata0..7    8 x UNSIGNED8 -> input bytes  2..9
 *
 * Only the LED and switch bit ranges below are touched. send_id, rec_id and
 * the CAN payload bytes belong to the slave application, so the master leaves
 * them alone -- note they are not byte 0/1 aligned to the LED bits if the PDO
 * mapping ever changes, hence the explicit first-bit offsets.
 *
 * The LEDs run a ping-pong light: a single set bit walks from led1 up to led8,
 * then back down to led1, so the light bounces instead of snapping back.
 *
 * The switches are printed both as raw hex and as a bit pattern, LSB first,
 * so the leftmost printed bit is switch1 as long as the TxPDO maps subindex 1
 * to bit 0. If the printout comes out mirrored, press switch1 and see which
 * end of the line moves -- then swap SW_BIT_ORDER_LSB.
 *
 * LED_ACTIVE_LOW = 0 (the board here) means active high: the LED bit is driven
 * to 1 to light the LED and the other bits sit at 0. Set it to 1 for a board
 * that sinks current, where the LED lit bit is 0 and the rest are 1. */
#define LED_ACTIVE_LOW (0)
#define LED_FIRST_BIT (0) /* offset of led1 within the slave's output bits */
#define LED_BITS (8)
#define SW_FIRST_BIT (0) /* offset of switch1 within the slave's input bits */
#define SW_BITS (8)
#define SW_BIT_ORDER_LSB (1)

/* Cycles per LED step. The process data is still exchanged every cycle, only
 * the pattern changes more slowly, so this does not affect bus timing.
 * At the 1 ms cycle below, 100 gives 100 ms per LED, so the light travels
 * led1 -> led8 in 700 ms and one full bounce takes 1.4 s. Raise it to slow
 * the running light down further. */
#define LED_STEP_CYCLES (100)

/* ---- DM (Damiao) motor behind the CAN gateway ---------------------------
 *
 * The slave is a CAN bridge: whatever sits in can_txdata0..7 is transmitted on
 * the CAN bus, and the motor's reply is copied back into can_rxdata0..7. Those
 * fields share the same 10 process data bytes as the LEDs and switches:
 *
 *   outputs  led1..8        bits LED_FIRST_BIT .. +LED_BITS         ( 0.. 7)
 *            send_id        bit  CAN_TX_ID_FIRST_BIT              (    8)
 *            can_txdata0..7 bits CAN_TX_DATA_FIRST_BIT .. +64     (16..79)
 *   inputs   switch1..8     bits SW_FIRST_BIT .. +SW_BITS
 *            rec_id         bit  CAN_RX_ID_FIRST_BIT
 *            can_rxdata0..7 bits CAN_RX_DATA_FIRST_BIT .. +64
 *
 * 8 + 8 + 64 = 80 bits = 10 bytes exactly: no padding, and the LED range and
 * the CAN fields are disjoint bit sets. The offsets are derived from the LED
 * and switch constants rather than written as 8/16, so the two ranges cannot
 * drift apart if the PDO mapping ever changes.
 *
 * DM protocol notes:
 *  - There is no separate "control mode" register here. The CAN ID picks the
 *    mode, and MIT mode uses motor_id -- the same ID as the enable frame. The
 *    two are told apart by their payload alone.
 *  - send_id is the motor id; the slave firmware derives the CAN ID from it.
 *    This master cannot send a CAN ID, only the motor id, so which CAN ID comes
 *    out is entirely the firmware's choice. The IDs for motor id 1 are:
 *        MIT control / enable   motor_id + 0x000 = 0x001   <- assumed below
 *        position-velocity      motor_id + 0x100 = 0x101   <- NOT used here
 *    0x101 is what the reference Python drives, because that script runs
 *    POS_MODE with a P_des/V_des float32 pair. Sending the packed MIT frame
 *    under that ID would be read as two floats -- the second one lands around
 *    -1.6e38, so the motor would run away at full speed. If the motor bolts the
 *    instant MIT control starts, suspect the firmware deriving 0x101: nothing
 *    on the master side can tell the two apart, and nothing here can fix it.
 *  - The slave transmits every EtherCAT cycle on its own, with no handshake,
 *    so keeping the buffer up to date is the whole interface. */
#define CAN_TX_ID_FIRST_BIT   (LED_FIRST_BIT + LED_BITS)             /*  8 */
#define CAN_TX_ID_BITS        (8)
#define CAN_TX_DATA_FIRST_BIT (CAN_TX_ID_FIRST_BIT + CAN_TX_ID_BITS) /* 16 */
#define CAN_TX_DATA_BYTES     (8)

#define CAN_RX_ID_FIRST_BIT   (SW_FIRST_BIT + SW_BITS)
#define CAN_RX_ID_BITS        (8)
#define CAN_RX_DATA_FIRST_BIT (CAN_RX_ID_FIRST_BIT + CAN_RX_ID_BITS)
#define CAN_RX_DATA_BYTES     (8)

/* Bytes a slave must expose for the gateway to be usable. */
#define DM_OUT_BYTES (10)
#define DM_IN_BYTES  (10)

/* The motor id written to send_id; the slave turns it into the CAN ID. */
#define DM_MOTOR_ID (1)

/* How long the enable payload occupies can_txdata before MIT control starts.
 * The reference script sends it ten times then sleeps 1 s; since the slave
 * forwards the buffer every cycle with no handshake, what matters is only that
 * the payload sits there for a full second, which is ~1000 cycles here. */
#define DM_ENABLE_MS (1000)

/* Re-send the enable payload every N control cycles, 0 = never.
 *
 * Off by default, and that differs from the reference script on purpose: the
 * script runs POS_MODE, where the control frames go to motor_id + 0x100 and the
 * enable/heartbeat lives alone on motor_id. In MIT mode the control frames are
 * already on motor_id every cycle, so an enable frame spliced in would interrupt
 * the MIT stream rather than maintain it. Turn this on only if the motor's
 * watchdog genuinely needs it. */
#define DM_HEARTBEAT_CYCLES (0)

/* Disable frames held on the wire when the demo ends, so the servo is not left
 * holding its final setpoint. The slave forwards whatever it is given every
 * cycle, so this is what actually lets the motor go. */
#define DM_DISABLE_CYCLES (50)

/* MIT command. kp/kd are the gains the motor closes its own loop with. */
#define DM_KP  (30.0f)
#define DM_KD  (1.0f)
#define DM_TFF (0.0f)

/* Fixed point ranges used to pack those commands, unchanged from the reference
 * script. DM's default kp/kd spans are [0,500] and [0,5]. */
#define DM_P_MAX  (12.5f)
#define DM_V_MAX  (45.0f)
#define DM_T_MAX  (18.0f)
#define DM_KP_MAX (500.0f)
#define DM_KD_MAX (5.0f)

/* Sinusoidal position profile: p = SIN_AMP*sin(SIN_OMEGA*t). SIN_AMP and the
 * peak velocity are deliberately the same as the triangular profile it
 * replaces, so the mechanism stays inside the envelope that was already
 * reviewed (+-pi rad, 2 rad/s = 115 deg/s). Deriving SIN_OMEGA from those two
 * gives 2/pi rad/s, hence a period of pi^2 ~= 9.87 s. The loop runs 10000
 * cycles at 1 ms, of which the first DM_ENABLE_MS is spent on the enable
 * payload, so MIT control gets 9 s -- about 91% of one cycle: +pi at t=2.47 s,
 * 0 at 4.93 s, -pi at 7.40 s, and back to -1.65 rad as the demo ends.
 *
 * Zero-velocity happens at the extremes and zero-position at the centre, so
 * unlike the triangle there is no point where position and velocity are both
 * changing fastest at once. */
#define SIN_AMP   (3.14159265f)
#define SIN_VEL   (2.0f)
#define SIN_OMEGA (SIN_VEL / SIN_AMP)

typedef enum
{
    DM_PHASE_ENABLE = 0, /* holding the enable payload */
    DM_PHASE_CONTROL = 1 /* streaming MIT frames */
} dm_phase_t;

static const uint8 DM_ENABLE_PAYLOAD[CAN_TX_DATA_BYTES] =
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFC};
static const uint8 DM_DISABLE_PAYLOAD[CAN_TX_DATA_BYTES] =
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD};

/* Number of bits to use from a range starting at first. A want of 0 means
 * "everything from first to the end", and the result is clamped so first +
 * count never runs past total. Keeps the call sites from repeating the clamp. */
static uint32 bit_count(uint32 first, uint32 want, uint32 total)
{
    if (first >= total)
        return 0;
    uint32 count = (want > 0) ? want : (total - first);
    return (first + count > total) ? (total - first) : count;
}

/* Prints a process data bit range, one character per bit. startbit is the
 * slave's Istartbit/Ostartbit, i.e. which bit of byte 0 the slave's range
 * begins at, so the output is correct even when the bits are packed into a
 * byte shared with a neighbouring slave. lsb_first controls whether the
 * leftmost printed character is the range's first bit or its last one. */
static void print_bits(const uint8 *buf, uint32 bytes, uint32 first, uint32 count,
                       uint32 startbit, int lsb_first)
{
    for (uint32 n = 0; n < count; ++n)
    {
        uint32 idx = lsb_first ? n : (count - 1 - n);
        uint32 abs = startbit + first + idx;
        if (abs / 8 >= bytes)
            break; /* range runs past the buffer, stop rather than read on */
        printf("%d", (buf[abs / 8] >> (abs % 8)) & 1);
    }
}

/* Writes one 8 bit field into a process data buffer. first is the field's bit
 * offset within the slave's own range and startbit is that slave's Ostartbit,
 * so bit b of value lands on bit (startbit + first + b) -- subindex 1 is bit 0
 * of the range, matching the LED and switch convention. Done bit by bit rather
 * than as a byte store because the field may not start on a byte boundary, and
 * a partial field at the end of the buffer is left unwritten rather than
 * spilling into whatever follows. */
static void write_byte_field(uint8 *buf, uint32 bytes, uint32 startbit,
                             uint32 first, uint8 value)
{
    uint32 abs = startbit + first;

    for (uint32 b = 0; b < 8; ++b)
    {
        if ((abs + b) / 8 >= bytes)
            return;
        uint8 mask = (uint8)(1u << ((abs + b) % 8));
        if ((value >> b) & 1u)
            buf[(abs + b) / 8] |= mask;
        else
            buf[(abs + b) / 8] &= (uint8)~mask;
    }
}

/* Reads one 8 bit field back. Returns 1 and stores the value when the whole
 * field fits, 0 otherwise with *out untouched -- a half present field is a
 * miss, not a value worth reporting. */
static int read_byte_field(const uint8 *buf, uint32 bytes, uint32 startbit,
                           uint32 first, uint8 *out)
{
    uint32 abs = startbit + first;

    if ((abs + 7) / 8 >= bytes)
        return 0;

    uint8 v = 0;
    for (uint32 b = 0; b < 8; ++b)
        v |= (uint8)(((buf[(abs + b) / 8] >> ((abs + b) % 8)) & 1u) << b);
    *out = v;
    return 1;
}

/* Fixed point encode: uint = (clamp(x) - lo) * ((1 << bits) - 1) / (hi - lo).
 * Clamping first keeps the product non-negative, so the truncating cast lands
 * where the reference script's int() does. bits is capped at 16 because that is
 * the widest field DM uses and it keeps (1u << bits) well defined. */
static uint32 float_to_uint(float x, float lo, float hi, int bits)
{
    float span = hi - lo;
    if (span <= 0.0f || bits <= 0 || bits > 16)
        return 0;
    if (x < lo)
        x = lo;
    if (x > hi)
        x = hi;
    return (uint32)((x - lo) * (float)((1u << bits) - 1u) / span);
}

/* Inverse of float_to_uint, for scaling the feedback frame back to rad. */
static float uint_to_float(uint32 x, float lo, float hi, int bits)
{
    return lo + (float)x * (hi - lo) / (float)((1u << bits) - 1u);
}

/* Packs one DM MIT frame: p_des 16 bit, then v_des/kp/kd/t_ff 12 bit each, so
 * 16 + 12*4 = 64 bits = exactly the 8 byte CAN payload with no spare bits.
 * Each field is masked to its own width, which is what keeps the shifts safe:
 * a caller passing the wrong bit count to float_to_uint turns into a wrong but
 * bounded value here instead of silently corrupting the neighbouring field.
 *
 * Note that a symmetric range does not encode zero as zero: t_ff = 0 comes out
 * as the midpoint of the range, not as 0x000. Hand writing zero payload bytes
 * is therefore always wrong -- always go through this function. */
static void dm_mit_pack(float p, float v, float kp, float kd, float t_ff, uint8 out[8])
{
    uint32 p_des = float_to_uint(p, -DM_P_MAX, DM_P_MAX, 16);
    uint32 v_des = float_to_uint(v, -DM_V_MAX, DM_V_MAX, 12);
    uint32 kp_u = float_to_uint(kp, 0.0f, DM_KP_MAX, 12);
    uint32 kd_u = float_to_uint(kd, 0.0f, DM_KD_MAX, 12);
    uint32 t_u = float_to_uint(t_ff, -DM_T_MAX, DM_T_MAX, 12);

    out[0] = (uint8)(p_des >> 8);
    out[1] = (uint8)(p_des & 0xFF);
    out[2] = (uint8)(v_des >> 4);
    out[3] = (uint8)(((v_des & 0x0F) << 4) | ((kp_u >> 8) & 0x0F));
    out[4] = (uint8)(kp_u & 0xFF);
    out[5] = (uint8)(kd_u >> 4);
    out[6] = (uint8)(((kd_u & 0x0F) << 4) | ((t_u >> 8) & 0x0F));
    out[7] = (uint8)(t_u & 0xFF);
}

/* Sinusoidal position profile: p = SIN_AMP*sin(SIN_OMEGA*t), v = dp/dt.
 * t is seconds since MIT control started, so the profile always begins at
 * p = 0 rather than wherever the process happened to be when the bus came up.
 * Unlike the triangle there are no corner discontinuities: the velocity is
 * continuous, so the servo is never asked for a step change in velocity.
 * vel_out, which may be NULL, is given the matching feed-forward velocity. */
static float dm_sin(float t, float *vel_out)
{
    const float a = SIN_OMEGA * t;
    if (vel_out)
        *vel_out = SIN_AMP * SIN_OMEGA * cosf(a);
    return SIN_AMP * sinf(a);
}

/* Decoded motor reply. */
typedef struct
{
    int valid;    /* 0 when the slave has not copied a CAN reply in yet */
    int motor_id; /* low nibble of byte 0 */
    int state;    /* high nibble: 0 STOP, 1 RUN, 2 ERR, 3 CALIB */
    float pos;    /* rad */
    float vel;    /* rad/s */
    float torque; /* Nm */
    uint8 t_mos;  /* MOS temperature, degC */
    uint8 t_coil; /* coil temperature, degC */
    uint8 raw[CAN_RX_DATA_BYTES];
} dm_feedback;

static const char *dm_state_name(int st)
{
    switch (st)
    {
    case 0:
        return "STOP";
    case 1:
        return "RUN";
    case 2:
        return "ERR";
    case 3:
        return "CALIB";
    default:
        return "?";
    }
}

/* Decodes the 8 byte DM reply the slave copied into can_rxdata0..7:
 *   byte 0   low nibble motor id, high nibble state
 *   p_int = (b1 << 8) | b2            16 bit
 *   v_int = (b3 << 4) | (b4 >> 4)     12 bit
 *   t_int = ((b4 & 0x0F) << 8) | b5   12 bit
 *   b6, b7 = MOS and coil temperature
 * An all zero frame means nothing has been received yet, so it is reported as
 * invalid rather than fed through the scaling as if it were a real reading.
 * rec_id is deliberately not used here: its meaning is not known, and keeping
 * it out of the decode means a wrong guess about it cannot corrupt the values. */
static dm_feedback dm_parse_feedback(const uint8 *buf, uint32 bytes, uint32 startbit)
{
    dm_feedback f;
    memset(&f, 0, sizeof(f));

    for (uint32 n = 0; n < CAN_RX_DATA_BYTES; ++n)
    {
        if (!read_byte_field(buf, bytes, startbit, CAN_RX_DATA_FIRST_BIT + n * 8, &f.raw[n]))
            return f; /* field lies outside this slave's inputs */
    }

    uint8 any = 0;
    for (uint32 n = 0; n < CAN_RX_DATA_BYTES; ++n)
        any |= f.raw[n];
    if (any == 0)
        return f; /* valid stays 0: no CAN reply has arrived */

    uint32 p_int = ((uint32)f.raw[1] << 8) | f.raw[2];
    uint32 v_int = ((uint32)f.raw[3] << 4) | (f.raw[4] >> 4);
    uint32 t_int = (((uint32)f.raw[4] & 0x0F) << 8) | f.raw[5];

    f.valid = 1;
    f.motor_id = f.raw[0] & 0x0F;
    f.state = (f.raw[0] >> 4) & 0x0F;
    f.pos = uint_to_float(p_int, -DM_P_MAX, DM_P_MAX, 16);
    f.vel = uint_to_float(v_int, -DM_V_MAX, DM_V_MAX, 12);
    f.torque = uint_to_float(t_int, -DM_T_MAX, DM_T_MAX, 12);
    f.t_mos = f.raw[6];
    f.t_coil = f.raw[7];
    return f;
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

    /************************* DM motor gateway state *************************/
    /* Located by byte count rather than by index, so a change in bus order
     * cannot silently point the CAN traffic at the wrong slave. */
    int dm_slave = 0;
    for (int i = 1; i <= ctx.slavecount; i++)
    {
        if (ctx.slavelist[i].Obytes >= DM_OUT_BYTES &&
            ctx.slavelist[i].Ibytes >= DM_IN_BYTES)
        {
            dm_slave = i;
            break;
        }
    }
    if (dm_slave == 0)
        printf("Warning: no slave with %d output / %d input bytes, "
               "motor control disabled.\n", DM_OUT_BYTES, DM_IN_BYTES);
    else
        printf("DM gateway on slave %03d (%s), motor id %d, kp %.0f kd %.0f\n",
               dm_slave, ctx.slavelist[dm_slave].name, DM_MOTOR_ID,
               (double)DM_KP, (double)DM_KD);

    dm_phase_t dm_phase = DM_PHASE_ENABLE; /* enable payload goes out first */
    int64 dm_enable_ns = 0;                /* stamped on the first enable write */
    int64 dm_ctrl_ns = 0;                  /* stamped when MIT control starts */
    int64 dm_now_ns = 0;                   /* this cycle's timestamp, shared */
    uint32 dm_ctrl_cycles = 0;             /* control cycles, for the heartbeat */
    float dm_cmd_pos = 0.0f;               /* last commanded setpoint */
    float dm_cmd_vel = 0.0f;
    dm_feedback dm_fb;

    memset(&dm_fb, 0, sizeof(dm_fb));

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

        /********* Drive the LEDs: one bouncing bit per slave *********/
        for (int i = 1; i <= ctx.slavecount; i++)
        {
            uint32 Obits = ctx.slavelist[i].Obits;
            uint32 Obytes = ctx.slavelist[i].Obytes;
            if (Obytes == 0 || Obits == 0)
                continue; /* slave has no byte addressable output */

            uint32 count = bit_count(LED_FIRST_BIT, LED_BITS, Obits);
            if (count == 0)
                continue;

            /* Ping-pong: walk up through the LEDs, then back down, so the light
             * bounces off both ends instead of snapping from the last LED back
             * to the first. One full bounce is 2 * (count - 1) steps; the
             * endpoints are visited once per pass, not twice. */
            uint32 span = (count > 1) ? (2 * (count - 1)) : 1;
            uint32 step = (uint32)((turn / LED_STEP_CYCLES) % span);
            uint32 bit = (step < count) ? step : (span - step);

            /* Write bit by bit rather than memsetting the byte: the LED byte
             * may be packed next to send_id and the CAN payload, and this
             * slave's bits only start at Ostartbit. */
            uint8 *p = ctx.slavelist[i].outputs;
            uint32 sb = ctx.slavelist[i].Ostartbit;
            for (uint32 b = 0; b < count; ++b)
            {
                uint32 abs = sb + LED_FIRST_BIT + b;
                if (abs / 8 >= Obytes)
                    break; /* partial byte at the end of the range */
                uint8 mask = (uint8)(1u << (abs % 8));
                if ((b == bit) != LED_ACTIVE_LOW)
                    p[abs / 8] |= mask;
                else
                    p[abs / 8] &= (uint8)~mask;
            }
        }

        /********* Feed the CAN gateway: enable first, then MIT frames *********/
        if (dm_slave != 0)
        {
            uint8 *op = ctx.slavelist[dm_slave].outputs;
            uint32 obytes = ctx.slavelist[dm_slave].Obytes;
            uint32 sbo = ctx.slavelist[dm_slave].Ostartbit;
            uint8 mit[CAN_TX_DATA_BYTES];

            ec_timet mnow = osal_current_time();
            dm_now_ns = (int64)mnow.tv_sec * 1000000000LL + mnow.tv_nsec;

            /* send_id addresses the frame, so write it every cycle and before
             * the payload. IOmap starts zeroed, which means without this the
             * slave's first forwarded frames would be addressed to motor 0
             * carrying whatever the enable payload happens to hold -- an enable
             * command aimed at nobody, but worth suppressing all the same. */
            write_byte_field(op, obytes, sbo, CAN_TX_ID_FIRST_BIT, DM_MOTOR_ID);

            const uint8 *payload = DM_ENABLE_PAYLOAD;

            if (dm_phase == DM_PHASE_ENABLE)
            {
                if (dm_enable_ns == 0)
                    dm_enable_ns = dm_now_ns;

                if ((dm_now_ns - dm_enable_ns) / 1000000LL >= DM_ENABLE_MS)
                {
                    dm_phase = DM_PHASE_CONTROL;
                    dm_ctrl_ns = dm_now_ns; /* trajectory clock starts here, not
                                             * when the process started */
                    dm_ctrl_cycles = 0;
                    printf("\nMOTOR: enable held %lld ms, starting MIT control "
                           "(kp %.0f kd %.0f t_ff %.1f, sine +-%.2f rad at "
                           "%.1f rad/s peak, period %.2f s)\n",
                           (long long)((dm_now_ns - dm_enable_ns) / 1000000LL),
                           (double)DM_KP, (double)DM_KD, (double)DM_TFF,
                           (double)SIN_AMP, (double)SIN_VEL,
                           (double)(2.0f * 3.14159265f / SIN_OMEGA));
                }
            }
            else
            {
                dm_ctrl_cycles++;

                /* An enable frame spliced into the MIT stream would interrupt it,
                 * so the heartbeat is off unless explicitly asked for. The
                 * trajectory clock keeps running through a skipped frame. */
                if (DM_HEARTBEAT_CYCLES > 0 &&
                    (dm_ctrl_cycles % DM_HEARTBEAT_CYCLES) == 0)
                {
                    payload = DM_ENABLE_PAYLOAD;
                }
                else
                {
                    float t = (float)(dm_now_ns - dm_ctrl_ns) * 1e-9f;
                    dm_cmd_pos = dm_sin(t, &dm_cmd_vel);
                    dm_mit_pack(dm_cmd_pos, dm_cmd_vel, DM_KP, DM_KD, DM_TFF, mit);
                    payload = mit;
                }
            }

            for (uint32 n = 0; n < CAN_TX_DATA_BYTES; ++n)
                write_byte_field(op, obytes, sbo, CAN_TX_DATA_FIRST_BIT + n * 8, payload[n]);
        }

        start = osal_current_time();
        ecx_send_processdata(&ctx);
        int wkc = ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
        end = osal_current_time();
        osal_time_diff(&start, &end, &diff);

        /* Pick up whatever the slave copied back from the CAN bus. */
        if (dm_slave != 0)
            dm_fb = dm_parse_feedback(ctx.slavelist[dm_slave].inputs,
                                      ctx.slavelist[dm_slave].Ibytes,
                                      ctx.slavelist[dm_slave].Istartbit);

        /* One rewritten line per cycle: the LED pattern, the switch pattern,
         * the commanded setpoint and the motor's reply.
         *
         * The commanded position is printed even when no feedback ever arrives,
         * because it keeps advancing either way -- that is the quickest way to
         * tell "the master is commanding but the CAN return path is dead" from
         * "the master is not commanding at all". */
        printf("\rLED ");
        for (int i = 1; i <= ctx.slavecount; i++)
        {
            if (ctx.slavelist[i].Obytes == 0)
                continue;
            print_bits(ctx.slavelist[i].outputs, ctx.slavelist[i].Obytes,
                       LED_FIRST_BIT,
                       bit_count(LED_FIRST_BIT, LED_BITS, ctx.slavelist[i].Obits),
                       ctx.slavelist[i].Ostartbit, 1);
            printf(" ");
        }

        printf(" SW ");
        for (int i = 1; i <= ctx.slavecount; i++)
        {
            if (ctx.slavelist[i].Ibytes == 0)
                continue;
            print_bits(ctx.slavelist[i].inputs, ctx.slavelist[i].Ibytes,
                       SW_FIRST_BIT,
                       bit_count(SW_FIRST_BIT, SW_BITS, ctx.slavelist[i].Ibits),
                       ctx.slavelist[i].Istartbit, SW_BIT_ORDER_LSB);
            printf(" ");
        }

        printf(" | cmd p=%+7.3f v=%+6.2f", (double)dm_cmd_pos, (double)dm_cmd_vel);

        if (dm_slave == 0)
            printf(" | no gateway");
        else if (dm_fb.valid)
            printf(" | fb p=%+7.3f v=%+6.2f t=%+5.2f %s %u/%uC%s",
                   (double)dm_fb.pos, (double)dm_fb.vel, (double)dm_fb.torque,
                   dm_state_name(dm_fb.state),
                   (unsigned)dm_fb.t_mos, (unsigned)dm_fb.t_coil,
                   (dm_fb.motor_id != DM_MOTOR_ID) ? " ID!" : "");
        else
            printf(" | fb none");

        /* Pad over the tail of any longer previous line, then leave the cursor
         * at column 0 so the next cycle overwrites this one cleanly. */
        printf("%-24s\r", "");

        if (wkc != expectedWKC)
        {
            /* The only bus health check there is, so say so rather than dropping
             * out of the loop for no visible reason. */
            printf("\nBus error: WKC %d, expected %d. Stopping.\n", wkc, expectedWKC);
            break;
        }
    }

    /* Disable the motor before dropping the bus. The slave keeps forwarding the
     * last payload every cycle forever, so leaving a live MIT frame in the
     * buffer would leave the servo actively holding its final setpoint. If the
     * loop exited on a wkc mismatch the bus is already broken and none of this
     * will reach the motor -- cut power in that case. */
    if (dm_slave != 0 && dm_phase == DM_PHASE_CONTROL)
    {
        uint8 *op = ctx.slavelist[dm_slave].outputs;
        uint32 obytes = ctx.slavelist[dm_slave].Obytes;
        uint32 sbo = ctx.slavelist[dm_slave].Ostartbit;

        printf("\nMOTOR: disabling, sending %d frames on motor id %d ...\n",
               DM_DISABLE_CYCLES, DM_MOTOR_ID);

        for (int n = 0; n < DM_DISABLE_CYCLES; ++n)
        {
            write_byte_field(op, obytes, sbo, CAN_TX_ID_FIRST_BIT, DM_MOTOR_ID);
            for (uint32 b = 0; b < CAN_TX_DATA_BYTES; ++b)
                write_byte_field(op, obytes, sbo, CAN_TX_DATA_FIRST_BIT + b * 8,
                                 DM_DISABLE_PAYLOAD[b]);

            ecx_send_processdata(&ctx);
            ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
            osal_usleep(1000);
        }

        printf("MOTOR: last frame sent, payload ");
        for (uint32 b = 0; b < CAN_TX_DATA_BYTES; ++b)
            printf("%02X%s", DM_DISABLE_PAYLOAD[b], (b + 1 < CAN_TX_DATA_BYTES) ? " " : "");
        printf(" on CAN id 0x%03X -- motor disabled.\n", DM_MOTOR_ID);
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
