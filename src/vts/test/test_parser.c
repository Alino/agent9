/*
 * Unit tests for parser.c — Phase 2.2 subset
 * (printable + C0 control + ESC dispatches).
 */
#include "../compat.h"
#include "../cells.h"
#include "../parser.h"
#include <assert.h>
#include <stdio.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void); \
                   static void test_##name##_runner(void) { \
                       tests_run++; \
                       printf("  test_%s ... ", #name); \
                       fflush(stdout); \
                       test_##name(); \
                       printf("ok\n"); \
                   } \
                   static void test_##name(void)

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf("FAIL\n    %s == %s\n    got %lld, expected %lld\n    %s:%d\n", \
               #a, #b, _a, _b, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

static void
feed(Parser *p, const char *s)
{
    parser_feed_str(p, s);
}

TEST(simple_printable) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "hello");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'h');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'e');
    ASSERT_EQ(cellbuf_get(&b, 0, 4).rune, 'o');
    ASSERT_EQ(b.cur_col, 5);
    cellbuf_free(&b);
}

TEST(lf_resets_column) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "ab\ncd");
    /* Plan 9 convention: bare LF moves to col 0 of next row.
     * ESC D (IND) preserves column — that's tested separately. */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'a');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'b');
    ASSERT_EQ(cellbuf_get(&b, 1, 0).rune, 'c');
    ASSERT_EQ(cellbuf_get(&b, 1, 1).rune, 'd');
    cellbuf_free(&b);
}

TEST(crlf) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "ab\r\ncd");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'a');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'b');
    ASSERT_EQ(cellbuf_get(&b, 1, 0).rune, 'c');
    ASSERT_EQ(cellbuf_get(&b, 1, 1).rune, 'd');
    cellbuf_free(&b);
}

TEST(backspace) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "abc\b\bX");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'a');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'X');
    ASSERT_EQ(cellbuf_get(&b, 0, 2).rune, 'c');
    cellbuf_free(&b);
}

TEST(tab) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "ab\tX");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'a');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'b');
    /* Tab lands at col 8 */
    ASSERT_EQ(cellbuf_get(&b, 0, 8).rune, 'X');
    cellbuf_free(&b);
}

TEST(esc_ris_resets) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "garbage");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'g');
    feed(&p, "\033c"); /* RIS */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, ' ');
    ASSERT_EQ(b.cur_row, 0);
    ASSERT_EQ(b.cur_col, 0);
    cellbuf_free(&b);
}

TEST(esc_save_restore) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "hello\0337"); /* save */
    /* Move cursor */
    cellbuf_goto(&b, 5, 5);
    feed(&p, "\0338"); /* restore */
    ASSERT_EQ(b.cur_row, 0);
    ASSERT_EQ(b.cur_col, 5); /* col after "hello" */
    cellbuf_free(&b);
}

TEST(esc_index) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "x\033Dy");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'x');
    ASSERT_EQ(cellbuf_get(&b, 1, 1).rune, 'y'); /* IND moves down */
    cellbuf_free(&b);
}

TEST(esc_nel) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "x\033Ey");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'x');
    ASSERT_EQ(cellbuf_get(&b, 1, 0).rune, 'y'); /* NEL = CR + LF */
    cellbuf_free(&b);
}

TEST(stray_esc_doesnt_lock_up) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "ab\033Zcd"); /* ESC Z is invalid */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'a');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'b');
    /* After unknown ESC dispatch, parser returns to Ground.
     * 'c' lands next. */
    ASSERT_EQ(cellbuf_get(&b, 0, 2).rune, 'c');
    ASSERT_EQ(cellbuf_get(&b, 0, 3).rune, 'd');
    cellbuf_free(&b);
}

TEST(utf8_two_byte) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    /* 'á' = U+00E1 = 0xC3 0xA1 */
    const char s[] = {(char)0xC3, (char)0xA1, 'b', 0};
    feed(&p, s);
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 0x00E1);
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'b');
    cellbuf_free(&b);
}

TEST(utf8_three_byte) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    /* '€' = U+20AC = 0xE2 0x82 0xAC */
    const char s[] = {(char)0xE2, (char)0x82, (char)0xAC, 'X', 0};
    feed(&p, s);
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 0x20AC);
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'X');
    cellbuf_free(&b);
}

TEST(csi_state_swallows_params_no_dispatch_yet) {
    /* Phase 2.3 implements dispatch. CUP positions cursor; the 'Z' lands
     * at (5,3) → row 4 col 2 (1-indexed → 0-indexed). */
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "a\033[5;3HZ");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'a');
    ASSERT_EQ(cellbuf_get(&b, 4, 2).rune, 'Z');
    cellbuf_free(&b);
}

TEST(csi_cuu) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    cellbuf_goto(&b, 10, 5);
    feed(&p, "\033[3AX");
    /* Cursor up 3 from (10,5) → (7,5); 'X' lands, cursor advances to (7,6) */
    ASSERT_EQ(cellbuf_get(&b, 7, 5).rune, 'X');
    cellbuf_free(&b);
}

TEST(csi_cud) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    cellbuf_goto(&b, 0, 0);
    feed(&p, "\033[5BX");
    ASSERT_EQ(cellbuf_get(&b, 5, 0).rune, 'X');
    cellbuf_free(&b);
}

TEST(csi_cuf_cub) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "\033[10Cright");
    ASSERT_EQ(cellbuf_get(&b, 0, 10).rune, 'r');
    feed(&p, "\033[15D[here]");
    /* After "right" cursor is at col 15; CUB 15 → col 0; '[here]' at 0..5 */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, '[');
    cellbuf_free(&b);
}

