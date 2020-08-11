/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck_gen_headers/config_console.h>

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>
#include <tilck/common/printk.h>
#include <tilck/common/color_defs.h>
#include <tilck/common/atomics.h>

#include <tilck/kernel/sched.h>
#include <tilck/kernel/interrupts.h>
#include <tilck/kernel/term.h>
#include <tilck/kernel/tty.h>
#include <tilck/kernel/datetime.h>

#define PRINTK_COLOR                          COLOR_GREEN
#define PRINTK_RINGBUF_FLUSH_COLOR            COLOR_CYAN
#define PRINTK_NOSPACE_IN_RBUF_FLUSH_COLOR    COLOR_MAGENTA
#define PRINTK_PANIC_COLOR                    COLOR_GREEN

struct ringbuf_stat {

   union {

      struct {
         u32 used : 10;
         u32 read_pos : 10;
         u32 write_pos : 10;
         u32 first_printk_on_stack : 1;
         u32 unused : 1;
      };

      ATOMIC(u32) raw;
      u32 __raw;
   };
};

static char printk_rbuf[1024];
static volatile struct ringbuf_stat printk_rbuf_stat;
bool __in_printk;

/*
 * NOTE: the ring buf cannot be larger than 1024 elems because the fields
 * 'used', 'read_pos' and 'write_pos' are 10 bits long and we CANNOT extend
 * them in 32 bits. Such approach is convenient because with everything packed
 * in 32 bits, we can do atomic operations.
 */
STATIC_ASSERT(sizeof(printk_rbuf) <= 1024);

static void printk_direct_flush_no_tty(const char *buf, size_t size, u8 color)
{
   /*
    * tty has not been initialized yet, therefore we have to translate here
    * \n to \r\n, by writing character by character to term.
    */

   for (u32 i = 0; i < size; i++) {

      __in_printk = true;
      {
         if (buf[i] == '\n')
            term_write("\r", 1, color);

         term_write(&buf[i], 1, color);
      }
      __in_printk = false;
   }
}

static void printk_direct_flush(const char *buf, size_t size, u8 color)
{
   if (UNLIKELY(get_curr_tty() == NULL))
      return printk_direct_flush_no_tty(buf, size, color);

   /* tty has been initialized and set a term write filter func */
   __in_printk = true;
   {
      if (KRN_PRINTK_ON_CURR_TTY || !get_curr_process_tty())
         term_write(buf, size, color);
      else
         tty_curr_proc_write(buf, size);
   }
   __in_printk = false;
   return;
}

static void
__printk_flush_ringbuf(char *tmpbuf, u32 buf_size)
{
   struct ringbuf_stat cs, ns;
   u32 to_read = 0;

   while (true) {

      do {
         cs = printk_rbuf_stat;
         ns = printk_rbuf_stat;

         /* We at most 'buf_size' bytes at a time */
         to_read = UNSAFE_MIN(buf_size, ns.used);

         /* And copy them to our minibuf */
         for (u32 i = 0; i < to_read; i++)
            tmpbuf[i] = printk_rbuf[(cs.read_pos + i) % sizeof(printk_rbuf)];

         /* Increase read_pos and decrease used */
         ns.read_pos = (ns.read_pos + to_read) % sizeof(printk_rbuf);
         ns.used -= to_read;

         if (!to_read)
            ns.first_printk_on_stack = 0;

         /* Repeat that until we were able to do that atomically */

      } while (!atomic_cas_weak(&printk_rbuf_stat.raw,
                                &cs.__raw,
                                ns.__raw,
                                mo_relaxed,
                                mo_relaxed));

      /* Note: we checked that `first_printk_on_stack` in `cs` was unset! */
      if (!to_read)
         break;

      printk_direct_flush(tmpbuf, to_read, PRINTK_RINGBUF_FLUSH_COLOR);
   }
}

void
printk_flush_ringbuf(void)
{
   char minibuf[80];
   __printk_flush_ringbuf(minibuf, sizeof(minibuf));
}

