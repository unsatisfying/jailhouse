/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2013-2017
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/processor.h>
#include <jailhouse/printk.h>
#include <jailhouse/entry.h>
#include <jailhouse/gcov.h>
#include <jailhouse/mmio.h>
#include <jailhouse/paging.h>
#include <jailhouse/control.h>
#include <jailhouse/string.h>
#include <jailhouse/unit.h>
#include <generated/version.h>
#include <asm/spinlock.h>

extern u8 __text_start[];

static const __attribute__((aligned(PAGE_SIZE))) u8 empty_page[PAGE_SIZE];

static spinlock_t init_lock;
static unsigned int master_cpu_id = INVALID_CPU_ID;
static volatile unsigned int entered_cpus, initialized_cpus;
static volatile int error;

static void init_early(unsigned int cpu_id)
{
	unsigned long core_and_percpu_size = hypervisor_header.core_size +
		sizeof(struct per_cpu) * hypervisor_header.max_cpus;
	u64 hyp_phys_start, hyp_phys_end;
	struct jailhouse_memory hv_page;

	master_cpu_id = cpu_id;

	system_config = (struct jailhouse_system *)
		(JAILHOUSE_BASE + core_and_percpu_size);

	virtual_console = SYS_FLAGS_VIRTUAL_DEBUG_CONSOLE(system_config->flags);

	arch_dbg_write_init();

	printk("\nInitializing Jailhouse hypervisor %s on CPU %d\n",
	       JAILHOUSE_VERSION, cpu_id);
	printk("Code location: %p\n", __text_start);

	gcov_init();

	error = paging_init();
	if (error)
		return;

	root_cell.config = &system_config->root_cell;

	error = cell_init(&root_cell);
	if (error)
		return;

	error = arch_init_early();
	if (error)
		return;

	/*
	 * Back the region of the hypervisor core and per-CPU page with empty
	 * pages for Linux. This allows to fault-in the hypervisor region into
	 * Linux' page table before shutdown without triggering violations.
	 *
	 * Allow read access to the console page, if the hypervisor has the
	 * debug console flag JAILHOUSE_SYS_VIRTUAL_DEBUG_CONSOLE set.
	 */
	hyp_phys_start = system_config->hypervisor_memory.phys_start;
	hyp_phys_end = hyp_phys_start + system_config->hypervisor_memory.size;

	hv_page.virt_start = hyp_phys_start;
	hv_page.size = PAGE_SIZE;
	hv_page.flags = JAILHOUSE_MEM_READ;
	while (hv_page.virt_start < hyp_phys_end) {
		if (virtual_console &&
		    hv_page.virt_start == paging_hvirt2phys(&console))
			hv_page.phys_start = paging_hvirt2phys(&console);
		else
			hv_page.phys_start = paging_hvirt2phys(empty_page);
		error = arch_map_memory_region(&root_cell, &hv_page);
		if (error)
			return;
		hv_page.virt_start += PAGE_SIZE;
	}

	paging_dump_stats("after early setup");
	printk("Initializing processors:\n");
}

static void cpu_init(struct per_cpu *cpu_data)
{
	int err = -EINVAL;

	printk(" CPU %d... ", cpu_data->public.cpu_id);

	if (!cpu_id_valid(cpu_data->public.cpu_id))
		goto failed;

	cpu_data->public.cell = &root_cell;

	/* set up per-CPU page table */
	cpu_data->pg_structs.hv_paging = true;
	cpu_data->pg_structs.root_paging = hv_paging_structs.root_paging;
	cpu_data->pg_structs.root_table =
		(page_table_t)cpu_data->public.root_table_page;

	err = paging_create_hvpt_link(&cpu_data->pg_structs, JAILHOUSE_BASE);
	if (err)
		goto failed;

#ifdef CONFIG_PAGE_TABLE_PROTECTION
	err = paging_create_hvpt_link(&cpu_data->pg_structs, PGP_RO_BUF_VIRT);
	if (err) {
		printk("error in mapping pgp ro buf hvpt link");
		goto failed;
	}
	else
	{
		printk("sucess in mapping pgp ro buf hvpt link");
	}
#endif

	if (CON_IS_MMIO(system_config->debug_console.flags)) {
		err = paging_create_hvpt_link(&cpu_data->pg_structs,
			(unsigned long)hypervisor_header.debug_console_base);
		if (err)
			goto failed;
	}

	/* set up private mapping of per-CPU data structure */
	err = paging_create(&cpu_data->pg_structs, paging_hvirt2phys(cpu_data),
			    sizeof(*cpu_data), LOCAL_CPU_BASE,
			    PAGE_DEFAULT_FLAGS,
			    PAGING_NON_COHERENT | PAGING_HUGE);
	if (err)
		goto failed;

	err = arch_cpu_init(cpu_data);
	if (err)
		goto failed;

	/* Make sure any remappings to the temporary regions can be performed
	 * without allocations of page table pages. */
	err = paging_create(&cpu_data->pg_structs, 0,
			    NUM_TEMPORARY_PAGES * PAGE_SIZE,
			    TEMPORARY_MAPPING_BASE, PAGE_NONPRESENT_FLAGS,
			    PAGING_NON_COHERENT | PAGING_NO_HUGE);
	if (err)
		goto failed;

	printk("OK\n");

	/*
	 * If this CPU is last, make sure everything was committed before we
	 * signal the other CPUs spinning on initialized_cpus that they can
	 * continue.
	 */
	memory_barrier();
	initialized_cpus++;
	return;

failed:
	printk("FAILED\n");
	error = err;
}

