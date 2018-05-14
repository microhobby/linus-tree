
#include <linux/init.h>
#include <linux/module.h>

static int my_init(void)
{
	printk( KERN_NOTICE "Simple-driver: init is called");

	panic("Ooooohhhh Manhêeeee, cadê a toalha?!?!?");

	return  0;
}

static void my_exit(void)
{
	printk( KERN_NOTICE "Simple-driver: exit is called" );
	return;
}

module_init(my_init);
module_exit(my_exit);
