#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

#ifdef CONFIG_UNWINDER_ORC
#include <asm/orc_header.h>
ORC_HEADER;
#endif

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
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

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x3af130aa, "cdev_add" },
	{ 0x40df682c, "class_create" },
	{ 0xe8962364, "device_create" },
	{ 0x728365e3, "cdev_del" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0x4b7b5a1a, "class_destroy" },
	{ 0x89940875, "mutex_lock_interruptible" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0x6b10bee1, "_copy_to_user" },
	{ 0xb39b608e, "device_destroy" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0x539565d0, "compat_ptr_ioctl" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0x122c3a7e, "_printk" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0xb2fd5ceb, "__put_user_4" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x73066ebc, "cdev_init" },
	{ 0xb08e71bf, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "FD59184068E878CB4C73091");
