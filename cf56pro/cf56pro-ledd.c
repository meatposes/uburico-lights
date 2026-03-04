/*
 * cf56pro-ledd.c - ORICO CF56Pro NAS LED daemon
 * uburico-lights v1.0.0
 *
 * Monitors ZFS pool membership and SMART health for each drive bay,
 * then writes to the LED class devices created by orico_cf56pro_leds.ko.
 *
 * LED states per bay (priority, first match wins):
 *   present && health_fail          -> fast red flash  (250ms)
 *   present && health_warning       -> slow red flash  (1000ms)
 *   expected_in_array && !present   -> solid red       (drive missing from pool)
 *   present                         -> solid blue
 *   (empty bay)                     -> off
 *
 * Flash is implemented by toggling sysfs brightness on a timer;
 * no kernel LED triggers are required.
 *
 * Config:  /etc/cf56pro-led.conf  (INI format, never overwritten on update)
 * Usage:   cf56pro-ledd [-f] [-c /path/to/config]
 *            -f   foreground mode (log to stderr instead of syslog)
 *            -c   config file path (default: /etc/cf56pro-led.conf)
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define NUM_BAYS            5
#define SYSFS_LEDS          "/sys/class/leds"
#define DEFAULT_CONFIG      "/etc/cf56pro-led.conf"
#define TICK_MS             50          /* main loop sleep in ms             */
#define DEVNAME_LEN         32          /* "/dev/sdX"                        */
#define LINE_LEN            512

/* SMART attribute IDs that trigger health_warning when RAW > 0 */
static const int SMART_WARN_IDS[] = { 197, 198, -1 };

/* =========================================================================
 * Types
 * ========================================================================= */

typedef enum {
    STATE_EMPTY = 0,
    STATE_GOOD,
    STATE_HEALTH_WARNING,
    STATE_HEALTH_FAIL,
    STATE_MISSING_EXPECTED,
} bay_state_t;

static const char * const STATE_NAMES[] = {
    "drive_empty",
    "drive_good",
    "drive_health_warning",
    "drive_health_fail",
    "drive_missing_expected",
};

typedef struct {
    int          num;
    const char  *hctl;
    char         devname[DEVNAME_LEN];  /* "/dev/sda" or "" if not present   */
    bool         present;
    bool         expected_in_array;
    bool         health_warning;
    bool         health_fail;
    bay_state_t  state;
    bay_state_t  prev_state;
} bay_t;

typedef struct {
    long  fast_period_ms;
    long  slow_period_ms;
    long  poll_interval_s;
    long  smart_interval_s;
    bool  zfs_enabled;
    bool  smart_enabled;
    bool  smart_respect_standby;
    unsigned long long io_threshold;  /* sectors/tick to trigger system LED */
    unsigned long long net_threshold; /* bytes/tick to trigger network LED  */
} config_t;

/* =========================================================================
 * Globals
 * ========================================================================= */

/* Diskstats: previous sample for delta calculation */
typedef struct {
    unsigned long long reads;
    unsigned long long writes;
} diskstats_t;

static diskstats_t g_prev_diskstats = {0, 0};

/* Netstats: previous sample for delta calculation */
typedef struct {
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
} netstats_t;

static netstats_t g_prev_netstats = {0, 0};

static volatile sig_atomic_t g_running = 1;
static bool                  g_foreground = false;
static const char           *g_config_path = DEFAULT_CONFIG;

/*
 * Bay table — HCTL assignments for the CF56Pro.
 *
 * TODO: These are PLACEHOLDER values. You MUST update them for your CF56Pro.
 * Run: ls /sys/class/scsi_device/
 * Then map each HCTL to a physical bay by inserting/removing drives.
 */
static bay_t g_bays[NUM_BAYS] = {
    { .num = 1, .hctl = "0:0:0:0" },
    { .num = 2, .hctl = "1:0:0:0" },
    { .num = 3, .hctl = "2:0:0:0" },
    { .num = 4, .hctl = "4:0:0:0" },
    { .num = 5, .hctl = "3:0:0:0" },
};

