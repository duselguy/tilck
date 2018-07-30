
#pragma once
#include <tilck/common/basic_defs.h>

#define TERM_ERASE_C   '\b'
#define TERM_ERASE_S   "\b"

#define TERM_WERASE_C  0x17    /* typical value for TERM=linux, Ctrl + W */
#define TERM_WERASE_S  "\x17"

#define TERM_KILL_C    0x15    /* typical value for TERM=linux, Ctrl + 7 */
#define TERM_KILL_S    "\x15"

typedef struct {

   /* Main functions */
   void (*set_char_at)(int row, int col, u16 entry);
   void (*set_row)(int row, u16 *data, bool flush); // NOTE: set_row() can
                                                    // safely assume that it has
                                                    // been called in a FPU
                                                    // context.
   void (*clear_row)(int row_num, u8 color);

   /* Cursor management */
   void (*move_cursor)(int row, int col, int color);
   void (*enable_cursor)(void);
   void (*disable_cursor)(void);

   /* Other (optional) */
   void (*scroll_one_line_up)(void);
   void (*flush_buffers)(void);

} video_interface;


void init_term(const video_interface *vi,
               int rows,
               int cols,
               bool use_serial_port);

bool term_is_initialized(void);

u32 term_get_tab_size(void);
u32 term_get_rows(void);
u32 term_get_cols(void);

u32 term_get_curr_row(void);
u32 term_get_curr_col(void);

void term_write(const char *buf, u32 len, u8 color);
void term_scroll_up(u32 lines);
void term_scroll_down(u32 lines);
void term_set_col_offset(u32 off);
void term_move_ch_and_cur(u32 row, u32 col);
void term_move_ch_and_cur_rel(s8 dx, s8 dy);

/* --- term write filter interface --- */

#define TERM_FILTER_WRITE_BLANK     0
#define TERM_FILTER_WRITE_C         1

typedef int (*term_filter_func)(char c, u8 *color /* in/out */, void *ctx);

void term_set_filter_func(term_filter_func func, void *ctx);
term_filter_func term_get_filter_func(void);

/* --- debug funcs --- */
void debug_term_print_scroll_cycles(void);
void debug_term_dump_font_table(void);

#define CHAR_BLOCK_LIGHT  0xb0  //  #
#define CHAR_BLOCK_MID    0xb1  //  #
#define CHAR_BLOCK_HEAVY  0xdb  //  #
#define CHAR_VLINE        0xb3  //   |
#define CHAR_VLINE_LEFT   0xb4  //  -|
#define CHAR_VLINE_RIGHT  0xc3  //  |-
#define CHAR_CORNER_LL    0xc0  //  |_
#define CHAR_CORNER_LR    0xd9  //  _|
#define CHAR_CORNER_UL    0xda  //
#define CHAR_CORNER_UR    0xbf  //
#define CHAR_BOTTOM_C     0xc1  //  _|_
#define CHAR_TOP_C        0xc2  //   T
#define CHAR_HLINE        0xc4  //  --
#define CHAR_CROSS        0xc5  //  +
