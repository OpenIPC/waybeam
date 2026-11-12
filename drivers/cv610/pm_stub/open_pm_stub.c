// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Hi3516CV610 B051 MPP registration-only PM shim.
 *
 * cmpi_init_modules() in open_base requires module ID 58 to be registered,
 * even when the application does not use power management. The complete
 * vendor open_pm module performs SoC-specific SVB/thermal setup before it
 * registers that ID and hangs on the current OpenIPC CV610 kernel/device tree.
 */

#include <linux/module.h>
#include <linux/build_bug.h>
#include <linux/types.h>

#define OT_ID_PM            58
#define MPP_VERSION_MAGIC   20250329U
#define MPP_MOD_NAME_LEN    16

enum mpp_mod_notice_id {
	MPP_MOD_NOTICE_STOP = 0x11,
};

enum mpp_mod_state {
	MPP_MOD_STATE_FREE = 0x11,
	MPP_MOD_STATE_BUSY = 0x22,
};

/* Binary-compatible with B051's umap_module on 32-bit ARM. */
struct mpp_module {
	struct list_head list;
	char mod_name[MPP_MOD_NAME_LEN];
	int mod_id;
	int (*init)(void *arg);
	void (*exit)(void);
	void (*query_state)(enum mpp_mod_state *state);
	void (*notify)(enum mpp_mod_notice_id notice);
	u32 (*version_checker)(void);
	int inited;
	void *export_funcs;
	void *data;
	char *version;
};

extern int cmpi_register_module(struct mpp_module *module);
extern void cmpi_unregister_module(int mod_id);

static int pm_noop_init(void *arg)
{
	(void)arg;
	return 0;
}

static void pm_noop_exit(void)
{
}

static u32 pm_version_magic(void)
{
	return MPP_VERSION_MAGIC;
}

static struct mpp_module pm_stub_module = {
	.mod_name = "pm",
	.mod_id = OT_ID_PM,
	.init = pm_noop_init,
	.exit = pm_noop_exit,
	.version_checker = pm_version_magic,
};

static int __init cv610_pm_stub_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct mpp_module) != 64);
	BUILD_BUG_ON(offsetof(struct mpp_module, init) != 0x1c);
	BUILD_BUG_ON(offsetof(struct mpp_module, version_checker) != 0x2c);

	ret = cmpi_register_module(&pm_stub_module);
	if (ret)
		pr_err("cv610_pm_stub: PM module registration failed: %d\n", ret);
	else
		pr_info("cv610_pm_stub: registered MPP PM module ID %d (no hardware control)\n",
			OT_ID_PM);
	return ret;
}

static void __exit cv610_pm_stub_exit(void)
{
	cmpi_unregister_module(OT_ID_PM);
	pr_info("cv610_pm_stub: unregistered MPP PM module ID %d\n", OT_ID_PM);
}

module_init(cv610_pm_stub_init);
module_exit(cv610_pm_stub_exit);

MODULE_DESCRIPTION("Hi3516CV610 B051 registration-only MPP PM shim");
MODULE_AUTHOR("OpenIPC contributors");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_SOFTDEP("pre: open_base");
