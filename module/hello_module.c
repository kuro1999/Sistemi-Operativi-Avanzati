#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

static int __init hello_module_init(void)
{
    pr_info("hello_module: modulo caricato correttamente\n");
    return 0;
}

static void __exit hello_module_exit(void)
{
    pr_info("hello_module: modulo rimosso correttamente\n");
}

module_init(hello_module_init);
module_exit(hello_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kuro1999");
MODULE_DESCRIPTION("Modulo minimale per verificare l'ambiente di sviluppo");
MODULE_VERSION("1.0");
