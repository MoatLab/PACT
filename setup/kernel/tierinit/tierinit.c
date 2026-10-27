
// tierinit - Initialize NUMA tiering without kernel patches
//
// Uses kprobes to resolve internal kernel symbols at load time.
// Works on vanilla Linux 6.3 with CONFIG_KPROBES=y and CONFIG_KALLSYMS_ALL=y
// (the configuration in setup/kernel/ has both).

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/kprobes.h>
#include <linux/memory-tiers.h>
#include <linux/device.h>
#include <linux/rcupdate.h>
#include <linux/list.h>
#include <linux/nodemask.h>

#define FARMEM_ADISTANCE	(MEMTIER_ADISTANCE_DRAM * 10)
#define FARMEM_NUMA		1
#define LOCAL_NUMA		0

static struct memory_dev_type *farmem_type;

/*
 * Private layouts copied verbatim from mm/memory-tiers.c for this kernel.
 * clear_node_memory_tier() and its helpers (__node_get_memory_tier,
 * destroy_memory_tier) are static and get FULLY inlined by the compiler in a
 * vanilla 6.3 build -- they leave no kallsyms entry (not even a .part.0
 * clone), so they cannot be resolved at runtime.  We therefore reimplement
 * clear_node_memory_tier() in-module.  This needs the private node_memory_types
 * array and the struct memory_tier layout, both reproduced below.
 *
 * struct memory_dev_type is already public in <linux/memory-tiers.h>.
 */
struct node_memory_type_map {
	struct memory_dev_type *memtype;
	int map_count;
};

struct memory_tier {
	struct list_head list;
	struct list_head memory_types;
	int adistance_start;
	struct device dev;
	nodemask_t lower_tier_mask;
};

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
typedef struct memory_tier *(*set_node_memory_tier_t)(int node);
typedef void (*establish_demotion_targets_t)(void);
typedef bool (*node_is_toptier_t)(int node);
typedef int (*next_demotion_node_t)(int node);

static struct mutex *k_memory_tier_lock;
static struct memory_dev_type **k_default_dram_type;
static int *k_top_tier_adistance;
static struct node_memory_type_map *k_node_memory_types;
static set_node_memory_tier_t k_set_node_memory_tier;
static establish_demotion_targets_t k_establish_demotion_targets;
static node_is_toptier_t k_node_is_toptier;
static next_demotion_node_t k_next_demotion_node;
static void **k_node_demotion;

/*
 * establish_demotion_targets() is static and may be partial-inlined by the
 * compiler, in which case only a mangled clone (e.g. .part.0) exists in
 * kallsyms. Try the base name first, then known mangled suffixes.
 */
static const char * const establish_candidates[] = {
	"establish_demotion_targets",
	"establish_demotion_targets.part.0",
	"establish_demotion_targets.constprop.0",
	"establish_demotion_targets.isra.0",
};

