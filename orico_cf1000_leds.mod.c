#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xcb8b6ec6, "kfree" },
	{ 0x12ad300e, "iounmap" },
	{ 0xe8213e80, "_printk" },
	{ 0x97dd6ca9, "ioremap" },
	{ 0xd710adbf, "__kmalloc_large_noprof" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0x168f2bc7, "led_classdev_register_ext" },
	{ 0x23f25c0a, "__dynamic_pr_debug" },
	{ 0xd272d446, "__fentry__" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x2c3737c1, "led_classdev_unregister" },
	{ 0x984622ae, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xcb8b6ec6,
	0x12ad300e,
	0xe8213e80,
	0x97dd6ca9,
	0xd710adbf,
	0x90a48d82,
	0x168f2bc7,
	0x23f25c0a,
	0xd272d446,
	0xd272d446,
	0x2c3737c1,
	0x984622ae,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"kfree\0"
	"iounmap\0"
	"_printk\0"
	"ioremap\0"
	"__kmalloc_large_noprof\0"
	"__ubsan_handle_out_of_bounds\0"
	"led_classdev_register_ext\0"
	"__dynamic_pr_debug\0"
	"__fentry__\0"
	"__x86_return_thunk\0"
	"led_classdev_unregister\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "FC4B51C4245BDD97C717297");
