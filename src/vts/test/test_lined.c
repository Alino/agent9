/*
 * Unit tests for line editor.
 */
#include "../compat.h"
#include "../lined.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

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

#define ASSERT_STR(a, b) do { \
    const char *_a = (const char*)(a), *_b = (const char*)(b); \
    if (strcmp(_a, _b) != 0) { \
        printf("FAIL\n    %s == %s\n    got %s, expected %s\n    %s:%d\n", \
               #a, #b, _a, _b, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* Helper to feed a string byte-by-byte. */
static void
feed_str(LineEditor *e, const char *s)
{
    uchar redraw[256];
    int rl;
    while (*s) {
        lined_feed_byte(e, *s++, NULL, NULL, redraw, sizeof(redraw), &rl);
    }
}

TEST(simple_typing) {
    LineEditor e;
    lined_init(&e);
    feed_str(&e, "hello");
    ASSERT_EQ(e.len, 5);
    ASSERT_EQ(e.cursor, 5);
    char tmp[16];
    memmove(tmp, e.buf, e.len);
    tmp[e.len] = 0;
    ASSERT_STR(tmp, "hello");
}

TEST(backspace) {
    LineEditor e;
    lined_init(&e);
    feed_str(&e, "hello");
    feed_str(&e, "\x08\x08"); /* two backspaces */
    ASSERT_EQ(e.len, 3);
    char tmp[16];
    memmove(tmp, e.buf, e.len);
    tmp[e.len] = 0;
    ASSERT_STR(tmp, "hel");
}

TEST(enter_completes_line) {
    LineEditor e;
    uchar *out;
    int out_len, rl;
    lined_init(&e);
    feed_str(&e, "ls -la");
    int r = lined_feed_byte(&e, '\n', &out, &out_len, NULL, 0, &rl);
    ASSERT_EQ(r, LINEED_COMPLETE);
    ASSERT_EQ(out_len, 6);
    /* Buffer should reset */
    ASSERT_EQ(e.len, 0);
    ASSERT_EQ(e.cursor, 0);
}

TEST(history_push_and_recall) {
    LineEditor e;
    lined_init(&e);
    lined_push_history(&e, "first");
    lined_push_history(&e, "second");
    lined_push_history(&e, "third");
    ASSERT_EQ(e.hist_count, 3);
    /* Press Up arrow: ESC [ A */
    uchar redraw[256];
    int rl;
    lined_feed_byte(&e, 0x1B, NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, '[', NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, 'A', NULL, NULL, redraw, sizeof(redraw), &rl);
    /* Should load "third" */
    ASSERT_EQ(e.len, 5);
    char tmp[16];
    memmove(tmp, e.buf, e.len);
    tmp[e.len] = 0;
    ASSERT_STR(tmp, "third");
    /* Up again: load "second" */
    lined_feed_byte(&e, 0x1B, NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, '[', NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, 'A', NULL, NULL, redraw, sizeof(redraw), &rl);
    memmove(tmp, e.buf, e.len);
    tmp[e.len] = 0;
    ASSERT_STR(tmp, "second");
}

TEST(enter_pushes_to_history) {
    LineEditor e;
    uchar *out;
    int out_len, rl;
    lined_init(&e);
    feed_str(&e, "command1");
    lined_feed_byte(&e, '\n', &out, &out_len, NULL, 0, &rl);
    feed_str(&e, "command2");
    lined_feed_byte(&e, '\n', &out, &out_len, NULL, 0, &rl);
    ASSERT_EQ(e.hist_count, 2);
}

TEST(left_right_arrow) {
    LineEditor e;
    lined_init(&e);
    feed_str(&e, "abc");
    /* Left arrow: cursor 3 -> 2 */
    uchar redraw[256]; int rl;
    lined_feed_byte(&e, 0x1B, NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, '[', NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, 'D', NULL, NULL, redraw, sizeof(redraw), &rl);
    ASSERT_EQ(e.cursor, 2);
    /* Right arrow: cursor 2 -> 3 */
    lined_feed_byte(&e, 0x1B, NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, '[', NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, 'C', NULL, NULL, redraw, sizeof(redraw), &rl);
    ASSERT_EQ(e.cursor, 3);
}

TEST(insert_at_cursor) {
    LineEditor e;
    lined_init(&e);
    feed_str(&e, "abc");
    /* Cursor left twice to position 1 (after 'a') */
    uchar redraw[256]; int rl;
    lined_feed_byte(&e, 0x1B, NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, '[', NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, 'D', NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, 0x1B, NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, '[', NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, 'D', NULL, NULL, redraw, sizeof(redraw), &rl);
    ASSERT_EQ(e.cursor, 1);
    feed_str(&e, "X");
    /* Buffer should be "aXbc" */
    char tmp[16];
    memmove(tmp, e.buf, e.len);
    tmp[e.len] = 0;
    ASSERT_STR(tmp, "aXbc");
}

TEST(append_fast_path_no_prompt_wipe) {
    /* When cursor is at end of buffer, typing a printable char must emit
     * just that byte — NOT a CR+EL redraw sequence. Otherwise the rc
     * prompt sitting to the left of the editing region gets wiped on
     * every keystroke and the user sees nothing until Enter. */
    LineEditor e;
    uchar redraw[256];
    int rl = 0;
    lined_init(&e);
    int r = lined_feed_byte(&e, 'h', NULL, NULL, redraw, sizeof(redraw), &rl);
    ASSERT_EQ(r, LINEED_CONSUMED);
    ASSERT_EQ(rl, 1);
    ASSERT_EQ(redraw[0], 'h');
    r = lined_feed_byte(&e, 'i', NULL, NULL, redraw, sizeof(redraw), &rl);
    ASSERT_EQ(rl, 1);
    ASSERT_EQ(redraw[0], 'i');
    /* Mid-line insert should still use full redraw (the prompt-wipe is
     * unavoidable there, but rare). */
    lined_feed_byte(&e, 0x1B, NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, '[', NULL, NULL, redraw, sizeof(redraw), &rl);
    lined_feed_byte(&e, 'D', NULL, NULL, redraw, sizeof(redraw), &rl);
    r = lined_feed_byte(&e, 'X', NULL, NULL, redraw, sizeof(redraw), &rl);
    ASSERT_EQ(r, LINEED_CONSUMED);
    /* Full redraw starts with '\r' */
    ASSERT_EQ(redraw[0], '\r');
}

TEST(ctrl_u_clears_line) {
    LineEditor e;
    lined_init(&e);
    feed_str(&e, "hello world");
    uchar redraw[256]; int rl;
    lined_feed_byte(&e, 0x15, NULL, NULL, redraw, sizeof(redraw), &rl);
    ASSERT_EQ(e.len, 0);
    ASSERT_EQ(e.cursor, 0);
}

TEST(ctrl_w_deletes_word) {
    LineEditor e;
    lined_init(&e);
    feed_str(&e, "first second third");
    uchar redraw[256]; int rl;
    lined_feed_byte(&e, 0x17, NULL, NULL, redraw, sizeof(redraw), &rl);
    /* Should leave "first second " */
    char tmp[32];
    memmove(tmp, e.buf, e.len);
    tmp[e.len] = 0;
    ASSERT_STR(tmp, "first second ");
}

TEST(disabled_passthrough) {
    LineEditor e;
    lined_init(&e);
    lined_set_enabled(&e, 0);
    int r = lined_feed_byte(&e, 'x', NULL, NULL, NULL, 0, NULL);
    ASSERT_EQ(r, LINEED_PASSTHROUGH);
    ASSERT_EQ(e.len, 0);
}

int main(void) {
    printf("=== test_lined ===\n");
    test_simple_typing_runner();
    test_backspace_runner();
    test_enter_completes_line_runner();
    test_history_push_and_recall_runner();
    test_enter_pushes_to_history_runner();
    test_left_right_arrow_runner();
    test_insert_at_cursor_runner();
    test_append_fast_path_no_prompt_wipe_runner();
    test_ctrl_u_clears_line_runner();
    test_ctrl_w_deletes_word_runner();
    test_disabled_passthrough_runner();
    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
