
#include <common/basic_defs.h>
#include <common/string_util.h>
#include <common/utils.h>

#include <multiboot.h>

#include <exos/hal.h>
#include <exos/irq.h>
#include <exos/kmalloc.h>
#include <exos/paging.h>
#include <exos/debug_utils.h>
#include <exos/process.h>
#include <exos/elf_loader.h>
#include <exos/tasklet.h>
#include <exos/sync.h>
#include <exos/fs/fat32.h>
#include <exos/fs/exvfs.h>
#include <exos/fs/devfs.h>
#include <exos/kb.h>
#include <exos/timer.h>
#include <exos/term.h>
#include <exos/pageframe_allocator.h>
#include <exos/datetime.h>
#include <exos/arch/generic_x86/textmode_video.h>
#include <exos/fb_console.h>

static bool multiboot;

extern u32 memsize_in_mb;
extern uptr ramdisk_paddr;
extern size_t ramdisk_size;

/* Variables used by the cmdline parsing code */

extern void (*self_test_to_run)(void);
extern const char *const cmd_args[16];
void parse_kernel_cmdline(const char *cmdline);

/* -- */


void read_multiboot_info(u32 magic, u32 mbi_addr)
{
   if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
      return;

   multiboot_info_t *mbi = (void *)(uptr)mbi_addr;
   memsize_in_mb = (mbi->mem_upper)/1024 + 1;

   multiboot = true;

   if (mbi->flags & MULTIBOOT_INFO_MODS) {
      if (mbi->mods_count >= 1) {

         multiboot_module_t *mod = ((multiboot_module_t *)(uptr)mbi->mods_addr);
         ramdisk_paddr = mod->mod_start;
         ramdisk_size = mod->mod_end - mod->mod_start;

      } else {
         ramdisk_paddr = 0;
         ramdisk_size = 0;
      }
   }

   if (mbi->flags & MULTIBOOT_INFO_CMDLINE) {
      parse_kernel_cmdline((const char *)(uptr)mbi->cmdline);
   }

   if (mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO) {
      if (mbi->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT) {
         set_framebuffer_info_from_mbi(mbi);
      }
   }
}



void show_hello_message(void)
{
   printk("Hello from exOS! [%s build, GCC %i.%i.%i]\n",
          BUILDTYPE_STR, __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);

   printk("TIMER_HZ: %i; TIME_SLOT: %i ms; MEM: %i MB\n",
          TIMER_HZ, 1000 / (TIMER_HZ / TIME_SLOT_JIFFIES), get_phys_mem_mb());

   datetime_t d;
   read_system_clock_datetime(&d);
   print_datetime(d);

   if (multiboot)
      printk("*** Detected multiboot ***\n");
}

void mount_ramdisk(void)
{
   if (!ramdisk_size) {
      printk("[WARNING] No RAMDISK found.\n");
      return;
   }

   filesystem *root_fs = fat_mount_ramdisk(KERNEL_PA_TO_VA(ramdisk_paddr));
   mountpoint_add(root_fs, "/");
   printk("Mounted RAMDISK at PADDR %p.\n", ramdisk_paddr);
}

void init_tty(void);

sptr sys_execve(const char *filename,
                const char *const *argv,
                const char *const *env);

void selftest_runner_thread()
{
   self_test_to_run();
}

void kmain(u32 multiboot_magic, u32 mbi_addr)
{
   printk("super early printk\n");
   read_multiboot_info(multiboot_magic, mbi_addr);

   setup_segmentation();
   init_pageframe_allocator(); /* NOTE: unused at the moment */

   init_paging();
   init_kmalloc();
   init_paging_cow();

   if (use_framebuffer())
      init_framebuffer_console();
   else
      init_textmode_console();

   show_hello_message();

   setup_soft_interrupt_handling();
   setup_irq_handling();
   init_sched();
   init_tasklets();

   post_sched_init_framebuffer_console();

   timer_set_freq(TIMER_HZ);
   irq_install_handler(X86_PC_TIMER_IRQ, timer_handler);
   irq_install_handler(X86_PC_KEYBOARD_IRQ, keyboard_handler);
   VERIFY(enqueue_tasklet0(&init_kb));

   setup_syscall_interfaces();
   mount_ramdisk();
   create_and_register_devfs();
   init_tty();

   if (self_test_to_run) {
      VERIFY(kthread_create(selftest_runner_thread, NULL) != NULL);
      switch_to_idle_task_outside_interrupt_context();
   }

   if (!ramdisk_size) {
      panic("No ramdisk and no selftest requested: nothing to do.");
   }

   enable_preemption();
   push_nested_interrupt(-1);
   printk("[main] sys_execve('%s')\n", cmd_args[0]);
   sptr rc = sys_execve(cmd_args[0], cmd_args, NULL);
   panic("execve('%s') failed with %i\n", cmd_args[0], rc);
}