static config_t g_cfg = {
    .fast_period_ms        = 250,
    .slow_period_ms        = 1000,
    .poll_interval_s       = 15,
    .smart_interval_s      = 300,
    .zfs_enabled           = true,
    .smart_enabled         = true,
    .smart_respect_standby = true,
    .io_threshold          = 100,   /* raised to 100 to reduce 'violet' flicker */
    .net_threshold         = 64,    /* bytes per tick; filters idle keepalives   */
};

/* =========================================================================
 * Logging
 * ========================================================================= */

static void log_msg(int priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    if (g_foreground) {
        const char *tag = (priority <= LOG_ERR)    ? "ERROR" :
                          (priority == LOG_WARNING) ? "WARN " :
                          (priority == LOG_INFO)    ? "INFO " : "DEBUG";
        fprintf(stderr, "[%s] cf56pro-ledd: ", tag);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    } else {
        vsyslog(priority, fmt, ap);
    }

    va_end(ap);
}

#define log_err(...)   log_msg(LOG_ERR,     __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARNING, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO,    __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG,   __VA_ARGS__)

/* =========================================================================
 * Config parser (minimal INI)
 * ========================================================================= */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static bool parse_bool(const char *v)
{
    return (strcasecmp(v, "true") == 0 ||
            strcasecmp(v, "yes")  == 0 ||
            strcmp(v, "1")        == 0);
}

static void load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            log_info("config file %s not found, using defaults", path);
        else
            log_warn("cannot open config %s: %s — using defaults",
                     path, strerror(errno));
        return;
    }

    char line[LINE_LEN];
    int  lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = trim(line);

        /* skip blanks and comments */
        if (*p == '\0' || *p == '#' || *p == ';' || *p == '[')
            continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        /* strip inline comment */
        char *hash = strchr(val, '#');
        if (hash) { *hash = '\0'; trim(val); }

        if      (strcmp(key, "fast_period_ms")       == 0) g_cfg.fast_period_ms       = atol(val);
        else if (strcmp(key, "slow_period_ms")       == 0) g_cfg.slow_period_ms       = atol(val);
        else if (strcmp(key, "poll_interval_s")      == 0) g_cfg.poll_interval_s      = atol(val);
        else if (strcmp(key, "smart_interval_s")     == 0) g_cfg.smart_interval_s     = atol(val);
        else if (strcmp(key, "zfs_enabled")          == 0) g_cfg.zfs_enabled          = parse_bool(val);
        else if (strcmp(key, "smart_enabled")        == 0) g_cfg.smart_enabled        = parse_bool(val);
        else if (strcmp(key, "smart_respect_standby")== 0) g_cfg.smart_respect_standby= parse_bool(val);
        else if (strcmp(key, "io_threshold")  == 0) g_cfg.io_threshold  = (unsigned long long)atoll(val);
        else if (strcmp(key, "net_threshold") == 0) g_cfg.net_threshold = (unsigned long long)atoll(val);
        else
            log_warn("config line %d: unknown key '%s'", lineno, key);
    }

    fclose(f);
    log_info("loaded config from %s", path);
}

/* =========================================================================
 * Time helpers
 * ========================================================================= */

static long ms_since(const struct timespec *past)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec  - past->tv_sec)  * 1000L
         + (now.tv_nsec - past->tv_nsec) / 1000000L;
}

static void ts_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

/* Zero out a timespec so ms_since() returns a large value on first check */
static void ts_epoch(struct timespec *ts)
{
    ts->tv_sec  = 0;
    ts->tv_nsec = 0;
}

/* =========================================================================
 * LED sysfs helpers
 * ========================================================================= */

