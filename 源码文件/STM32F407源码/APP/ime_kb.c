#include "ime_kb.h"
#include "soft_uart.h"

#include <string.h>
#include <stdio.h>

/* ============================================================
 * 外部依赖
 * ============================================================ */
extern lv_font_t *g_bin_font;

#define IME_CAND_MAX   5
#define IME_KB_N       41
#define IME_KB_FUHAO   42

/* ============================================================
 * 状态
 * ============================================================ */
static lv_obj_t *s_layer;          /* 弹出层（挂在顶层） */
static lv_obj_t *s_kb;             /* 键盘 */
static lv_obj_t *s_cand_row;
static lv_obj_t *s_cand_btn[IME_CAND_MAX];
static lv_obj_t *s_cand_lbl[IME_CAND_MAX];

static lv_obj_t *s_input;          /* 当前输入框 */
static lv_obj_t *s_orig_parent;    /* 输入框原来的父对象 */
static lv_coord_t s_in_x, s_in_y, s_in_w, s_in_h;

static ime_kb_cb_t s_on_send;
static ime_kb_cb_t s_on_close;

/* 多个输入框共用同一个键盘：点谁用谁的回调 */
#define IME_BIND_MAX 4

typedef struct
{
    lv_obj_t     *input;
    ime_kb_cb_t   on_send;
    ime_kb_cb_t   on_close;
} ime_bind_t;

static ime_bind_t s_binds[IME_BIND_MAX];
static uint8_t    s_bind_n;

static char    s_cand[IME_CAND_MAX][32];
static uint8_t s_cand_n;

static uint8_t  s_cn    = 1;
static uint8_t  s_shift = 0;
static uint8_t  s_sym   = 0;
static uint8_t  s_open  = 0;
static uint8_t  s_page  = 1;
static uint8_t  s_busy  = 0;
static uint32_t s_busy_ms = 0;

/* 独立拼音缓冲区：与输入框内容完全解耦。
 * 只有"拼"模式下打的字母才会进来，候选只根据它生成。 */
#define IME_PY_MAX 40
static char    s_py[IME_PY_MAX + 1];
static uint8_t s_py_n = 0;

static const lv_font_t *s_font;

/* ============================================================
 * 键盘布局
 * ============================================================ */
static const char *kb_low[] = {/*大写*/
    "符", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "删", "\n",
    
    "ABC","a", "s", "d", "f", "g", "h", "j", "k", "l", "收起", "\n",
    
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    
    "拼", "<", "空格", ">", "发", "\n",
    
    ""
};

static const char *kb_up[] = {/*大写*/
    "符", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "删", "\n",
    
    "abc","A", "S", "D", "F", "G", "H", "J", "K", "L", "收起", "\n",
    
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    
    "拼", "<", "空格", ">", "发", "\n",
    
    ""
};

static const char *kb_sym[] = { /*符号*/
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "删", "\n",
    
    "abc","!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "收起", "\n",
    
    "_", "-", "+", "=", "/", "*", "?", "!", "~", "'", ".", ",", ":", "\n",
    
    "拼", "<", "空格", ">", "发", "\n",
    
    ""
};

/* 按键宽度权重：1=标准，2=双倍，5=空格，3=发送 */
static const lv_btnmatrix_ctrl_t kb_ctrl[IME_KB_N] = {
    1,1,1,1,1,1,1,1,1,1,1,1,
    2,1,1,1,1,1,1,1,1,1,2,
    1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,5,1,1
};

/* 符号模式下按键宽度表（当前与 kb_ctrl 相同，预留扩展） */
static const lv_btnmatrix_ctrl_t kb_fuhao[IME_KB_FUHAO] = {
    1,1,1,1,1,1,1,1,1,1,2,
    2,1,1,1,1,1,1,1,1,1,1,2,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,5,1,1
};

static const char *s_kb_buf[64];

/* ============================================================
 * 工具
 * ============================================================ */

/* 判断字符是否为 ASCII 字母（a-z / A-Z） */
static int is_ascii_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* 通过软串口发送一行字符串（追加 '\n'） */
static void send_line(const char *s)
{
    suart_send((const uint8_t *)s, (uint16_t)strlen(s));
    suart_send((const uint8_t *)"\n", 1);
}

static void cand_clear(void);      /* 前向声明 */

/* 把拼音"落定"：清空拼音缓冲、重置页码并清空候选；
 * 输入框里已写入的字母原样保留（当作英文） */
