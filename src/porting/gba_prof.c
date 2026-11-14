/*
 * gba_prof.c -- device PROF build (`make PROF=1`, GBA_PROF): DWT cycle
 * counts of the main loop's phases, printed every GBA_PROF_WINDOW frames.
 *
 *   PROF/clock: <MHz>, the audio rate and the frame rate, once
 *   PROF/frame: frames drawn fw_skip vskip wall_ms fps
 *               drawn = pictures presented (each one shows: one swap per
 *               LCD refresh), fw_skip = the firmware's frameskip, vskip =
 *               pictures dropped because the last swap was still pending,
 *               fps = drawn per second of wall time
 *   PROF/budget: budget busy heaviest     cycles per frame: the frame time
 *                                         at the real frame rate, the average
 *                                         work (all but the wait), the worst
 *                                         frame's
 *   PROF/emu: execute_arm per frame, split: arm (the ARM7 + memory + DMA +
 *             timers + sound FIFOs), ppu (update_scanline's drawing, drawn
 *             frames only), m4a (the mixer HLE)
 *   PROF/cpi: device cycles per interpreted GBA instruction (the arm part
 *             over the instructions), and the instructions per frame
 *   PROF/video: scale present per DRAWN frame
 *   PROF/pace: the waits per frame (the audio's and the LCD window's),
 *              frames over budget (their work, the waits left out, took
 *              longer than a frame: the real overruns), the ring's lowest
 *              fill at a frame's end (samples; 0 = it ran dry)
 *
 * Every figure is cycles of the core clock. busy / budget is the share of
 * the frame the emulation uses.
 *
 * PC SAMPLER (make PCS=1, with PROF=1; off by default: its SD writes stall
 * the loop and it swaps the interrupt table): while a replay runs, the firmware's 1 kHz SysTick interrupt
 * goes through gba_pcs_systick (the interrupt table copied to RAM, VTOR
 * pointed at the copy -- with the firmware ABI table after it, which cores
 * find at VTOR + 0x400), which counts the interrupted PC in a histogram and
 * then runs the firmware's own handler. Every GBA_PCS_BLOCK replay frames
 * (and at the replay's end) the histogram is appended to "<rom>.pcs" next
 * to the ROM and cleared: the same recording samples the same frames.
 * tools/gba_pcs_report.py maps it onto the build's ELF. Buckets: ITCM by
 * 16 bytes, the core's RAM_EMU code and gba.xip by 32; everything else
 * (the firmware: its ISRs, HAL, the audio wait's sleep) in one counter.
 * The firmware's table goes back on quit (gba_prof_quit).
 */
#include "gba_prof.h"

#if defined(GBA_PROF) && !defined(HOST_BUILD)
#include <odroid_system.h>
#include <stdio.h>
#include <string.h>
#include "common.h"
#include "gw_lcd.h"
#include "gw_malloc.h"
#include "stm32h7xx.h"
#include "gba_rec.h"
#include "gw_core_bridge.h"
#include "gba_timing.h"
#include "gba_idle.h"

uint32_t gba_prof_acc[GBA_PART_N];
uint32_t gba_prof_t0[GBA_PART_N];
uint32_t gba_prof_m4a_calls;
#ifdef GBA_ICOUNT
extern uint32_t gba_icount;           /* cpu.cc, CPI=1 */
#else
static uint32_t gba_icount;           /* no count: PROF/cpi is left out */
#endif
uint32_t gba_audio_queued(void);      /* main_gba.c: the ISR ring's fill */

static uint32_t ph[GBA_PH_N];
static uint32_t last, frame_t0, frame_wait, heaviest;
static uint32_t frames, drawn, fw_skip, vskip, over, ring_min = 0xffffffffu;
static uint32_t mhz, wall_t0, budget, icount0;

