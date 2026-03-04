/*
 * cf56pro_led_test.c - test utility for the orico_cf56pro_leds kernel driver
 *
 * Talks to the driver via /sys/class/leds/cf56pro:bayN:COLOR/brightness.
 * No /dev/mem, no root required for reads; writes need root (or udev rules).
 *
 * Build:  gcc -O2 -Wall -o cf56pro_led_test cf56pro_led_test.c
 *   (or:  make test-tool-cf56pro)
 *
 * Commands:
 *   status                     - print current brightness of all LEDs
 *   set <bay> <color> <on|off> - control one bay
 *   all <color> <on|off>       - control all 5 bays
 *   test                       - basic solid/off cycle (driver sanity check)
 *   sweep                      - Knight Rider sweep back and forth
 *   flash                      - split-speed flash test
 *   probe                      - toggle each LED one at a time for identification
 *
 * Bay:    1-5
 * Color:  blue | red | both
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define SYSFS_LEDS      "/sys/class/leds"
#define NUM_BAYS        5
#define BRIGHTNESS_ON   255
#define BRIGHTNESS_OFF  0

/* Timing constants (microseconds) */
#define SWEEP_STEP_US       120000   /* 120ms per bay step - classic KR pace  */
#define SWEEP_TAIL_US        60000   /* 60ms tail-dimming step (comet trail)  */
#define FLASH_TICK_US        50000   /* 50ms base tick for flash loop         */
#define FLASH_FAST_TICKS      5      /* 5 ticks x 50ms = 250ms half-period   */
#define FLASH_SLOW_TICKS     10      /* 10 ticks x 50ms = 500ms half-period  */
#define FLASH_DURATION_S     12      /* how long flash test runs              */
#define PROBE_HOLD_S          3      /* seconds to hold each LED during probe */

/* Non-bay LEDs exposed by the driver */
static const char * const EXTRA_LEDS[] = {
    "network", "system", NULL
};

/* -----------------------------------------------------------------------
 * Low-level sysfs helpers
 * ----------------------------------------------------------------------- */

static void make_path(const char *led_name, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/%s/brightness", SYSFS_LEDS, led_name);
}

static int led_exists(const char *led_name)
{
    char path[256];
    struct stat st;
    make_path(led_name, path, sizeof(path));
    return (stat(path, &st) == 0);
}

static int led_write(const char *led_name, int brightness)
{
    char path[256];
    FILE *f;

    make_path(led_name, path, sizeof(path));
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "  WRITE FAILED %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", brightness);
    fclose(f);
    return 0;
}

static int led_read(const char *led_name)
{
    char path[256];
    FILE *f;
    int val = -1;

    make_path(led_name, path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%d", &val) != 1)
        val = -1;
    fclose(f);
    return val;
}

/* -----------------------------------------------------------------------
 * Per-bay helpers
 * ----------------------------------------------------------------------- */

static void bay_led_name(int bay, const char *color, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "cf56pro:bay%d:%s", bay, color);
}

/* Write a bay LED without stdout chatter - used in animation loops */
static void bay_set_quiet(int bay, const char *color, int on)
{
    char name[64];
    bay_led_name(bay, color, name, sizeof(name));
    led_write(name, on ? BRIGHTNESS_ON : BRIGHTNESS_OFF);
}

/* Write a bay LED with a one-line log - used in manual commands */
static int bay_set_verbose(int bay, int do_blue, int do_red, int on)
{
    char name[64];
    int ret = 0;

    if (do_blue) {
        bay_led_name(bay, "blue", name, sizeof(name));
        printf("  bay%-2d blue %-3s\n", bay, on ? "ON" : "off");
        ret |= led_write(name, on ? BRIGHTNESS_ON : BRIGHTNESS_OFF);
    }
    if (do_red) {
        bay_led_name(bay, "red", name, sizeof(name));
        printf("  bay%-2d red  %-3s\n", bay, on ? "ON" : "off");
        ret |= led_write(name, on ? BRIGHTNESS_ON : BRIGHTNESS_OFF);
    }
    return ret;
}

/* Turn off every LED on every bay */
static void all_off_quiet(void)
{
    for (int bay = 1; bay <= NUM_BAYS; bay++) {
        bay_set_quiet(bay, "blue", 0);
        bay_set_quiet(bay, "red",  0);
    }
    /* Also turn off network and system LEDs */
    char name[64];
    for (const char * const *extra = EXTRA_LEDS; *extra; extra++) {
        snprintf(name, sizeof(name), "cf56pro:%s:blue", *extra);
        led_write(name, BRIGHTNESS_OFF);
        snprintf(name, sizeof(name), "cf56pro:%s:red", *extra);
        led_write(name, BRIGHTNESS_OFF);
    }
}

/* -----------------------------------------------------------------------
 * Driver presence check
 * ----------------------------------------------------------------------- */