static int led_write(int bay, const char *color, int brightness)
{
    char path[256];
    snprintf(path, sizeof(path),
             "%s/cf56pro:bay%d:%s/brightness", SYSFS_LEDS, bay, color);

    FILE *f = fopen(path, "w");
    if (!f) {
        log_err("cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", brightness);
    fclose(f);
    return 0;
}

static bool leds_available(void)
{
    char path[256];
    snprintf(path, sizeof(path),
             "%s/cf56pro:bay1:blue/brightness", SYSFS_LEDS);
    struct stat st;
    return stat(path, &st) == 0;
}

/* =========================================================================
 * Drive discovery: HCTL -> /dev/sdX
 * ========================================================================= */

/*
 * For a given HCTL (e.g. "0:0:0:0"), look for:
 *   /sys/class/scsi_device/0:0:0:0/device/block/<devname>/
 * If found, populate bay->devname with "/dev/<devname>" and set present=true.
 */
static void discover_bay(bay_t *bay)
{
    char pattern[256];
    snprintf(pattern, sizeof(pattern),
             "/sys/class/scsi_device/%s/device/block/*/",
             bay->hctl);

    glob_t gl;
    memset(&gl, 0, sizeof(gl));

    if (glob(pattern, GLOB_NOSORT, NULL, &gl) != 0 || gl.gl_pathc == 0) {
        /* drive not present */
        bool was_present = bay->present;
        bay->present = false;
        bay->devname[0] = '\0';
        if (was_present)
            log_info("bay%d: drive removed", bay->num);
        globfree(&gl);
        return;
    }

    /* Take the last path component of the first match as the device name */
    const char *fullpath = gl.gl_pathv[0];
    /* trim trailing slash */
    char tmp[256];
    strncpy(tmp, fullpath, sizeof(tmp) - 1);
    tmp[sizeof(tmp)-1] = '\0';
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len-1] == '/')
        tmp[--len] = '\0';

    const char *slash = strrchr(tmp, '/');
    const char *devbase = slash ? slash + 1 : tmp;

    char newdev[DEVNAME_LEN];
    snprintf(newdev, sizeof(newdev), "/dev/%s", devbase);

    if (!bay->present || strcmp(bay->devname, newdev) != 0) {
        log_info("bay%d: drive present -> %s (HCTL %s)",
                 bay->num, newdev, bay->hctl);
    }

    strncpy(bay->devname, newdev, sizeof(bay->devname) - 1);
    bay->devname[sizeof(bay->devname)-1] = '\0';
    bay->present = true;

    globfree(&gl);
}

static void discover_all_drives(void)
{
    for (int i = 0; i < NUM_BAYS; i++)
        discover_bay(&g_bays[i]);
}

/* =========================================================================
 * ZFS: expected_in_array
 * =========================================================================
 *
 * Runs "zpool status -vPL" and collects every /dev/... token that appears.
 * Strips trailing partition digits (/dev/sda1 -> /dev/sda) when comparing
 * against bay device names, so we match regardless of ZFS partition usage.
 */

static void strip_partition(const char *dev, char *out, size_t outsz)
{
    strncpy(out, dev, outsz - 1);
    out[outsz - 1] = '\0';
    size_t l = strlen(out);
    while (l > 5 && isdigit((unsigned char)out[l-1]))  /* keep "/dev/sdX" */
        out[--l] = '\0';
}

