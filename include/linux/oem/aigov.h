#ifndef __INCLUDE_AIGOV__
#define __INCLUDE_AIGOV__

#ifndef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#endif

#define AIGOV_TAG "aigov: "
#define aigov_logv(fmt...) \
	do { \
		if (aigov_log_lv < 1) \
			pr_info(AIGOV_TAG fmt); \
	} while (0)

#define aigov_logi(fmt...) \
	do { \
		if (aigov_log_lv < 2) \
			pr_info(AIGOV_TAG fmt); \
	} while (0)

#define aigov_logw(fmt...) \
	do { \
		if (aigov_log_lv < 3) \
			pr_warn(AIGOV_TAG fmt); \
	} while (0)

#define aigov_loge(fmt...) pr_err(AIGOV_TAG fmt)
#define aigov_logd(fmt...) pr_debug(AIGOV_TAG fmt)

static inline bool aigov_enabled(void) { return false; }
static inline bool aigov_hooked(void) { return false; }
static inline bool aigov_use_util(void) { return false; }
static inline void aigov_reset(void) {}
static inline void aigov_update_util(int clus) {}
static inline void aigov_set_cpufreq(int cpu, int cpufreq) {}
static inline void aigov_set_ddrfreq(int ddrfreq) {}
static inline void aigov_inc_boost_hint(int cpu) {}
static inline void aigov_reset_boost_hint(int cpu) {}
static inline int aigov_get_cpufreq(int cpu) { return 0; }
static inline int aigov_get_ddrfreq(void) { return 0; }
static inline int aigov_get_boost_hint(int cpu) { return 0; }
static inline unsigned int aigov_get_weight(void) { return 0; }
static inline void aigov_dump(int cpu, unsigned long util, unsigned long aig_util, unsigned long extra_util) {}

#endif // __INCLUDE_AIGOV___