void gba_prof_init(unsigned spf, unsigned rate)
{
    (void)spf; (void)rate;
    *(volatile uint32_t *)0xE000EDFCu |= (1u << 24);   /* DEMCR.TRCENA */
    *(volatile uint32_t *)0xE0001000u |= 1u;           /* DWT_CTRL.CYCCNTENA */
    uint32_t t = HAL_GetTick();
    while (HAL_GetTick() == t) { }
    uint32_t c0 = GBA_PROF_NOW(), t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < 100u)
        wdog_refresh();
    mhz = (GBA_PROF_NOW() - c0 + 50000u) / 100000u;
    budget = (uint32_t)((uint64_t)mhz * 1000000000u / gba_timing_frame_mhz());
    printf("PROF/clock: %lu MHz, audio %lu.%03lu Hz, frame %lu.%03lu Hz (budget %lu cycles)\n",
           (unsigned long)mhz,
           (unsigned long)(gba_timing_rate_mhz() / 1000u), (unsigned long)(gba_timing_rate_mhz() % 1000u),
           (unsigned long)(gba_timing_frame_mhz() / 1000u), (unsigned long)(gba_timing_frame_mhz() % 1000u),
           (unsigned long)budget);
    wall_t0 = HAL_GetTick();
    icount0 = gba_icount;
    last = frame_t0 = GBA_PROF_NOW();
}

void gba_prof_mark(int phase)
{
    uint32_t now = GBA_PROF_NOW();
    uint32_t d = now - last;
    ph[phase] += d;
    if (phase == GBA_PH_WAIT)
        frame_wait += d;
    last = now;
}

static int pcs_frame(void);
static void frame_end(int was_drawn, int was_fw_skip, int was_vskip, int waited);

void gba_prof_frame_end(int was_drawn, int was_fw_skip, int was_vskip, int waited)
{
    frame_end(was_drawn, was_fw_skip, was_vskip, waited);
    if (pcs_frame())                   /* an SD write: charged to nobody */
        last = frame_t0 = GBA_PROF_NOW();
}