static void refresh_zfs(void)
{
    if (!g_cfg.zfs_enabled) return;

    FILE *fp = popen("zpool status -vPL 2>/dev/null", "r");
    if (!fp) {
        log_warn("zpool status failed: %s", strerror(errno));
        return;
    }

    /* Collect base device names seen in pool config */
    char pool_devs[NUM_BAYS * 2][DEVNAME_LEN];
    int  pool_dev_count = 0;

    char line[LINE_LEN];
    while (fgets(line, sizeof(line), fp)) {
        /* Look for tokens starting with /dev/ on this line */
        char *p = line;
        while ((p = strstr(p, "/dev/")) != NULL) {
            /* extract the device token (up to whitespace) */
            char token[DEVNAME_LEN];
            int j = 0;
            while (*p && !isspace((unsigned char)*p) && j < (int)sizeof(token)-1)
                token[j++] = *p++;
            token[j] = '\0';

            char base[DEVNAME_LEN];
            strip_partition(token, base, sizeof(base));

            /* store if we have room and haven't seen it */
            if (pool_dev_count < (int)(sizeof(pool_devs)/sizeof(pool_devs[0]))) {
                bool dup = false;
                for (int k = 0; k < pool_dev_count; k++) {
                    if (strcmp(pool_devs[k], base) == 0) { dup = true; break; }
                }
                if (!dup) {
                    snprintf(pool_devs[pool_dev_count], DEVNAME_LEN, "%s", base);
                    pool_dev_count++;
                }
            }
        }
    }
    pclose(fp);

    /* Update expected_in_array for each bay */
    for (int i = 0; i < NUM_BAYS; i++) {
        bay_t *b = &g_bays[i];
        if (!b->present || b->devname[0] == '\0') {
            /* absent drives can still be "expected" if they were in the pool */
            /* we only update the flag when we can confirm either way */
            continue;
        }

        bool in_pool = false;
        for (int k = 0; k < pool_dev_count; k++) {
            if (strcmp(pool_devs[k], b->devname) == 0) {
                in_pool = true;
                break;
            }
        }
        b->expected_in_array = in_pool;
    }
}

/*
 * One-shot ZFS refresh that also sets expected_in_array for absent drives.
 * Run once at startup with all drives possibly present so we capture the
 * full expected set; thereafter refresh_zfs() handles present drives only.
 */
static void initial_zfs(void)
{
    if (!g_cfg.zfs_enabled) return;

    FILE *fp = popen("zpool status -vPL 2>/dev/null", "r");
    if (!fp) return;

    char pool_devs[NUM_BAYS * 4][DEVNAME_LEN];
    int  pool_dev_count = 0;

    char line[LINE_LEN];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while ((p = strstr(p, "/dev/")) != NULL) {
            char token[DEVNAME_LEN];
            int j = 0;
            while (*p && !isspace((unsigned char)*p) && j < (int)sizeof(token)-1)
                token[j++] = *p++;
            token[j] = '\0';

            char base[DEVNAME_LEN];
            strip_partition(token, base, sizeof(base));

            if (pool_dev_count < (int)(sizeof(pool_devs)/sizeof(pool_devs[0]))) {
                bool dup = false;
                for (int k = 0; k < pool_dev_count; k++) {
                    if (strcmp(pool_devs[k], base) == 0) { dup = true; break; }
                }
                if (!dup) {
                    snprintf(pool_devs[pool_dev_count], DEVNAME_LEN, "%s", base);
                    pool_dev_count++;
                }
            }
        }
    }
    pclose(fp);

    /*
     * Match pool devices against ALL bays (present or not).
     * We do a prefix match: pool device /dev/sda matches bay device /dev/sda.
     */
    for (int i = 0; i < NUM_BAYS; i++) {
        bay_t *b = &g_bays[i];
        if (b->devname[0] == '\0') continue;

        for (int k = 0; k < pool_dev_count; k++) {
            if (strcmp(pool_devs[k], b->devname) == 0) {
                b->expected_in_array = true;
                log_info("bay%d (%s): expected in ZFS array",
                         b->num, b->devname);
                break;
            }
        }
    }
}

/* =========================================================================
 * Network LED — aggregate traffic activity
 * =========================================================================
 *
 * Samples /proc/net/dev every tick and computes rx/tx byte deltas across
 * all interfaces except loopback (lo).
 *
 * LED mapping (network LED):
 *   tx only  -> blue   (outbound traffic)
 *   rx only  -> red    (inbound traffic)
 *   both     -> purple (bidirectional)
 *   idle     -> off
 */