static unsigned long kprobe_addr(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long addr;

	if (register_kprobe(&kp) < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

static int resolve_symbols(void)
{
	kallsyms_lookup_name_t lookup;
	int i;

	lookup = (kallsyms_lookup_name_t)kprobe_addr("kallsyms_lookup_name");
	if (!lookup) {
		pr_err("tierinit: cannot resolve kallsyms_lookup_name\n");
		return -ENOENT;
	}

	k_memory_tier_lock       = (void *)lookup("memory_tier_lock");
	k_default_dram_type      = (void *)lookup("default_dram_type");
	k_top_tier_adistance     = (void *)lookup("top_tier_adistance");
	k_node_memory_types      = (void *)lookup("node_memory_types");
	k_set_node_memory_tier   = (void *)lookup("set_node_memory_tier");
	k_node_is_toptier        = (void *)lookup("node_is_toptier");
	k_next_demotion_node     = (void *)lookup("next_demotion_node");
	k_node_demotion          = (void *)lookup("node_demotion");

	for (i = 0; i < ARRAY_SIZE(establish_candidates); i++) {
		k_establish_demotion_targets =
			(void *)lookup(establish_candidates[i]);
		if (k_establish_demotion_targets) {
			pr_info("tierinit: resolved %s\n",
				establish_candidates[i]);
			break;
		}
	}

	if (!k_memory_tier_lock || !k_default_dram_type ||
	    !k_top_tier_adistance || !k_node_memory_types ||
	    !k_set_node_memory_tier || !k_establish_demotion_targets ||
	    !k_node_is_toptier || !k_next_demotion_node || !k_node_demotion) {
		pr_err("tierinit: failed to resolve one or more symbols\n");
		pr_err("  memory_tier_lock=%px set_node_memory_tier=%px\n",
		       k_memory_tier_lock, k_set_node_memory_tier);
		pr_err("  node_memory_types=%px establish_demotion_targets=%px\n",
		       k_node_memory_types, k_establish_demotion_targets);
		pr_err("  default_dram_type=%px top_tier_adistance=%px\n",
		       k_default_dram_type, k_top_tier_adistance);
		pr_err("  node_is_toptier=%px next_demotion_node=%px\n",
		       k_node_is_toptier, k_next_demotion_node);
		pr_err("  node_demotion=%px establish_demotion_targets=%px\n",
		       k_node_demotion, k_establish_demotion_targets);
		return -ENOENT;
	}

	pr_info("tierinit: all symbols resolved\n");
	return 0;
}

/*
 * Replicate the guard the compiler inlined into the original callers of
 * establish_demotion_targets():
 *   if (!node_demotion || !IS_ENABLED(CONFIG_MIGRATION)) return;
 * Required when we resolved the partial-inlined .part.0 clone, whose body
 * starts after that guard. Harmless if we resolved the un-inlined symbol.
 */
static void call_establish_demotion_targets(void)
{
	if (*k_node_demotion)
		k_establish_demotion_targets();
}

static void do_init_memory_tier(int node)
{
	struct memory_tier *memtier;

	mutex_lock(k_memory_tier_lock);
	memtier = k_set_node_memory_tier(node);
	if (!IS_ERR(memtier))
		call_establish_demotion_targets();
	mutex_unlock(k_memory_tier_lock);
}

/*
 * In-module reimplementation of mm/memory-tiers.c:destroy_memory_tier().
 * Must be called with memory_tier_lock held.
 */
static void destroy_memory_tier_local(struct memory_tier *memtier)
{
	list_del(&memtier->list);
	device_unregister(&memtier->dev);
}

/*
 * In-module reimplementation of mm/memory-tiers.c:clear_node_memory_tier(),
 * which is fully inlined (no kallsyms symbol) in a vanilla 6.3 build.
 * Mirrors the original exactly; must be called with memory_tier_lock held.
 */
static bool clear_node_memory_tier_local(int node)
{
	bool cleared = false;
	pg_data_t *pgdat;
	struct memory_tier *memtier;

	pgdat = NODE_DATA(node);
	if (!pgdat)
		return false;

	memtier = rcu_dereference_protected(pgdat->memtier,
					    lockdep_is_held(k_memory_tier_lock));
	if (memtier) {
		struct memory_dev_type *memtype;

		rcu_assign_pointer(pgdat->memtier, NULL);
		synchronize_rcu();
		memtype = k_node_memory_types[node].memtype;
		node_clear(node, memtype->nodes);
		if (nodes_empty(memtype->nodes)) {
			list_del_init(&memtype->tier_sibiling);
			if (list_empty(&memtier->memory_types))
				destroy_memory_tier_local(memtier);
		}
		cleared = true;
	}
	return cleared;
}

static void do_clear_memory_tier(int node)
{
	mutex_lock(k_memory_tier_lock);
	clear_node_memory_tier_local(node);
	call_establish_demotion_targets();
	mutex_unlock(k_memory_tier_lock);
}

static void fix_top_tier_adistance(void)
{
	int fixed = round_down((*k_default_dram_type)->adistance,
			       MEMTIER_CHUNK_SIZE) + MEMTIER_CHUNK_SIZE - 1;
	WRITE_ONCE(*k_top_tier_adistance, fixed);
	pr_info("tierinit: top_tier_adistance forced to %d\n", fixed);
}

static int __init tierinit_init(void)
{
	int ret;

	ret = resolve_symbols();
	if (ret)
		return ret;

	pr_info("tierinit: demotion[%d]=%d (before)\n",
		LOCAL_NUMA, k_next_demotion_node(LOCAL_NUMA));

	farmem_type = alloc_memory_type(FARMEM_ADISTANCE);
	if (IS_ERR(farmem_type)) {
		pr_err("tierinit: failed to allocate memory type\n");
		return PTR_ERR(farmem_type);
	}

	do_clear_memory_tier(FARMEM_NUMA);
	clear_node_memory_type(FARMEM_NUMA, *k_default_dram_type);
	init_node_memory_type(FARMEM_NUMA, farmem_type);
	do_init_memory_tier(FARMEM_NUMA);
	fix_top_tier_adistance();

	pr_info("tierinit: demotion[%d]=%d (after)\n",
		LOCAL_NUMA, k_next_demotion_node(LOCAL_NUMA));

	if (k_node_is_toptier(FARMEM_NUMA))
		pr_warn("tierinit: node %d is still top tier\n", FARMEM_NUMA);
	else
		pr_info("tierinit: node %d placed in lower tier\n", FARMEM_NUMA);

	return 0;
}

static void __exit tierinit_exit(void)
{
	do_clear_memory_tier(FARMEM_NUMA);
	clear_node_memory_type(FARMEM_NUMA, farmem_type);
	do_init_memory_tier(FARMEM_NUMA);

	if (k_node_is_toptier(FARMEM_NUMA))
		pr_info("tierinit: node %d restored to top tier\n", FARMEM_NUMA);

	destroy_memory_type(farmem_type);
	pr_info("tierinit: unloaded\n");
}

module_init(tierinit_init);
module_exit(tierinit_exit);
MODULE_AUTHOR("PACT");
MODULE_DESCRIPTION("Initialize NUMA tiering");
MODULE_LICENSE("GPL");