static void frame_end(int was_drawn, int was_fw_skip, int was_vskip, int waited)
{
    uint32_t now = GBA_PROF_NOW();
    uint32_t busy = now - frame_t0 - frame_wait;
    uint32_t q = gba_audio_queued();
    if (busy > heaviest)
        heaviest = busy;
    if (busy > budget)
        over++;
    if (q < ring_min)
        ring_min = q;
    frame_t0 = now;
    frame_wait = 0;
    frames++;
    drawn += was_drawn ? 1u : 0u;
    fw_skip += was_fw_skip ? 1u : 0u;
    vskip += was_vskip ? 1u : 0u;
    (void)waited;
    if (frames < GBA_PROF_WINDOW)
        return;

    uint32_t n = frames, d = drawn ? drawn : 1u;
    uint32_t wall = HAL_GetTick() - wall_t0;
    uint32_t busy_sum = 0;
    for (int i = 0; i < GBA_PH_N; i++)
        if (i != GBA_PH_WAIT)
            busy_sum += ph[i] / n;
    uint32_t emu = ph[GBA_PH_EMU] / n;
    uint32_t ppu = gba_prof_acc[GBA_PART_PPU] / n, m4a = gba_prof_acc[GBA_PART_M4A] / n;
    uint32_t arm = emu > ppu + m4a ? emu - ppu - m4a : 0;
    uint32_t instr = gba_icount - icount0;

    printf("PROF/frame: frames=%lu drawn=%lu fw_skip=%lu vskip=%lu wall_ms=%lu fps=%lu.%lu\n",
           (unsigned long)n, (unsigned long)drawn, (unsigned long)fw_skip, (unsigned long)vskip,
           (unsigned long)wall, (unsigned long)(wall ? drawn * 1000u / wall : 0),
           (unsigned long)(wall ? drawn * 10000u / wall % 10u : 0));
    printf("PROF/budget: budget=%lu busy=%lu (%lu%%) heaviest=%lu (cycles per frame, %lu MHz)\n",
           (unsigned long)budget, (unsigned long)busy_sum,
           (unsigned long)(budget ? (uint64_t)busy_sum * 100u / budget : 0),
           (unsigned long)heaviest, (unsigned long)mhz);
    printf("PROF/emu: emu=%lu arm=%lu ppu=%lu (%lu per drawn frame) m4a=%lu (%lu calls) framectl=%lu audio=%lu per frame\n",
           (unsigned long)emu, (unsigned long)arm, (unsigned long)ppu,
           (unsigned long)(gba_prof_acc[GBA_PART_PPU] / d), (unsigned long)m4a,
           (unsigned long)gba_prof_m4a_calls,
           (unsigned long)(ph[GBA_PH_FRAMECTL] / n), (unsigned long)(ph[GBA_PH_AUDIO] / n));
#ifdef GBA_ICOUNT
    printf("PROF/cpi: arm=%lu.%lu device cycles per GBA instruction (%lu instructions per frame)\n",
           (unsigned long)(instr ? (uint64_t)arm * n / instr : 0),
           (unsigned long)(instr ? (uint64_t)arm * n * 10u / instr % 10u : 0),
           (unsigned long)(instr / n));
#else
    (void)instr;
#endif
    {   /* the generic poll-loop skip (gba_idle.c): ARM7 cycles skipped per
         * frame (of 280,896), skips in the window, the armed loop */
        static uint32_t sk0, n0, a0, r0;
        printf("PROF/idle: skipped=%lu GBA cycles per frame, skips=%lu head=%08lx arms=%lu rejects=%lu per window\n",
               (unsigned long)((gba_idle_skipped - sk0) / n), (unsigned long)(gba_idle_skips - n0),
               (unsigned long)gba_idle_head_pc, (unsigned long)(gba_idle_arms - a0),
               (unsigned long)(gba_idle_rejects - r0));
        sk0 = gba_idle_skipped; n0 = gba_idle_skips; a0 = gba_idle_arms; r0 = gba_idle_rejects;
    }
    printf("PROF/video: scale=%lu present=%lu per drawn frame\n",
           (unsigned long)(ph[GBA_PH_SCALE] / d), (unsigned long)(ph[GBA_PH_PRESENT] / d));
    printf("PROF/pace: wait=%lu per frame over=%lu (frames over budget) ring_min=%lu (samples, of %lu per frame)\n",
           (unsigned long)(ph[GBA_PH_WAIT] / n), (unsigned long)over, (unsigned long)ring_min,
           (unsigned long)804u);

    for (int i = 0; i < GBA_PH_N; i++)
        ph[i] = 0;
    for (int i = 0; i < GBA_PART_N; i++)
        gba_prof_acc[i] = 0;
    gba_prof_m4a_calls = 0;
    frames = drawn = fw_skip = vskip = over = heaviest = 0;
    ring_min = 0xffffffffu;
    icount0 = gba_icount;
    wall_t0 = HAL_GetTick();
    /* the printing above is charged to nobody */
    last = frame_t0 = GBA_PROF_NOW();
}

#ifdef GBA_PCS
/* ------------------------------------------------------------ PC sampler */
#define GBA_PCS_BLOCK 1800u            /* replay frames per histogram block */
#define PCS_MAGIC     0x53435047u      /* "GPCS" */
#define ITCM_N        (0x10000u >> 4)

extern uint8_t __RAM_EMU_START__[];
extern uint8_t _OVERLAY_GBA_BSS_START[];

typedef struct {
    uint32_t magic, version, frame_from, frame_to;
    uint32_t xip_addr, ram_lo, itcm_n, ram_n, xip_n, other, total;
} pcs_header_t;

/* touched by the handler: plain globals, the handler lives in ITCM */
volatile uint32_t gba_pcs_on;
uint32_t gba_pcs_orig;                 /* the firmware's SysTick handler */
static uint16_t *pcs_itcm, *pcs_ram, *pcs_xip;
static uint32_t pcs_ram_lo, pcs_ram_n, pcs_xip_lo, pcs_xip_n, pcs_other, pcs_total;
static uint32_t *pcs_table, pcs_old_vtor, pcs_from;
static int pcs_state;                  /* 0 not set up, 1 installed, -1 failed */
static const uint8_t *pcs_xip_addr;
static uint32_t pcs_xip_size;