static void refresh_network(void)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;

    unsigned long long total_rx = 0, total_tx = 0;
    char line[256];

    /* Skip two header lines */
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        char iface[64];
        unsigned long long rx_bytes, tx_bytes;

        /* /proc/net/dev format:
         *   iface: rx_bytes rx_packets rx_errs rx_drop rx_fifo rx_frame
         *          rx_compressed rx_multicast
         *          tx_bytes tx_packets ... */
        char *colon = strchr(line, ':');
        if (!colon) continue;

        *colon = ' ';   /* replace colon so sscanf sees iface as a token */

        if (sscanf(line, "%63s %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                   iface, &rx_bytes, &tx_bytes) != 3)
            continue;

        /* Skip loopback */
        if (strcmp(iface, "lo") == 0) continue;

        total_rx += rx_bytes;
        total_tx += tx_bytes;
    }
    fclose(f);

    unsigned long long delta_rx = total_rx - g_prev_netstats.rx_bytes;
    unsigned long long delta_tx = total_tx - g_prev_netstats.tx_bytes;

    g_prev_netstats.rx_bytes = total_rx;
    g_prev_netstats.tx_bytes = total_tx;

    /* Ignore first call — deltas from zero would be huge */
    static bool first_call = true;
    if (first_call) { first_call = false; return; }

    bool has_rx = (delta_rx >= g_cfg.net_threshold);
    bool has_tx = (delta_tx >= g_cfg.net_threshold);

    /* TX only -> blue; RX only -> red; both -> purple; idle -> off */
    int blue, red;
    if      (has_tx && !has_rx) { blue = 255; red = 0;   }
    else if (has_rx && !has_tx) { blue = 0;   red = 255; }
    else if (has_tx && has_rx)  { blue = 255; red = 255; }
    else                        { blue = 0;   red = 0;   }

    char path[256];
    FILE *sf;
    snprintf(path, sizeof(path), "%s/cf56pro:network:blue/brightness", SYSFS_LEDS);
    if ((sf = fopen(path, "w"))) { fprintf(sf, "%d\n", blue); fclose(sf); }
    snprintf(path, sizeof(path), "%s/cf56pro:network:red/brightness", SYSFS_LEDS);
    if ((sf = fopen(path, "w"))) { fprintf(sf, "%d\n", red);  fclose(sf); }
}

/* =========================================================================
 * System LED — aggregate disk I/O activity
 * =========================================================================
 *
 * Samples /proc/diskstats every tick and computes read/write deltas.
 * Filters to sd* and nvme* only to avoid noise from loop/dm/ram devices.
 *
 * LED mapping (system LED):
 *   writes only  -> red
 *   reads  only  -> blue
 *   both         -> purple (red + blue)
 *   idle         -> off
 *
 * /proc/diskstats column layout (kernel 4.18+):
 *   1  major  2  minor  3  name
 *   4  reads_completed   5  reads_merged   6  sectors_read    7  ms_reading
 *   8  writes_completed  9  writes_merged  10 sectors_written 11 ms_writing
 *   ... (more fields, we only need 6 and 10)
 */

static bool is_data_disk(const char *name)
{
    /* Accept sd[a-z], sda1 etc., nvme0n1, nvme0n1p1 etc. */
    if (strncmp(name, "sd", 2) == 0 && name[2] >= 'a' && name[2] <= 'z')
        return true;
    if (strncmp(name, "nvme", 4) == 0)
        return true;
    return false;
}

