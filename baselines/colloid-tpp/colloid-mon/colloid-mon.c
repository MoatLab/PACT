#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/delay.h>

/* #define SPINPOLL */ /* TODO: configure this */
#define SAMPLE_INTERVAL_MS 100 /* Only used if SPINPOLL is not set */
#ifdef SPINPOLL
#define EWMA_EXP 5
#else
#define EWMA_EXP 1
#endif

extern int colloid_local_lat_gt_remote;
extern int colloid_nid_of_interest;

#define CORE_MON 1
#define LOCAL_NUMA 0
#define WORKER_BUDGET 1000000
#define LOG_SIZE 10000
#define MIN_LOCAL_LAT 15
#define MIN_REMOTE_LAT 30

/*
 * CHA counters are MSR-based.
 *   The starting MSR address is 0x0E00 + 0x10*CHA
 *      Offset 0 is Unit Control -- mostly un-needed
 *      Offsets 1-4 are the Counter PerfEvtSel registers
 *      Offset 5 is Filter0 -- selects state for LLC lookup event (and TID, if enabled by bit 19 of PerfEvtSel)
 *      Offset 6 is Filter1 -- lots of filter bits, including opcode -- default if unused should be 0x03b, or 0x------33 if using opcode matching
 *      Offset 7 is Unit Status
 *      Offsets 8,9,A,B are the Counter count registers
 */
#define CHA_MSR_PMON_BASE 0x0E00L
#define CHA_MSR_PMON_CTL_BASE 0x0E01L
/* #define CHA_MSR_PMON_FILTER0_BASE 0x0E05L */
#define CHA_MSR_PMON_FILTER1_BASE 0x0E06L /* No Filter1 on Icelake, use Filter1 on skylake */
#define CHA_MSR_PMON_STATUS_BASE 0x0E07L
#define CHA_MSR_PMON_CTR_BASE 0x0E08L

#define NUM_CHA_BOXES 10    /* only 10 CHAs on skylake */
#define NUM_CHA_COUNTERS 4

#define frequency 2200 /* MHz */

u64 smoothed_occ_local, smoothed_inserts_local;
u64 smoothed_occ_remote, smoothed_inserts_remote;
u64 smoothed_lat_local, smoothed_lat_remote;
u64 colloid_cha_0_delta_tsc, colloid_cha_0_clockticks;
u64 colloid_cha_1_delta_tsc, colloid_cha_1_clockticks;

void thread_fun_poll_cha(struct work_struct *);
struct workqueue_struct *poll_cha_queue;
#ifdef SPINPOLL
struct work_struct poll_cha;
#else
DECLARE_DELAYED_WORK(poll_cha, thread_fun_poll_cha);
#endif

u64 cur_ctr_tsc[NUM_CHA_BOXES][NUM_CHA_COUNTERS], prev_ctr_tsc[NUM_CHA_BOXES][NUM_CHA_COUNTERS];
u64 cur_ctr_val[NUM_CHA_BOXES][NUM_CHA_COUNTERS], prev_ctr_val[NUM_CHA_BOXES][NUM_CHA_COUNTERS];
int terminate_mon;

struct log_entry {
    u64 tsc;
    u64 occ_local;
    u64 inserts_local;
    u64 occ_remote;
    u64 inserts_remote;
};

struct log_entry log_buffer[LOG_SIZE];
int log_idx;

static inline __attribute__((always_inline)) unsigned long rdtscp(void)
{
    unsigned long a, d, c;

    __asm__ volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));

    return (a | (d << 32));
}