TEST(csi_cup_no_params) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    cellbuf_goto(&b, 5, 5);
    feed(&p, "\033[HZ");
    /* CSI H with no params → home (0,0) */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'Z');
    cellbuf_free(&b);
}

TEST(csi_ed) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 4, 5);
    parser_init(&p, &b);
    feed(&p, "ABCD\r\nEFGH\r\nIJKL");
    /* Cursor at (2,4). Move to row 2 col 3 (1-indexed) → (1,2): */
    feed(&p, "\033[2;3H");
    feed(&p, "\033[J"); /* erase below */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'A');
    ASSERT_EQ(cellbuf_get(&b, 1, 0).rune, 'E');
    ASSERT_EQ(cellbuf_get(&b, 1, 1).rune, 'F');
    /* col 2 (cursor) and onwards is erased */
    ASSERT_EQ(cellbuf_get(&b, 1, 2).rune, ' ');
    ASSERT_EQ(cellbuf_get(&b, 2, 0).rune, ' ');
    cellbuf_free(&b);
}

TEST(csi_ed_all) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 3, 5);
    parser_init(&p, &b);
    feed(&p, "ABCD\r\nEFGH");
    feed(&p, "\033[2J");
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 5; c++)
            ASSERT_EQ(cellbuf_get(&b, r, c).rune, ' ');
    cellbuf_free(&b);
}

TEST(csi_el) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 2, 7);
    parser_init(&p, &b);
    feed(&p, "ABCDEF");
    feed(&p, "\033[1;4H"); /* row 1 col 4 (1-indexed) → row 0 col 3 */
    feed(&p, "\033[K");    /* erase right of cursor */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'A');
    ASSERT_EQ(cellbuf_get(&b, 0, 2).rune, 'C');
    ASSERT_EQ(cellbuf_get(&b, 0, 3).rune, ' ');
    ASSERT_EQ(cellbuf_get(&b, 0, 5).rune, ' ');
    cellbuf_free(&b);
}

TEST(sgr_basic_color) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "\033[31mR\033[0mN");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'R');
    ASSERT_EQ(cellbuf_get(&b, 0, 0).fg, 1);
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'N');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).fg, 7);
    cellbuf_free(&b);
}

TEST(sgr_bright_color) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "\033[92mG"); /* bright green */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).fg, 10);
    cellbuf_free(&b);
}

TEST(sgr_combined_attrs) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "\033[1;4;33mX"); /* bold + underline + yellow fg */
    Cell c = cellbuf_get(&b, 0, 0);
    ASSERT_EQ(c.fg, 3);
    ASSERT_EQ(c.attrs & ATTR_BOLD, ATTR_BOLD);
    ASSERT_EQ(c.attrs & ATTR_UNDERLINE, ATTR_UNDERLINE);
    cellbuf_free(&b);
}

TEST(sgr_bg) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "\033[44mB"); /* blue background */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).bg, 4);
    cellbuf_free(&b);
}

TEST(sgr_256color_quantized) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    /* 38;5;9 = bright red in xterm 256 color; truncated to 16 → 9 */
    feed(&p, "\033[38;5;9mX");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).fg, 9);
    cellbuf_free(&b);
}

TEST(decset_cursor_hide_show) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    ASSERT_EQ(b.cur_visible, 1);
    feed(&p, "\033[?25l"); /* hide */
    ASSERT_EQ(b.cur_visible, 0);
    feed(&p, "\033[?25h"); /* show */
    ASSERT_EQ(b.cur_visible, 1);
    cellbuf_free(&b);
}

TEST(alt_screen_save_clear_restore) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "hello");
    feed(&p, "\033[?1049h"); /* alt screen — saves cursor, clears */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, ' ');
    ASSERT_EQ(b.cur_row, 0);
    ASSERT_EQ(b.cur_col, 0);
    feed(&p, "ALT");
    feed(&p, "\033[?1049l"); /* exit alt — restores cursor (NOT content) */
    /* Cursor restored to row 0 col 5 (after "hello") */
    ASSERT_EQ(b.cur_col, 5);
    cellbuf_free(&b);
}

TEST(osc_swallowed) {
    Buffer b;
    Parser p;
    cellbuf_init(&b, 24, 80);
    parser_init(&p, &b);
    feed(&p, "a\033]0;mytitle\007b");
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'a');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'b');
    cellbuf_free(&b);
}

int main(void) {
    printf("=== test_parser (Phase 2.3 — full state machine + CSI) ===\n");
    test_simple_printable_runner();
    test_lf_resets_column_runner();
    test_crlf_runner();
    test_backspace_runner();
    test_tab_runner();
    test_esc_ris_resets_runner();
    test_esc_save_restore_runner();
    test_esc_index_runner();
    test_esc_nel_runner();
    test_stray_esc_doesnt_lock_up_runner();
    test_utf8_two_byte_runner();
    test_utf8_three_byte_runner();
    test_csi_state_swallows_params_no_dispatch_yet_runner();
    test_csi_cuu_runner();
    test_csi_cud_runner();
    test_csi_cuf_cub_runner();
    test_csi_cup_no_params_runner();
    test_csi_ed_runner();
    test_csi_ed_all_runner();
    test_csi_el_runner();
    test_sgr_basic_color_runner();
    test_sgr_bright_color_runner();
    test_sgr_combined_attrs_runner();
    test_sgr_bg_runner();
    test_sgr_256color_quantized_runner();
    test_decset_cursor_hide_show_runner();
    test_alt_screen_save_clear_restore_runner();
    test_osc_swallowed_runner();
    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