static void refresh_system_led(void)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;

    unsigned long long total_reads = 0, total_writes = 0;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        int major, minor;
        char name[64];
        unsigned long long rd_ios, rd_merge, rd_sectors;
        unsigned long long wr_ios, wr_merge, wr_sectors;

        int n = sscanf(line,
            "%d %d %63s "
            "%llu %llu %llu %*u "
            "%llu %llu %llu",
            &major, &minor, name,
            &rd_ios, &rd_merge, &rd_sectors,
            &wr_ios, &wr_merge, &wr_sectors);

        if (n < 9) continue;
        if (!is_data_disk(name)) continue;

        total_reads  += rd_sectors;
        total_writes += wr_sectors;
    }
    fclose(f);

    unsigned long long delta_r = total_reads  - g_prev_diskstats.reads;
    unsigned long long delta_w = total_writes - g_prev_diskstats.writes;

    g_prev_diskstats.reads  = total_reads;
    g_prev_diskstats.writes = total_writes;

    /* On the very first call (prev == 0) deltas will be huge — ignore */
    static bool first_call = true;
    if (first_call) { first_call = false; return; }

    bool has_read  = (delta_r >= g_cfg.io_threshold);
    bool has_write = (delta_w >= g_cfg.io_threshold);

    /* Reads only -> blue; writes only -> red; both -> purple; idle -> off */
    int blue, red;
    if      (has_read && !has_write) { blue = 255; red = 0;   }
    else if (has_write && !has_read) { blue = 0;   red = 255; }
    else if (has_read && has_write)  { blue = 255; red = 255; }
    else                             { blue = 0;   red = 0;   }

    char path[256];
    FILE *sf;
    snprintf(path, sizeof(path), "%s/cf56pro:system:blue/brightness", SYSFS_LEDS);
    if ((sf = fopen(path, "w"))) { fprintf(sf, "%d\n", blue); fclose(sf); }
    snprintf(path, sizeof(path), "%s/cf56pro:system:red/brightness", SYSFS_LEDS);
    if ((sf = fopen(path, "w"))) { fprintf(sf, "%d\n", red);  fclose(sf); }
}

/* =========================================================================
 * SMART health checks
 * ========================================================================= */

/*
 * Run smartctl on a single drive. Uses -n standby to avoid waking a
 * spun-down drive. Populates health_warning and health_fail.
 *
 * smartctl exit codes (bitmask):
 *   bit 0 (1): command line parse error
 *   bit 1 (2): device open error OR device in standby (with -n standby)
 *   bit 2 (4): SMART or ATA command failed
 *   bit 3 (8): SMART disk failing
 *   bit 4 (16): prefail attributes found in past
 *   bit 5 (32): SMART status check returned error
 *   bit 6 (64): self-test errors found
 *   bit 7 (128): device in low power mode
 */
static void smart_check_bay(bay_t *bay)
{
    if (!bay->present || bay->devname[0] == '\0') return;

    /* Build smartctl command */
    char cmd[256];
    if (g_cfg.smart_respect_standby)
        snprintf(cmd, sizeof(cmd),
                 "smartctl -n standby -H -A %s 2>/dev/null",
                 bay->devname);
    else
        snprintf(cmd, sizeof(cmd),
                 "smartctl -H -A %s 2>/dev/null",
                 bay->devname);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        log_warn("bay%d: popen smartctl failed: %s",
                 bay->num, strerror(errno));
        return;
    }

    bool found_health  = false;
    bool health_passed = true;
    bool warn          = false;
    bool in_standby    = false;

    char line[LINE_LEN];
    while (fgets(line, sizeof(line), fp)) {
        /* Standby detection */
        if (strstr(line, "CHECK POWER MODE"))      { in_standby = true; break; }
        if (strstr(line, "low power mode"))        { in_standby = true; break; }
        if (strstr(line, "standby"))               { in_standby = true; break; }

        /* Overall health */
        if (strstr(line, "overall-health self-assessment")) {
            found_health = true;
            if (strstr(line, "FAILED")) health_passed = false;
        }

        /* Attribute lines: "  ID# ATTRIBUTE ...  RAW_VALUE" */
        int id;
        long raw;
        if (sscanf(line, " %d %*s %*s %*s %*s %*s %*s %*s %*s %ld",
                   &id, &raw) == 2) {
            for (int j = 0; SMART_WARN_IDS[j] != -1; j++) {
                if (id == SMART_WARN_IDS[j] && raw > 0) {
                    log_warn("bay%d (%s): SMART attr %d raw=%ld",
                             bay->num, bay->devname, id, raw);
                    warn = true;
                }
            }
        }
    }

    int rc = pclose(fp);
    int status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

    /*
     * With -n standby, bit 1 set in exit code means device is in standby.
     * Treat this as "skip" — don't clear existing health flags.
     */
    if (in_standby || (g_cfg.smart_respect_standby && status != -1 && (status & 2))) {
        log_debug("bay%d (%s): drive in standby, skipping SMART",
                  bay->num, bay->devname);
        return;
    }

    if (!found_health) {
        log_debug("bay%d (%s): smartctl returned no health data (status %d)",
                  bay->num, bay->devname, status);
        return;
    }

    bay->health_fail    = !health_passed;
    bay->health_warning = warn && health_passed;  /* warning only if not already failing */
}