static int check_driver(void)
{
    if (!led_exists("cf56pro:bay1:blue")) {
        fprintf(stderr,
            "ERROR: /sys/class/leds/cf56pro:bay1:blue not found.\n"
            "       Is the orico_cf56pro_leds module loaded?\n"
            "       Try: sudo insmod orico_cf56pro_leds.ko\n");
        return 0;
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * SIGINT handler - clean shutdown from animation loops
 * ----------------------------------------------------------------------- */

static volatile int g_stop = 0;

static void handle_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void setup_sigint(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
}

/* -----------------------------------------------------------------------
 * cmd_status
 * ----------------------------------------------------------------------- */

static void cmd_status(void)
{
    int bay, bval, rval;
    const char * const *extra;
    char name[64];

    if (!check_driver()) return;

    printf("%-24s  %s\n", "LED", "brightness");
    printf("%-24s  %s\n", "------------------------", "----------");

    for (bay = 1; bay <= NUM_BAYS; bay++) {
        bay_led_name(bay, "blue", name, sizeof(name));
        bval = led_read(name);
        bay_led_name(bay, "red", name, sizeof(name));
        rval = led_read(name);
        printf("  bay%-2d   blue=%-3s  red=%-3s\n",
               bay,
               bval < 0 ? "???" : bval ? "ON " : "off",
               rval < 0 ? "???" : rval ? "ON " : "off");
    }

    printf("\n");
    for (extra = EXTRA_LEDS; *extra; extra++) {
        snprintf(name, sizeof(name), "cf56pro:%s:blue", *extra);
        bval = led_read(name);
        snprintf(name, sizeof(name), "cf56pro:%s:red", *extra);
        rval = led_read(name);
        printf("  %-8s blue=%-3s  red=%-3s\n",
               *extra,
               bval < 0 ? "???" : bval ? "ON " : "off",
               rval < 0 ? "???" : rval ? "ON " : "off");
    }
}

/* -----------------------------------------------------------------------
 * cmd_test - basic solid-color sanity check
 * ----------------------------------------------------------------------- */

static void cmd_test(void)
{
    int bay;

    if (!check_driver()) return;

    printf("=== CF56Pro basic driver test ===\n");
    printf("(Ctrl-C to abort at any point)\n\n");
    setup_sigint();

    printf("[1/4] All LEDs OFF\n");
    all_off_quiet();
    sleep(1);
    if (g_stop) goto done;

    printf("[2/4] All BLUE ON\n");
    for (bay = 1; bay <= NUM_BAYS; bay++)
        bay_set_quiet(bay, "blue", 1);
    sleep(2);
    if (g_stop) goto done;

    printf("[3/4] All RED ON\n");
    for (bay = 1; bay <= NUM_BAYS; bay++)
        bay_set_quiet(bay, "red", 1);
    sleep(2);
    if (g_stop) goto done;

    printf("[4/4] Flash all red 4x\n");
    all_off_quiet();
    for (int i = 0; i < 4 && !g_stop; i++) {
        for (bay = 1; bay <= NUM_BAYS; bay++)
            bay_set_quiet(bay, "red", 1);
        usleep(200000);
        for (bay = 1; bay <= NUM_BAYS; bay++)
            bay_set_quiet(bay, "red", 0);
        usleep(200000);
    }

done:
    printf("\nAll LEDs OFF\n");
    all_off_quiet();
    printf("Done.\n");
}

/* -----------------------------------------------------------------------
 * cmd_sweep - Knight Rider style
 *
 * A blue or red "eye" sweeps bay 1->5->1 for 3 full passes.
 * Color alternates blue/red/purple each pass so both LED circuits
 * are tested.
 * ----------------------------------------------------------------------- */

static void sweep_on(int bay, const char *color)
{
    if (strcmp(color, "purple") == 0) {
        bay_set_quiet(bay, "blue", 1);
        bay_set_quiet(bay, "red",  1);
    } else {
        bay_set_quiet(bay, color, 1);
    }
}

static void sweep_off(int bay, const char *color)
{
    if (strcmp(color, "purple") == 0) {
        bay_set_quiet(bay, "blue", 0);
        bay_set_quiet(bay, "red",  0);
    } else {
        bay_set_quiet(bay, color, 0);
    }
}

static void cmd_sweep(void)
{
    if (!check_driver()) return;

    printf("=== Knight Rider sweep ===\n");
    printf("  Pass 1: blue\n");
    printf("  Pass 2: red\n");
    printf("  Pass 3: purple\n");
    printf("Ctrl-C to stop.\n\n");
    setup_sigint();
    all_off_quiet();

    /* Pass colors: blue, red, purple */
    const char *colors[] = { "blue", "red", "purple" };

    for (int pass = 0; pass < 3 && !g_stop; pass++) {
        const char *color = colors[pass];
        printf("Pass %d: sweeping %s\n", pass + 1, color);

        /* Forward: bay 1 -> 5 */
        for (int bay = 1; bay <= NUM_BAYS && !g_stop; bay++) {
            sweep_on(bay, color);
            if (bay > 1)
                sweep_off(bay - 1, color);
            usleep(SWEEP_STEP_US);
        }
        /* hold end bay, then clear it before reversing */
        usleep(SWEEP_STEP_US);
        sweep_off(NUM_BAYS, color);
        if (g_stop) break;

        /* Back: bay 5 -> 1 */
        for (int bay = NUM_BAYS; bay >= 1 && !g_stop; bay--) {
            sweep_on(bay, color);
            if (bay < NUM_BAYS)
                sweep_off(bay + 1, color);
            usleep(SWEEP_STEP_US);
        }
        /* hold start bay, then clear it before next pass */
        usleep(SWEEP_STEP_US);
        sweep_off(1, color);
    }

    all_off_quiet();
    printf("Sweep done.\n");
}

/* -----------------------------------------------------------------------
 * cmd_flash - split-speed flash test
 *
 * Layout (5 bays):
 *   Bays 1-3   red  FAST  (250ms on / 250ms off)
 *   Bays 4-5   blue SLOW  (500ms on / 500ms off)
 *
 * Runs for FLASH_DURATION_S seconds then turns everything off.
 * Ctrl-C also exits cleanly.
 * ----------------------------------------------------------------------- */

static void cmd_flash(void)
{
    if (!check_driver()) return;

    printf("=== Split-speed flash test ===\n");
    printf("  Bays 1-3  : red  FAST (250ms period)\n");
    printf("  Bays 4-5  : blue SLOW (500ms period)\n");
    printf("Running %d seconds. Ctrl-C to stop early.\n\n", FLASH_DURATION_S);

    setup_sigint();
    all_off_quiet();

    /* Per-bay on/off state, indexed 0-based */
    int state[NUM_BAYS] = {0};

    int fast_ticks = 0;
    int slow_ticks = 0;
    int fast_on    = 0;
    int slow_on    = 0;

    long total_ticks = (long)FLASH_DURATION_S * 1000000L / FLASH_TICK_US;

    for (long tick = 0; tick < total_ticks && !g_stop; tick++) {

        fast_ticks++;
        slow_ticks++;

        int fast_changed = 0;
        if (fast_ticks >= FLASH_FAST_TICKS) {
            fast_on    = !fast_on;
            fast_ticks = 0;
            fast_changed = 1;
        }

        int slow_changed = 0;
        if (slow_ticks >= FLASH_SLOW_TICKS) {
            slow_on    = !slow_on;
            slow_ticks = 0;
            slow_changed = 1;
        }

        /* Only write on transitions to avoid hammering sysfs */
        if (fast_changed) {
            for (int bay = 1; bay <= 3; bay++) {
                if (state[bay-1] != fast_on) {
                    bay_set_quiet(bay, "red", fast_on);
                    state[bay-1] = fast_on;
                }
            }
        }

        if (slow_changed) {
            for (int bay = 4; bay <= 5; bay++) {
                if (state[bay-1] != slow_on) {
                    bay_set_quiet(bay, "blue", slow_on);
                    state[bay-1] = slow_on;
                }
            }
        }

        usleep(FLASH_TICK_US);
    }

    all_off_quiet();
    printf("Flash test done.\n");
}

/* -----------------------------------------------------------------------
 * cmd_probe - identify each LED one at a time
 *
 * Turns on each LED individually for PROBE_HOLD_S seconds so you can
 * visually identify which physical LED corresponds to which sysfs node.
 * This is essential for verifying GPIO register mappings on new hardware.
 * ----------------------------------------------------------------------- */

static void cmd_probe(void)
{
    char name[64];

    if (!check_driver()) return;

    printf("=== CF56Pro LED probe ===\n");
    printf("Each LED will turn on for %d seconds.\n", PROBE_HOLD_S);
    printf("Watch your NAS and note which physical LED lights up.\n");
    printf("Ctrl-C to stop.\n\n");

    setup_sigint();
    all_off_quiet();

    /* Probe each bay LED individually */
    for (int bay = 1; bay <= NUM_BAYS && !g_stop; bay++) {
        /* Blue */
        printf(">>> bay%d BLUE  — look for a blue LED in bay slot %d\n", bay, bay);
        bay_set_quiet(bay, "blue", 1);
        sleep(PROBE_HOLD_S);
        bay_set_quiet(bay, "blue", 0);
        if (g_stop) break;

        /* Red */
        printf(">>> bay%d RED   — look for a red LED in bay slot %d\n", bay, bay);
        bay_set_quiet(bay, "red", 1);
        sleep(PROBE_HOLD_S);
        bay_set_quiet(bay, "red", 0);
        if (g_stop) break;
    }

    /* Probe network LED */
    if (!g_stop) {
        printf(">>> NETWORK BLUE — look for the network indicator\n");
        snprintf(name, sizeof(name), "cf56pro:network:blue");
        led_write(name, BRIGHTNESS_ON);
        sleep(PROBE_HOLD_S);
        led_write(name, BRIGHTNESS_OFF);
    }
    if (!g_stop) {
        printf(">>> NETWORK RED  — look for the network indicator\n");
        snprintf(name, sizeof(name), "cf56pro:network:red");
        led_write(name, BRIGHTNESS_ON);
        sleep(PROBE_HOLD_S);
        led_write(name, BRIGHTNESS_OFF);
    }

    /* Probe system LED */
    if (!g_stop) {
        printf(">>> SYSTEM BLUE  — look for the system indicator\n");
        snprintf(name, sizeof(name), "cf56pro:system:blue");
        led_write(name, BRIGHTNESS_ON);
        sleep(PROBE_HOLD_S);
        led_write(name, BRIGHTNESS_OFF);
    }
    if (!g_stop) {
        printf(">>> SYSTEM RED   — look for the system indicator\n");
        snprintf(name, sizeof(name), "cf56pro:system:red");
        led_write(name, BRIGHTNESS_ON);
        sleep(PROBE_HOLD_S);
        led_write(name, BRIGHTNESS_OFF);
    }

    all_off_quiet();
    printf("\nProbe complete.\n");
    printf("If any LED didn't light up, the GPIO register offset for that\n");
    printf("LED needs to be corrected in orico_cf56pro_leds.c\n");
}

/* -----------------------------------------------------------------------
 * Usage / main
 * ----------------------------------------------------------------------- */

static void usage(const char *prog)
{
    printf("CF56Pro LED test utility - talks to orico_cf56pro_leds.ko via sysfs\n\n");
    printf("Usage:\n");
    printf("  %s status                        show all LED states\n", prog);
    printf("  %s set <bay> <color> <on|off>    control one bay\n", prog);
    printf("  %s all <color> <on|off>          control all 5 bays\n", prog);
    printf("  %s test                          basic solid/off driver check\n", prog);
    printf("  %s sweep                         Knight Rider sweep (3 passes)\n", prog);
    printf("  %s flash                         split-speed flash test\n", prog);
    printf("  %s probe                         identify each LED one at a time\n\n", prog);
    printf("  bay    : 1-5\n");
    printf("  color  : blue | red | both\n\n");
    printf("Examples:\n");
    printf("  sudo %s set 1 blue on\n", prog);
    printf("  sudo %s all both off\n", prog);
    printf("  sudo %s sweep\n", prog);
    printf("  sudo %s probe\n\n", prog);
    printf("Note: writes require root (or a udev rule granting group access).\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "status") == 0) { cmd_status(); return 0; }
    if (strcmp(argv[1], "test")   == 0) { cmd_test();   return 0; }
    if (strcmp(argv[1], "sweep")  == 0) { cmd_sweep();  return 0; }
    if (strcmp(argv[1], "flash")  == 0) { cmd_flash();  return 0; }
    if (strcmp(argv[1], "probe")  == 0) { cmd_probe();  return 0; }

    if (argc < 4) { usage(argv[0]); return 1; }

    const char *color = argv[2];
    const char *state = argv[3];
    int do_blue = (strcmp(color, "blue") == 0 || strcmp(color, "both") == 0);
    int do_red  = (strcmp(color, "red")  == 0 || strcmp(color, "both") == 0);
    int on      = (strcmp(state, "on") == 0);

    if (!do_blue && !do_red) {
        fprintf(stderr, "Unknown color '%s'. Use: blue | red | both\n", color);
        return 1;
    }

    if (!check_driver()) return 1;

    if (strcmp(argv[1], "all") == 0) {
        for (int bay = 1; bay <= NUM_BAYS; bay++)
            bay_set_verbose(bay, do_blue, do_red, on);
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 5) { usage(argv[0]); return 1; }
        int bay = atoi(argv[2]);
        if (bay < 1 || bay > NUM_BAYS) {
            fprintf(stderr, "Invalid bay '%s'. Valid range: 1-5\n", argv[2]);
            return 1;
        }
        color   = argv[3];
        state   = argv[4];
        do_blue = (strcmp(color, "blue") == 0 || strcmp(color, "both") == 0);
        do_red  = (strcmp(color, "red")  == 0 || strcmp(color, "both") == 0);
        on      = (strcmp(state, "on") == 0);
        bay_set_verbose(bay, do_blue, do_red, on);
        return 0;
    }

    usage(argv[0]);
    return 1;
}