void gba_prof_set_xip(const void *addr, uint32_t size)
{
    pcs_xip_addr = (const uint8_t *)addr;
    pcs_xip_size = size;
}

__attribute__((section(".gba_itcm_text"), used))
void gba_pcs_hit(uint32_t pc)
{
    uint16_t *b;
    if (!gba_pcs_on)
        return;
    pcs_total++;
    if (pc < 0x10000u)
        b = &pcs_itcm[pc >> 4];
    else if (pc - pcs_ram_lo < pcs_ram_n * 32u)
        b = &pcs_ram[(pc - pcs_ram_lo) >> 5];
    else if (pc - pcs_xip_lo < pcs_xip_n * 32u)
        b = &pcs_xip[(pc - pcs_xip_lo) >> 5];
    else {
        pcs_other++;
        return;
    }
    if (*b != 0xFFFFu)
        (*b)++;
}

/* SysTick: count the PC the exception interrupted (the stacked frame's +24,
 * MSP or PSP as EXC_RETURN says), then the firmware's handler with the same
 * EXC_RETURN, as if it had been entered directly. */
__attribute__((section(".gba_itcm_text"), naked, used))
void gba_pcs_systick(void)
{
    __asm volatile(
        "tst   lr, #4            \n"
        "ite   eq                \n"
        "mrseq r0, msp           \n"
        "mrsne r0, psp           \n"
        "ldr   r0, [r0, #24]     \n"
        "push  {r4, lr}          \n"
        "bl    gba_pcs_hit       \n"
        "pop   {r4, lr}          \n"
        "ldr   r0, =gba_pcs_orig \n"
        "ldr   r0, [r0]          \n"
        "bx    r0                \n"
        ".ltorg                  \n");
}

static void pcs_clear(void)
{
    memset(pcs_itcm, 0, ITCM_N * 2u);
    memset(pcs_ram, 0, pcs_ram_n * 2u);
    memset(pcs_xip, 0, pcs_xip_n * 2u);
    pcs_other = pcs_total = 0;
}

static void pcs_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s", gba_rec_file());
    size_t n = strlen(out);
    if (n > 4 && strcmp(out + n - 4, ".rec") == 0)
        strcpy(out + n - 4, ".pcs");
    else
        strncat(out, ".pcs", cap - n - 1);
}

static void pcs_dump(uint32_t frame_to, int first)
{
    char path[320];
    pcs_header_t h = { PCS_MAGIC, 1, pcs_from, frame_to, (uint32_t)pcs_xip_lo, pcs_ram_lo,
                       ITCM_N, pcs_ram_n, pcs_xip_n, pcs_other, pcs_total };
    uint32_t on = gba_pcs_on;
    gba_pcs_on = 0;                    /* the write itself is not sampled */
    pcs_path(path, sizeof path);
    FILE *f = fopen(path, first ? "wb" : "ab");
    if (f) {
        fwrite(&h, sizeof h, 1, f);
        fwrite(pcs_itcm, 2, ITCM_N, f);
        fwrite(pcs_ram, 2, pcs_ram_n, f);
        fwrite(pcs_xip, 2, pcs_xip_n, f);
        fclose(f);
        printf("PROF/pcs: frames %lu-%lu, %lu samples -> %s\n", (unsigned long)pcs_from,
               (unsigned long)frame_to, (unsigned long)pcs_total, path);
    } else {
        printf("PROF/pcs: cannot write %s\n", path);
    }
    pcs_clear();
    gba_pcs_on = on;
    /* The SD write took ~0.2 s: tell the firmware's pacer, or it skips frames
     * to catch up (2 frameskips in the next PROF window, every block). */
    common_emu_frame_loop_reset();
}