static void py_flush(void)
{
    s_py_n   = 0;
    s_py[0]  = 0;
    s_page   = 1;

    cand_clear();
}

/* 给按钮设置统一风格：圆角、无边框、无阴影、纯色背景 */
static void style_btn(lv_obj_t *btn, lv_color_t c)
{
    lv_obj_set_style_bg_color(btn, c, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
}

/* 通过调整上下 padding 让输入框文字垂直居中；
 * 高度自适应的输入框跳过此处理 */
static void center_text_vertically(lv_obj_t *ta)
{
    const lv_font_t *f = lv_obj_get_style_text_font(ta, LV_PART_MAIN);
    lv_coord_t h, lh, border, avail, pad;

    if (f == NULL) f = s_font;
    if (f == NULL) return;

    /* 高度自适应的输入框：改 pad 会反过来改变框高，不做居中 */
    if (lv_obj_get_style_height(ta, 0) == LV_SIZE_CONTENT)
    {
        lv_obj_set_style_pad_top(ta, 2, 0);
        lv_obj_set_style_pad_bottom(ta, 2, 0);
        return;
    }

    h      = lv_obj_get_height(ta);
    lh     = lv_font_get_line_height(f);
    border = lv_obj_get_style_border_width(ta, 0);      /* 边框会吃掉内容高度 */
    avail  = h - border * 2;

    pad = (avail - lh) / 2;
    if (pad < 0) pad = 0;

    lv_obj_set_style_pad_top(ta, pad, 0);
    lv_obj_set_style_pad_bottom(ta, 0, 0);
    lv_obj_scroll_to_y(ta, 0, LV_ANIM_OFF);
}

/* ============================================================
 * 拼音请求
 * ============================================================ */

/* 向输入法后端发送 "PY <拼音> [页码]" 查询候选；
 * 无拼音时直接清空候选；150ms 内重复请求做节流 */
static void py_request(void)
{
    char cmd[64];

    if (s_py_n == 0)                 /* 没有正在输入的拼音：清空候选 */
    {
        cand_clear();
        return;
    }

    if (s_busy && (lv_tick_get() - s_busy_ms) < 150) return;

    s_busy    = 1;
    s_busy_ms = lv_tick_get();

    if (s_page <= 1)
        snprintf(cmd, sizeof(cmd), "PY %s", s_py);
    else
        snprintf(cmd, sizeof(cmd), "PY %s %u", s_py, (unsigned)s_page);

    send_line(cmd);
}

/* ============================================================
 * 候选词
 * ============================================================ */

/* 清空候选：计数归零、清空标签文字、隐藏所有候选按钮 */
static void cand_clear(void)
{
    uint8_t i;

    s_cand_n = 0;

    for (i = 0; i < IME_CAND_MAX; i++)
    {
        lv_label_set_text(s_cand_lbl[i], "");
        lv_obj_add_flag(s_cand_btn[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* 解析后端返回的候选串（"词1|词2|词3..."），填到候选按钮里并显示；
 * 收到空串或 "NA" 时忽略 */
static void cand_show(const char *payload)
{
    uint8_t n = 0;
    const char *p = payload;

    if (payload[0] == 0) return;
    if (strcmp(payload, "NA") == 0) return;

    while (*p && n < IME_CAND_MAX)
    {
        const char *q = strchr(p, '|');
        int len = q ? (int)(q - p) : (int)strlen(p);

        if (len > 0 && len < (int)sizeof(s_cand[0]))
        {
            memcpy(s_cand[n], p, len);
            s_cand[n][len] = 0;
            lv_label_set_text(s_cand_lbl[n], s_cand[n]);
            lv_obj_clear_flag(s_cand_btn[n], LV_OBJ_FLAG_HIDDEN);
            n++;
        }

        if (!q) break;
        p = q + 1;
    }

    s_cand_n = n;
}

/* 提交第 idx 个候选：
 * 先删掉输入框里本次拼音写入的字母，再插入候选词，最后清空拼音状态 */
static void commit_cand(uint8_t idx)
{
    uint8_t k;

    if (idx >= s_cand_n) return;

    /* 只删掉本次拼音写入的那些字母，前面的英文/汉字不受影响 */
    for (k = 0; k < s_py_n; k++)
        lv_textarea_del_char(s_input);

    lv_textarea_add_text(s_input, s_cand[idx]);

    /* 提交候选后清除垂直滚动（否则文字会上移） */
    lv_obj_scroll_to_y(s_input, 0, LV_ANIM_OFF);

    s_py_n  = 0;
    s_py[0] = 0;

    cand_clear();
    s_page = 1;
}

/* 候选按钮点击事件：从 user_data 取出索引并提交 */
static void cand_clicked(lv_event_t *e)
{
    commit_cand((uint8_t)(intptr_t)lv_event_get_user_data(e));
}

/* ============================================================
 * 层显隐
 * ============================================================ */
static void ime_kb_close_cb(lv_event_t *e);

/* 打开键盘：
 * ① 记录输入框原父对象与位置尺寸；② 搬到弹层顶部并放大；③ 清拼音、显示弹层 */
void ime_kb_open(void)
{
    if (s_layer == NULL || s_input == NULL) return;
    if (s_open) return;

    s_open = 1;
    /* ① 记住输入框原来的父对象和位置尺寸 */
    s_orig_parent = lv_obj_get_parent(s_input);
    s_in_x = lv_obj_get_x(s_input);
    s_in_y = lv_obj_get_y(s_input);
    s_in_w = lv_obj_get_width(s_input);
    s_in_h = lv_obj_get_height(s_input);
     /* ② 搬到弹窗顶部并放大 */
    lv_obj_set_parent(s_input, s_layer);
    lv_obj_set_align(s_input, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(s_input, 3, 2);
    lv_obj_set_size(s_input, 314, 28);
    center_text_vertically(s_input);

    py_flush();                  /* 打开时清空拼音缓冲 */
    s_page = 1;

    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_layer);
}

/* 关闭键盘：把输入框恢复到原来的父对象、位置和尺寸，清拼音并隐藏弹层 */
void ime_kb_close(void)
{
    if (s_open == 0) return;

    s_open = 0;

    if (s_orig_parent)
    {
        lv_obj_set_parent(s_input, s_orig_parent);
        lv_obj_set_align(s_input, LV_ALIGN_DEFAULT);
        lv_obj_set_pos(s_input, s_in_x, s_in_y);
        lv_obj_set_size(s_input, s_in_w, s_in_h);
    }

    py_flush();
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
}

/* 返回键盘当前是否打开（1=打开，0=关闭） */
int ime_kb_is_open(void)
{
    return s_open;
}

/* "收起"按钮回调：关闭键盘，并以输入框文本触发 on_close 回调 */
static void ime_kb_close_cb(lv_event_t *e)
{
    (void)e;

    ime_kb_close();

    if (s_on_close)
    {
        s_on_close(lv_textarea_get_text(s_input));
    }
}

/* 输入框点击回调：从绑定表里找到被点的输入框，切换其回调并打开键盘 */
static void input_clicked(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    uint8_t   i;

    for (i = 0; i < s_bind_n; i++)
    {
        if (s_binds[i].input == target)
        {
            s_input    = s_binds[i].input;
            s_on_send  = s_binds[i].on_send;
            s_on_close = s_binds[i].on_close;
            break;
        }
    }

    ime_kb_open();
}

/* ============================================================
 * 键盘布局刷新 / 事件
 * ============================================================ */

/* 根据 s_sym / s_shift 选择布局表，动态改写"拼/英"字样，
 * 然后一次性应用到 btnmatrix（map + ctrl_map） */
static void kb_apply(void)
{
    const char **src;
    int i = 0;

    src = s_sym ? kb_sym : (s_shift ? kb_up : kb_low);

    while (src[i][0] != 0 && i < 60)
    {
        s_kb_buf[i] = src[i];
        i++;
    }
    s_kb_buf[i] = "";

    for (i = 0; s_kb_buf[i][0] != 0; i++)
    {
        if (strcmp(s_kb_buf[i], "拼") == 0 || strcmp(s_kb_buf[i], "英") == 0)
            s_kb_buf[i] = s_cn ? "拼" : "英";
    }

    if (s_kb)
    {
        lv_btnmatrix_set_map(s_kb, s_kb_buf);
        if(s_sym)
        lv_btnmatrix_set_ctrl_map(s_kb, kb_fuhao);
        else
        lv_btnmatrix_set_ctrl_map(s_kb, kb_ctrl);
    }
}

/* 键盘按键事件：按按键文字分派——切布局、退格、空格、发送、
 * 中英切换、候选翻页、字母入拼音缓冲 / 普通字符插入 */
static void kb_event_cb(lv_event_t *e)
{
    lv_obj_t   *kb = lv_event_get_target(e);
    uint16_t    id = lv_btnmatrix_get_selected_btn(kb);
    const char *t  = lv_btnmatrix_get_btn_text(kb, id);

    if (t == NULL || s_input == NULL) return;

    if (strcmp(t, "符") == 0)  { s_sym = 1; s_shift = 0; kb_apply(); return; }
    if (strcmp(t, "ABC") == 0) { s_shift = 1; s_sym = 0; kb_apply(); return; }
    if (strcmp(t, "abc") == 0) { s_shift = 0; s_sym = 0; kb_apply(); return; }

    if (strcmp(t, "删") == 0)
    {
        if (s_py_n > 0)                       /* 正在组拼音：只退拼音 */
        {
            lv_textarea_del_char(s_input);
            s_py[--s_py_n] = 0;
            s_page = 1;

            if (s_py_n) py_request();
            else        cand_clear();
        }
        else                                  /* 否则删普通文字 */
        {
            lv_textarea_del_char(s_input);
            s_page = 1;
        }

        lv_obj_scroll_to_y(s_input, 0, LV_ANIM_OFF);
        return;
    }

    if (strcmp(t, "收起") == 0)
    {
        ime_kb_close_cb(NULL);
        return;
    }

    if (strcmp(t, "空格") == 0)
    {
        if (s_cand_n)                 commit_cand(0);   /* 有候选：上屏 */
        else                                      /* 没候选：拼音当作英文落定 */
        {
            py_flush();
            lv_textarea_add_char(s_input, ' ');
        }
        return;
    }

    if (strcmp(t, "发") == 0)
    {
        char text[220];

        py_flush();                   /* 未上屏的拼音按英文保留 */

        snprintf(text, sizeof(text), "%s", lv_textarea_get_text(s_input));

        ime_kb_close();

        if (s_on_send) s_on_send(text);
        return;
    }

    if (strcmp(t, "拼") == 0 || strcmp(t, "英") == 0)
    {
        py_flush();                   /* 切换前先把拼音落定 */
        s_cn = s_cn ? 0 : 1;
        kb_apply();
        return;
    }

    if (strcmp(t, "<") == 0)
    {
        if (s_page > 1) { s_page--; py_request(); }
        return;
    }

    if (strcmp(t, ">") == 0)
    {
        if (s_page < 20) { s_page++; py_request(); }
        return;
    }

    if (s_cn && is_ascii_alpha((unsigned char)t[0]) && s_py_n < IME_PY_MAX)
    {
        /* 拼模式下的字母：进入拼音缓冲区，只按它查候选 */
        s_py[s_py_n++] = t[0];
        s_py[s_py_n]   = 0;

        lv_textarea_add_text(s_input, t);
        lv_obj_scroll_to_y(s_input, 0, LV_ANIM_OFF);

        s_page = 1;
        py_request();
    }
    else
    {
        /* 英模式 / 符号 / 缓冲区满：先把拼音落定，再插入字符 */
        py_flush();

        lv_textarea_add_text(s_input, t);
        lv_obj_scroll_to_y(s_input, 0, LV_ANIM_OFF);
    }
}

/* ============================================================
 * 创建
 * ============================================================ */

/* 构建整个输入法 UI（只调用一次）：
 * 检测中文字体 → 创建弹层 → 收起按钮 → 候选行 → 键盘，最后隐藏弹层 */
static void build(void)
{
    lv_obj_t *btn;
    uint8_t i;
    lv_font_glyph_dsc_t d;

    s_font = g_bin_font;
    if (!(s_font && lv_font_get_glyph_dsc(s_font, &d, 0x62FC, 0))) s_font = NULL;

    s_layer = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_layer, 320, 200);
    lv_obj_set_align(s_layer, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_pos(s_layer, 0, 30);
    lv_obj_set_style_bg_color(s_layer, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_layer, 0, 0);
    lv_obj_set_style_border_width(s_layer, 0, 0);
    lv_obj_set_style_pad_all(s_layer, 2, 0);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);

    /* 收起按钮 */
    btn = lv_btn_create(s_layer);
    lv_obj_set_size(btn, 40, 24);
    lv_obj_set_pos(btn, 276, 4);
    style_btn(btn, lv_color_hex(0x552222));
    lv_obj_add_event_cb(btn, ime_kb_close_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, "收");
        if (s_font) lv_obj_set_style_text_font(l, s_font, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
    }

    /* 候选行 */
    s_cand_row = lv_obj_create(s_layer);
    lv_obj_set_size(s_cand_row, 316, 30);
    lv_obj_set_pos(s_cand_row, 2, 34);
    lv_obj_set_style_bg_opa(s_cand_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cand_row, 0, 0);
    lv_obj_set_style_pad_all(s_cand_row, 0, 0);
    lv_obj_set_flex_flow(s_cand_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_cand_row, 4, 0);
    lv_obj_set_scrollbar_mode(s_cand_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_cand_row, LV_DIR_HOR);

    for (i = 0; i < IME_CAND_MAX; i++)
    {
        s_cand_btn[i] = lv_btn_create(s_cand_row);
        lv_obj_set_size(s_cand_btn[i], 58, 28);
        style_btn(s_cand_btn[i], lv_color_hex(0xE0E0E0));

        s_cand_lbl[i] = lv_label_create(s_cand_btn[i]);
        lv_label_set_text(s_cand_lbl[i], "");
        if (s_font) lv_obj_set_style_text_font(s_cand_lbl[i], s_font, 0);
        lv_obj_set_style_text_color(s_cand_lbl[i], lv_color_black(), 0);
        lv_obj_center(s_cand_lbl[i]);

        lv_obj_add_event_cb(s_cand_btn[i], cand_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_flag(s_cand_btn[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 键盘 */
    s_kb = lv_btnmatrix_create(s_layer);
    lv_obj_set_size(s_kb, 316, 128);
    lv_obj_set_pos(s_kb, 2, 68);
    kb_apply();

    if (s_font)
    {
        lv_obj_set_style_text_font(s_kb, s_font, LV_PART_ITEMS);
        lv_obj_set_style_text_font(s_kb, s_font, 0);
    }

    lv_obj_set_style_pad_all(s_kb, 2, 0);
    lv_obj_set_style_pad_row(s_kb, 3, 0);
    lv_obj_set_style_pad_column(s_kb, 2, 0);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(0x87CEEB), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kb, 4, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(0x1E90FF), LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
}

/* ============================================================
 * 对外接口
 * ============================================================ */

/* 绑定输入框：首次调用创建 UI；登记回调到绑定表；
 * 配置输入框样式（单行、隐藏滚动条、水平滚动、最大长度、中文字体、垂直居中） */
void ime_kb_bind(lv_obj_t *input, ime_kb_cb_t on_send, ime_kb_cb_t on_close)
{
    if (input == NULL) return;

    if (s_layer == NULL) build();

    if (s_open) ime_kb_close();

    /* 登记到表里（已登记过就更新回调） */
    {
        uint8_t i;
        int     found = 0;

        for (i = 0; i < s_bind_n; i++)
        {
            if (s_binds[i].input == input)
            {
                s_binds[i].on_send  = on_send;
                s_binds[i].on_close = on_close;
                found = 1;
                break;
            }
        }

        if (!found && s_bind_n < IME_BIND_MAX)
        {
            s_binds[s_bind_n].input    = input;
            s_binds[s_bind_n].on_send  = on_send;
            s_binds[s_bind_n].on_close = on_close;
            s_bind_n++;

            lv_obj_add_event_cb(input, input_clicked, LV_EVENT_CLICKED, NULL);
        }
    }

    s_input    = input;
    s_on_send  = on_send;
    s_on_close = on_close;

    py_flush();

    if (s_font)
    {
        lv_obj_set_style_text_font(input, s_font, LV_PART_MAIN);
    }

    lv_textarea_set_one_line(input, 1);
    lv_obj_set_scrollbar_mode(input, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_anim_time(input, 0, 0);
    lv_obj_set_scroll_dir(input, LV_DIR_HOR);
    lv_obj_scroll_to_y(input, 0, LV_ANIM_OFF);
    lv_textarea_set_max_length(input, 100);

    center_text_vertically(input);
}

/* 处理串口收到的一行：识别 "PYC <候选串>" 协议，更新候选；
 * 返回 1 表示已处理，0 表示不是候选行 */
int ime_kb_handle_line(char *s)
{
    char *m = strstr(s, "PYC ");

    if (m == NULL) return 0;

    s_busy = 0;
    cand_show(m + 4);

    return 1;
}