static void poll_cha_init(void)
{
    int cha, ret;
    u32 msr_num;
    u64 msr_val;
    for (cha = 0; cha < NUM_CHA_BOXES; cha++) {
        /* use filter1 on skylake
         * msr_num = CHA_MSR_PMON_FILTER0_BASE + (0xE * cha); // Filter0
         * msr_val = 0x00000000; // default; no filtering
         * ret = wrmsr_on_cpu(CORE_MON, msr_num, msr_val & 0xFFFFFFFF, msr_val >> 32);
         * if(ret != 0) {
         *     printk(KERN_ERR "wrmsr FILTER0 failed\n");
         *     return;
         * }
         */

        msr_num = CHA_MSR_PMON_FILTER1_BASE + (0x10 * cha); /* Filter1 */
        msr_val = (cha % 2 == 0) ? (0x40432) : (0x40431);   /* Filter DRd of local/remote on even/odd CHA boxes */
        ret = wrmsr_on_cpu(CORE_MON, msr_num, msr_val & 0xFFFFFFFF, msr_val >> 32);
        if (ret != 0) {
            printk(KERN_ERR "wrmsr FILTER1 failed\n");
            return;
        }

        msr_num = CHA_MSR_PMON_CTL_BASE + (0x10 * cha) + 0; /* control / counter 0 */
        msr_val = 0x402136;                                 /* TOR Occupancy, DRd, Miss on CHA boxes */
        ret = wrmsr_on_cpu(CORE_MON, msr_num, msr_val & 0xFFFFFFFF, msr_val >> 32);
        if (ret != 0) {
            printk(KERN_ERR "wrmsr COUNTER 0 failed\n");
            return;
        }

        msr_num = CHA_MSR_PMON_CTL_BASE + (0x10 * cha) + 1; /* control / counter 1 */
        msr_val = 0x402135;                                 /* TOR Inserts, DRd, Miss, on CHA boxes */
        ret = wrmsr_on_cpu(CORE_MON, msr_num, msr_val & 0xFFFFFFFF, msr_val >> 32);
        if (ret != 0) {
            printk(KERN_ERR "wrmsr COUNTER 1 failed\n");
            return;
        }

        msr_num = CHA_MSR_PMON_CTL_BASE + (0x10 * cha) + 2; /* control / counter 2 */
        msr_val = 0x400000;                                 /* CLOCKTICKS */
        ret = wrmsr_on_cpu(CORE_MON, msr_num, msr_val & 0xFFFFFFFF, msr_val >> 32);
        if (ret != 0) {
            printk(KERN_ERR "wrmsr COUNTER 2 failed\n");
            return;
        }
    }
}

static inline void sample_cha_ctr(int cha, int ctr)
{
    u32 msr_num, msr_high, msr_low;
    msr_num = CHA_MSR_PMON_CTR_BASE + (0x10 * cha) + ctr;
    rdmsr_on_cpu(CORE_MON, msr_num, &msr_low, &msr_high);
    prev_ctr_val[cha][ctr] = cur_ctr_val[cha][ctr];
    cur_ctr_val[cha][ctr] = (((u64)msr_high) << 32) | msr_low;
    prev_ctr_tsc[cha][ctr] = cur_ctr_tsc[cha][ctr];
    cur_ctr_tsc[cha][ctr] = rdtscp();
}

static void dump_log(void)
{
    int i;
    pr_info("Dumping colloid mon log");
    for (i = 0; i < LOG_SIZE; i++) {
        printk("%llu %llu %llu %llu %llu\n", log_buffer[i].tsc, log_buffer[i].occ_local, log_buffer[i].inserts_local, log_buffer[i].occ_remote, log_buffer[i].inserts_remote);
    }
}