static int pcs_install(void)
{
    pcs_ram_lo = (uint32_t)__RAM_EMU_START__;
    pcs_ram_n = ((uint32_t)_OVERLAY_GBA_BSS_START - pcs_ram_lo + 31u) / 32u;
    pcs_xip_lo = (uint32_t)pcs_xip_addr;
    pcs_xip_n = (pcs_xip_size + 31u) / 32u;
    pcs_itcm = (uint16_t *)ahb_calloc(ITCM_N, 2);
    pcs_ram = (uint16_t *)ahb_calloc(pcs_ram_n, 2);
    pcs_xip = (uint16_t *)ahb_calloc(pcs_xip_n ? pcs_xip_n : 1, 2);
    /* the copy holds the vector table AND the firmware ABI table: a core finds
     * the ABI at VTOR + GW_FIRMWARE_ABI_OFFSET (gw_firmware_abi()), so a copy
     * of the vectors alone would send every firmware call through zeros */
    const uint32_t copy = GW_FIRMWARE_ABI_OFFSET + ((sizeof(gw_firmware_abi_t) + 31u) & ~31u);
    uint8_t *raw = (uint8_t *)ahb_calloc(copy + 1024u, 1);
    if (!pcs_itcm || !pcs_ram || !pcs_xip || !raw) {
        printf("PROF/pcs: no AHB memory for the PC sampler (%lu bytes)\n",
               (unsigned long)((ITCM_N + pcs_ram_n + pcs_xip_n) * 2u + copy + 1024u));
        return -1;
    }
    /* VTOR needs the table aligned to its size rounded up to a power of two
     * (171 vectors: 1 KB) */
    pcs_table = (uint32_t *)(((uint32_t)raw + 1023u) & ~1023u);
    pcs_old_vtor = SCB->VTOR;
    memcpy(pcs_table, (const void *)pcs_old_vtor, copy);
    gba_pcs_orig = pcs_table[15];
    pcs_table[15] = (uint32_t)&gba_pcs_systick | 1u;
    SCB_CleanDCache_by_Addr(pcs_table, (int32_t)copy);
    __DSB();
    __disable_irq();
    SCB->VTOR = (uint32_t)pcs_table;
    __DSB();
    __ISB();
    __enable_irq();
    printf("PROF/pcs: sampling the PC at 1 kHz during replays (ITCM %u, code %lu, xip %lu buckets)\n",
           (unsigned)ITCM_N, (unsigned long)pcs_ram_n, (unsigned long)pcs_xip_n);
    return 1;
}

void gba_prof_quit(void)
{
    if (pcs_state != 1)
        return;
    gba_pcs_on = 0;
    __disable_irq();
    SCB->VTOR = pcs_old_vtor;
    __DSB();
    __ISB();
    __enable_irq();
    pcs_state = 0;
}

/* once a frame (gba_prof_frame_end): sample while a replay runs, a block
 * every GBA_PCS_BLOCK replay frames and at its end */
static int pcs_frame(void)
{
    static int first;
    int replaying = gba_rec_mode() == GBA_REC_REPLAY;
    if (replaying && !gba_pcs_on) {
        if (pcs_state == 0)
            pcs_state = pcs_install();
        if (pcs_state != 1)
            return 0;
        pcs_clear();
        pcs_from = gba_rec_frame();
        first = 1;
        gba_pcs_on = 1;
        return 0;
    }
    if (!gba_pcs_on)
        return 0;
    if (!replaying) {
        pcs_dump(gba_rec_frame(), first);   /* the replay's end */
        gba_pcs_on = 0;
        return 1;
    }
    if (gba_rec_frame() - pcs_from >= GBA_PCS_BLOCK) {
        uint32_t to = gba_rec_frame();
        pcs_dump(to, first);
        first = 0;
        pcs_from = to;
        return 1;
    }
    return 0;
}
#else
/* make PCS=1 adds the PC sampler; without it these are no-ops */
void gba_prof_set_xip(const void *addr, uint32_t size) { (void)addr; (void)size; }
void gba_prof_quit(void) {}
static int pcs_frame(void) { return 0; }
#endif /* GBA_PCS */
#endif