static void init_late(void)
{
	unsigned int n, cpu, expected_cpus = 0;
	const struct jailhouse_memory *mem;
	struct unit *unit;

	for_each_cpu(cpu, root_cell.cpu_set)
		expected_cpus++;
	if (hypervisor_header.online_cpus != expected_cpus) {
		error = trace_error(-EINVAL);
		return;
	}

	for_each_unit(unit) {
		printk("Initializing unit: %s\n", unit->name);
		error = unit->init();
		if (error)
			return;
	}

	for_each_mem_region(mem, root_cell.config, n) {
		if (JAILHOUSE_MEMORY_IS_SUBPAGE(mem))
			error = mmio_subpage_register(&root_cell, mem);
		else
			error = arch_map_memory_region(&root_cell, mem);
		if (error)
			return;
	}
	
#ifdef CONFIG_PAGE_TABLE_PROTECTION
    // paging_set_flag(arch_get_pg_struct(&(root_cell.arch)), PGP_RO_BUF_BASE, PGP_ROBUF_SIZE,
    //         PAGING_NON_COHERENT | PAGING_HUGE, GPHYS2PHYS_WRITE_MASK, GPHYS2PHYS_WRITE_PROTECTION_VALUE);
	paging_set_flag(arch_get_pg_struct(&(root_cell.arch)), PGP_RO_BUF_BASE, PGP_ROBUF_SIZE,
            PAGING_NON_COHERENT | PAGING_HUGE, GPHYS2PHYS_WRITE_MASK, GPHYS2PHYS_WRITE_PROTECTION_VALUE);
#endif
	config_commit(&root_cell);

	paging_dump_stats("after late setup");
}

/*
 * This is the architecture independent C entry point, which is called by
 * arch_entry. This routine is called on each CPU when initializing Jailhouse.
 */
int entry(unsigned int cpu_id, struct per_cpu *cpu_data)
{
	printk("[PGP]: %d cpu get in entry...\n",cpu_id);
	static volatile bool activate;
	bool master = false;

	cpu_data->public.cpu_id = cpu_id;

	spin_lock(&init_lock);

	/*
	 * If this CPU is last, make sure everything was committed before we
	 * signal the other CPUs spinning on entered_cpus that they can
	 * continue.
	 */
	memory_barrier();
	entered_cpus++;

	spin_unlock(&init_lock);

	while (entered_cpus < hypervisor_header.online_cpus)
		cpu_relax();

	spin_lock(&init_lock);

	if (master_cpu_id == INVALID_CPU_ID) {
		/* Only the master CPU, the first to enter this
		 * function, performs system-wide initializations. */
		master = true;
		init_early(cpu_id);
	}

	if (!error)
		cpu_init(cpu_data);

	spin_unlock(&init_lock);

	while (!error && initialized_cpus < hypervisor_header.online_cpus)
		cpu_relax();

	if (!error && master) {
		init_late();
		if (!error) {
			/*
			 * Make sure everything was committed before we signal
			 * the other CPUs that they can continue.
			 */
			memory_barrier();
			activate = true;
		}
	} else {
		while (!error && !activate)
			cpu_relax();
	}

	if (error) {
		if (master)
			shutdown();
		arch_cpu_restore(cpu_id, error);
		return error;
	}

	if (master)
		printk("Activating hypervisor\n");

	/* point of no return */
	arch_cpu_activate_vmm();
}

/** Hypervisor description header. */
struct jailhouse_header __attribute__((section(".header")))
hypervisor_header = {
	.signature = JAILHOUSE_SIGNATURE,
	.core_size = (unsigned long)__page_pool - JAILHOUSE_BASE,
	.percpu_size = sizeof(struct per_cpu),
	.entry = arch_entry - JAILHOUSE_BASE,
	.console_page = (unsigned long)&console - JAILHOUSE_BASE,
};