void thread_fun_poll_cha(struct work_struct *work)
{
    int cpu = CORE_MON;
    #ifdef SPINPOLL
    u32 budget = WORKER_BUDGET;
    #else
    u32 budget = 1;
    #endif
    u64 cha_0_delta_tsc, cha_1_delta_tsc;
    u64 cha_0_cur_occ, cha_0_cur_inserts, cha_0_clockticks;
    u64 cha_1_cur_occ, cha_1_cur_inserts, cha_1_clockticks;
    u64 cha_0_lat, cha_1_lat;

    while (budget) {
        // Sample counters and update state
        // TODO: For starters using CHA0 for local and CHA1 for remote
        sample_cha_ctr(0, 0); // CHA0 occupancy
        sample_cha_ctr(0, 1); // CHA0 inserts
        sample_cha_ctr(1, 0);
        sample_cha_ctr(1, 1);
        sample_cha_ctr(0, 2);
        sample_cha_ctr(1, 2);

        cha_0_cur_occ = cur_ctr_val[0][0] - prev_ctr_val[0][0];
        cha_0_cur_inserts = cur_ctr_val[0][1] - prev_ctr_val[0][1];
        cha_0_delta_tsc = cur_ctr_tsc[0][0] - prev_ctr_tsc[0][0];
        cha_0_clockticks = cur_ctr_val[0][2] - prev_ctr_val[0][2];

        cha_1_cur_occ = cur_ctr_val[1][0] - prev_ctr_val[1][0];
        cha_1_cur_inserts = cur_ctr_val[1][1] - prev_ctr_val[1][1];
        cha_1_delta_tsc = cur_ctr_tsc[1][0] - prev_ctr_tsc[1][0];
        cha_1_clockticks = cur_ctr_val[1][2] - prev_ctr_val[1][2];

        WRITE_ONCE(smoothed_occ_local, (cha_0_cur_occ + ((1 << EWMA_EXP) - 1) * smoothed_occ_local) >> EWMA_EXP);
        WRITE_ONCE(smoothed_inserts_local, (cha_0_cur_inserts + ((1 << EWMA_EXP) - 1) * smoothed_inserts_local) >> EWMA_EXP);
        cha_0_lat = (smoothed_inserts_local > 0 && cha_0_clockticks > 0) ? (smoothed_occ_local / smoothed_inserts_local) * (cha_0_delta_tsc * 1000 / frequency) / (cha_0_clockticks) : (MIN_LOCAL_LAT);

        WRITE_ONCE(smoothed_occ_remote, (cha_1_cur_occ + ((1 << EWMA_EXP) - 1) * smoothed_occ_remote) >> EWMA_EXP);
        WRITE_ONCE(smoothed_inserts_remote, (cha_1_cur_inserts + ((1 << EWMA_EXP) - 1) * smoothed_inserts_remote) >> EWMA_EXP);
        cha_1_lat = (smoothed_inserts_remote > 0 && cha_1_clockticks > 0) ? (smoothed_occ_remote / smoothed_inserts_remote) * (cha_1_delta_tsc * 1000 / frequency) / (cha_1_clockticks) : (MIN_REMOTE_LAT);

        WRITE_ONCE(smoothed_lat_local, (cha_0_lat > MIN_LOCAL_LAT) ? (cha_0_lat) : (MIN_LOCAL_LAT));
        WRITE_ONCE(smoothed_lat_remote, (cha_1_lat > MIN_REMOTE_LAT) ? (cha_1_lat) : (MIN_REMOTE_LAT));

        WRITE_ONCE(colloid_local_lat_gt_remote, (smoothed_lat_local > smoothed_lat_remote));

        /* store the time interval */
        WRITE_ONCE(colloid_cha_0_delta_tsc, cha_0_delta_tsc);
        WRITE_ONCE(colloid_cha_1_delta_tsc, cha_1_delta_tsc);
        WRITE_ONCE(colloid_cha_0_clockticks, cha_0_clockticks);
        WRITE_ONCE(colloid_cha_1_clockticks, cha_1_clockticks);

        /* log_idx = (log_idx+1)%LOG_SIZE; */

        budget--;
    }
    if (!READ_ONCE(terminate_mon)) {
        #ifdef SPINPOLL
        queue_work_on(cpu, poll_cha_queue, &poll_cha);
        #else
        queue_delayed_work_on(cpu, poll_cha_queue, &poll_cha, msecs_to_jiffies(SAMPLE_INTERVAL_MS));
        #endif
    } else {
        return;
    }
}

static void init_mon_state(void)
{
    int cha, ctr;
    for (cha = 0; cha < NUM_CHA_BOXES; cha++) {
        for (ctr = 0; ctr < NUM_CHA_COUNTERS; ctr++) {
            cur_ctr_tsc[cha][ctr] = 0;
            cur_ctr_val[cha][ctr] = 0;
            sample_cha_ctr(cha, ctr);
        }
    }
    log_idx = 0;
}

static int colloidmon_init(void)
{
    poll_cha_queue = alloc_workqueue("poll_cha_queue",  WQ_HIGHPRI | WQ_CPU_INTENSIVE, 0);
    if (!poll_cha_queue) {
        printk(KERN_ERR "Failed to create CHA workqueue\n");
        return -ENOMEM;
    }

    #ifdef SPINPOLL
    INIT_WORK(&poll_cha, thread_fun_poll_cha);
    #else
    INIT_DELAYED_WORK(&poll_cha, thread_fun_poll_cha);
    #endif
    poll_cha_init();
    pr_info("Programmed counters");
    /* Initialize state */
    init_mon_state();
    WRITE_ONCE(terminate_mon, 0);
    #ifdef SPINPOLL
    queue_work_on(CORE_MON, poll_cha_queue, &poll_cha);
    #else
    queue_delayed_work_on(CORE_MON, poll_cha_queue, &poll_cha, msecs_to_jiffies(SAMPLE_INTERVAL_MS));
    #endif

    WRITE_ONCE(colloid_nid_of_interest, LOCAL_NUMA);

    int i;
    for (i = 0; i < 5; i++) {
        msleep(1000);
        printk("%llu %llu\n", READ_ONCE(smoothed_occ_local), READ_ONCE(smoothed_occ_remote));
    }

    return 0;
}

static void colloidmon_exit(void)
{
    WRITE_ONCE(terminate_mon, 1);
    msleep(5000);
    flush_workqueue(poll_cha_queue);
    destroy_workqueue(poll_cha_queue);

    /* dump_log(); */

    pr_info("colloidmon exit");
}

module_init(colloidmon_init);
module_exit(colloidmon_exit);
MODULE_AUTHOR("Midhul");
MODULE_LICENSE("GPL");
