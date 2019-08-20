/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include <tilck/kernel/kb.h>

#define DP_W   76
#define DP_H   23

void dp_show_opts(void);
void do_show_tasks(void);
void dp_show_irq_stats(void);
void dp_show_sys_mmap(void);
void dp_show_kmalloc_heaps(void);

typedef struct {

   const char *label;
   void (*draw_func)(void);
   keypress_func on_keypress_func;

} dp_context;

extern int dp_rows;
extern int dp_cols;
extern int dp_start_row;
extern int dp_start_col;

static inline sptr dp_int_abs(sptr val) {
   return val >= 0 ? val : -val;
}