static void refresh_smart(void)
{
    if (!g_cfg.smart_enabled) return;

    for (int i = 0; i < NUM_BAYS; i++) {
        if (g_bays[i].present)
            smart_check_bay(&g_bays[i]);
    }
}

/* =========================================================================
 * State computation
 * ========================================================================= */

static bay_state_t compute_state(const bay_t *b)
{
    if (b->present && b->health_fail)                  return STATE_HEALTH_FAIL;
    if (b->present && b->health_warning)               return STATE_HEALTH_WARNING;
    if (b->expected_in_array && !b->present)           return STATE_MISSING_EXPECTED;
    if (b->present)                                    return STATE_GOOD;
    return STATE_EMPTY;
}

static void compute_all_states(void)
{
    for (int i = 0; i < NUM_BAYS; i++) {
        bay_t *b = &g_bays[i];
        b->prev_state = b->state;
        b->state      = compute_state(b);

        if (b->state != b->prev_state) {
            log_info("bay%d: %s -> %s",
                     b->num,
                     STATE_NAMES[b->prev_state],
                     STATE_NAMES[b->state]);
        }
    }
}

/* =========================================================================
 * LED update
 * ========================================================================= */

/*
 * Apply the current state to a bay's LEDs.
 * flash_fast and flash_slow are the current on/off phase for each period.
 */
static void update_bay_leds(const bay_t *b, bool flash_fast, bool flash_slow)
{
    int blue = 0, red = 0;

    switch (b->state) {
    case STATE_GOOD:
        blue = 255; red = 0;
        break;
    case STATE_HEALTH_FAIL:
        blue = 0;   red = flash_fast ? 255 : 0;
        break;
    case STATE_HEALTH_WARNING:
        blue = 0;   red = flash_slow ? 255 : 0;
        break;
    case STATE_MISSING_EXPECTED:
        blue = 0;   red = 255;
        break;
    case STATE_EMPTY:
    default:
        blue = 0;   red = 0;
        break;
    }

    led_write(b->num, "blue", blue);
    led_write(b->num, "red",  red);
}

static void update_all_leds(bool flash_fast, bool flash_slow)
{
    for (int i = 0; i < NUM_BAYS; i++)
        update_bay_leds(&g_bays[i], flash_fast, flash_slow);
}

static void all_leds_off(void)
{
    for (int i = 0; i < NUM_BAYS; i++) {
        led_write(g_bays[i].num, "blue", 0);
        led_write(g_bays[i].num, "red",  0);
    }
}

/* =========================================================================
 * Signal handling
 * ========================================================================= */

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
}

/* =========================================================================
 * Main loop
 * ========================================================================= */