static void printk_append_to_ringbuf(const char *buf, size_t size)
{
   static const char err_msg[] = "{_DROPPED_}\n";

   struct ringbuf_stat cs, ns;

   do {
      cs = printk_rbuf_stat;
      ns = printk_rbuf_stat;

      if (cs.used + size >= sizeof(printk_rbuf)) {

         if (term_is_initialized()) {
            printk_direct_flush(buf, size, PRINTK_NOSPACE_IN_RBUF_FLUSH_COLOR);
            return;
         }

         if (buf != err_msg && cs.used < sizeof(printk_rbuf) - 1) {
            size = MIN(sizeof(printk_rbuf) - cs.used - 1, sizeof(err_msg));
            printk_append_to_ringbuf(err_msg, size);
         }

         return;
      }

      ns.used += size;
      ns.write_pos = (ns.write_pos + size) % sizeof(printk_rbuf);

   } while (!atomic_cas_weak(&printk_rbuf_stat.raw,
                             &cs.__raw,
                             ns.__raw,
                             mo_relaxed,
                             mo_relaxed));

   // Now we have some allocated space in the ringbuf

   for (u32 i = 0; i < size; i++)
      printk_rbuf[(cs.write_pos + i) % sizeof(printk_rbuf)] = buf[i];
}

static bool
try_set_first_printk_on_stack(void)
{
   struct ringbuf_stat cs, ns;

   do {
      cs = printk_rbuf_stat;
      ns = printk_rbuf_stat;
      ns.first_printk_on_stack = 1;
   } while (!atomic_cas_weak(&printk_rbuf_stat.raw,
                              &cs.__raw,
                              ns.__raw,
                              mo_relaxed,
                              mo_relaxed));

   /*
    * If, when we swapped atomically the state, cs.first_printk_on_stack was 0
    * we were the first to set it to 1.
    */
   return !cs.first_printk_on_stack;
}

void tilck_vprintk(u32 flags, const char *fmt, va_list args)
{
   static const char truncated_str[] = "[...]";

   char buf[256];
   int written = 0;
   bool prefix = in_panic() ? false : true;

   if (*fmt == PRINTK_CTRL_CHAR) {
      u32 cmd = *(u32 *)fmt;
      fmt += 4;

      if (cmd == *(u32 *)NO_PREFIX)
         prefix = false;
   }

   if (flags & PRINTK_FL_NO_PREFIX)
      prefix = false;

   if (prefix) {

      const u64 systime = get_sys_time();

      written = snprintk(
         buf, sizeof(buf), "[%5u.%03u] ",
         (u32)(systime / TS_SCALE),
         (u32)((systime % TS_SCALE) / (TS_SCALE / 1000))
      );
   }

   written += vsnprintk(buf + written, sizeof(buf) - (u32)written, fmt, args);

   if (written == sizeof(buf)) {

      /*
       * Corner case: the buffer is completely full and the final \0 has been
       * included in 'written'.
       */

      memcpy(buf + sizeof(buf) - sizeof(truncated_str),
             truncated_str,
             sizeof(truncated_str));

      written--;
   }

   if (!term_is_initialized()) {
      printk_append_to_ringbuf(buf, (size_t) written);
      return;
   }

   if (in_panic()) {
      printk_direct_flush(buf, (size_t) written, PRINTK_PANIC_COLOR);
      return;
   }

   disable_preemption();
   {
      if (try_set_first_printk_on_stack()) {

         /*
          * OK, we were the first. Now, flush our buffer directly and loop
          * flushing anything that, in the meanwhile, printk() calls from IRQs
          * generated.
          */
         printk_direct_flush(buf, (size_t) written, PRINTK_COLOR);
         __printk_flush_ringbuf(buf, sizeof(buf));

      } else {

         /*
          * We were NOT the first printk on the stack: we're in an IRQ handler
          * and can only append our data to the ringbuf. On return, at some
          * point, the first printk(), in printk_flush_ringbuf() [case above]
          * will flush our data.
          */
         printk_append_to_ringbuf(buf, (size_t) written);
      }
   }
   enable_preemption();
}

void printk(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   vprintk(fmt, args);
   va_end(args);
}