static void main_loop(void)
{
    struct timespec last_poll, last_smart;
    struct timespec last_fast_toggle, last_slow_toggle;
    bool flash_fast = false;
    bool flash_slow = false;

    /* Trigger immediate poll+SMART on first iteration */
    ts_epoch(&last_poll);
    ts_epoch(&last_smart);
    ts_epoch(&last_fast_toggle);
    ts_epoch(&last_slow_toggle);

    log_info("entering main loop (poll=%lds, smart=%lds, fast=%ldms, slow=%ldms)",
             g_cfg.poll_interval_s, g_cfg.smart_interval_s,
             g_cfg.fast_period_ms,  g_cfg.slow_period_ms);

    while (g_running) {

        /* --- Flash phase toggles --- */
        if (ms_since(&last_fast_toggle) >= g_cfg.fast_period_ms / 2) {
            flash_fast = !flash_fast;
            ts_now(&last_fast_toggle);
        }
        if (ms_since(&last_slow_toggle) >= g_cfg.slow_period_ms / 2) {
            flash_slow = !flash_slow;
            ts_now(&last_slow_toggle);
        }

        /* --- Drive presence + ZFS refresh --- */
        if (ms_since(&last_poll) >= g_cfg.poll_interval_s * 1000L) {
            discover_all_drives();
            refresh_zfs();
            refresh_network();
            compute_all_states();
            ts_now(&last_poll);
        }

        /* --- SMART refresh (less frequent) --- */
        if (ms_since(&last_smart) >= g_cfg.smart_interval_s * 1000L) {
            refresh_smart();
            compute_all_states();
            ts_now(&last_smart);
        }

        /* --- Write bay LEDs --- */
        update_all_leds(flash_fast, flash_slow);

        /* --- System LED: aggregate disk I/O --- */
        refresh_system_led();

        /* --- Sleep one tick --- */
        struct timespec ts = { 0, TICK_MS * 1000000L };
        nanosleep(&ts, NULL);
    }
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-f] [-c /path/to/config]\n"
        "  -f  foreground mode (log to stderr)\n"
        "  -c  config file path (default: %s)\n",
        prog, DEFAULT_CONFIG);
}

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "fc:h")) != -1) {
        switch (opt) {
        case 'f': g_foreground   = true;       break;
        case 'c': g_config_path  = optarg;     break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!g_foreground)
        openlog("cf56pro-ledd", LOG_PID, LOG_DAEMON);

    log_info("cf56pro-ledd starting");

    /* Config */
    load_config(g_config_path);
    log_info("config: poll=%lds smart=%lds fast=%ldms slow=%ldms "
             "zfs=%s smart=%s standby=%s",
             g_cfg.poll_interval_s, g_cfg.smart_interval_s,
             g_cfg.fast_period_ms,  g_cfg.slow_period_ms,
             g_cfg.zfs_enabled  ? "on" : "off",
             g_cfg.smart_enabled ? "on" : "off",
             g_cfg.smart_respect_standby ? "respect" : "ignore");

    /* Sanity check: driver loaded? */
    if (!leds_available()) {
        log_err("LED sysfs nodes not found — is orico_cf56pro_leds.ko loaded?");
        log_err("Try: modprobe orico_cf56pro_leds");
        /* Don't exit — module might be loaded after we start on first boot.
         * The main loop will keep trying to write. */
    }

    setup_signals();

    /* Initial discovery */
    log_info("discovering drives...");
    discover_all_drives();

    log_info("querying ZFS pool membership...");
    initial_zfs();

    log_info("initialising network activity LED...");
    refresh_network();

    /* Prime diskstats baseline so first tick doesn't show spurious activity */
    refresh_system_led();

    if (g_cfg.smart_enabled) {
        log_info("running initial SMART checks...");
        refresh_smart();
    }

    compute_all_states();

    /* Log initial state */
    for (int i = 0; i < NUM_BAYS; i++) {
        bay_t *b = &g_bays[i];
        log_info("bay%d: %s%s state=%s",
                 b->num,
                 b->present ? b->devname : "(empty)",
                 b->expected_in_array ? " [in-pool]" : "",
                 STATE_NAMES[b->state]);
    }

    main_loop();

    /* Shutdown: turn everything off */
    log_info("shutting down, clearing LEDs");
    all_leds_off();

    if (!g_foreground)
        closelog();

    log_info("cf56pro-ledd stopped");
    return 0;
}
