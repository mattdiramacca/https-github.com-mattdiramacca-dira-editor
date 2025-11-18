/* main.c - full-featured editor with gap buffer */
#define _POSIX_C_SOURCE 200809L
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#define ABUF_SIZE 32768
#define TAB_STOP 4

/* -------- key definitions -------- */
enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* -------- syntax highlighting -------- */
enum editorHighlight {
    HL_NORMAL = 0,
    HL_KEYWORD,
    HL_STRING,
    HL_COMMENT,
    HL_NUMBER
};

/* -------- undo/redo system -------- */
enum editType {
    EDIT_INSERT,
    EDIT_DELETE,
    EDIT_INSERT_NEWLINE,
    EDIT_DELETE_NEWLINE
};

struct edit {
    enum editType type;
    int pos;           // Position in buffer
    char ch;           // Character (for single char ops)
    struct edit *next;
    struct edit *prev;
};

struct editHistory {
    struct edit *undoStack;
    struct edit *redoStack;
    int grouping;      // For grouping sequential edits
};

/* -------- selection/clipboard -------- */
struct selection {
    int active;
    int start_row, start_col;
    int end_row, end_col;
};

struct clipboard {
    char *data;
    int len;
};

/* -------- editor state -------- */
struct editorConfig {
    int cx, cy;
    int rowoff, coloff;
    int screenrows, screencols;
    struct termios orig_termios;
    char *filename;
    int dirty;
    char statusmsg[80];
    struct editHistory history;
    struct selection sel;
    struct clipboard clip;
    char *search_query;
    int search_direction;
    int search_match_pos;
    int show_welcome;
};

static struct editorConfig E;

/* -------- gap buffer -------- */
struct gapbuf {
    char *buf;
    int cap;
    int gap_start;
    int gap_end;
};

struct gapbuf g;

void gap_init(struct gapbuf *g, int initial_cap) {
    g->cap = initial_cap > 0 ? initial_cap : 1024;
    g->buf = malloc(g->cap);
    g->gap_start = 0;
    g->gap_end = g->cap;
}

void gap_free(struct gapbuf *g) { free(g->buf); }

int gap_length(struct gapbuf *g) { return g->cap - (g->gap_end - g->gap_start); }

void gap_move(struct gapbuf *g, int pos) {
    if (pos < 0) pos = 0;
    int len = gap_length(g);
    if (pos > len) pos = len;
    if (pos < g->gap_start) {
        int move_len = g->gap_start - pos;
        g->gap_end -= move_len;
        memmove(g->buf + g->gap_end, g->buf + pos, move_len);
        g->gap_start = pos;
    } else if (pos > g->gap_start) {
        int move_len = pos - g->gap_start;
        memmove(g->buf + g->gap_start, g->buf + g->gap_end, move_len);
        g->gap_start += move_len;
        g->gap_end += move_len;
    }
}

void gap_insert(struct gapbuf *g, char c) {
    if (g->gap_start == g->gap_end) {
        int gap_size = g->cap / 2;
        int newcap = g->cap + gap_size;
        char *nb = malloc(newcap);
        int prefix = g->gap_start;
        int suffix = g->cap - g->gap_end;
        if (prefix) memcpy(nb, g->buf, prefix);
        if (suffix) memcpy(nb + newcap - suffix, g->buf + g->gap_end, suffix);
        g->gap_end = newcap - suffix;
        g->cap = newcap;
        free(g->buf);
        g->buf = nb;
    }
    g->buf[g->gap_start++] = c;
}

int gap_backspace(struct gapbuf *g) {
    if (g->gap_start == 0) return 0;
    g->gap_start--;
    return 1;
}

int gap_delete(struct gapbuf *g) {
    if (g->gap_end == g->cap) return 0;
    g->gap_end++;
    return 1;
}

int gap_get(struct gapbuf *g, char *out, int outcap) {
    int len = gap_length(g);
    if (outcap < len) return -1;
    int prefix = g->gap_start;
    int suffix = g->cap - g->gap_end;
    if (prefix) memcpy(out, g->buf, prefix);
    if (suffix) memcpy(out + prefix, g->buf + g->gap_end, suffix);
    return len;
}

char gap_char_at(struct gapbuf *g, int pos) {
    if (pos < 0 || pos >= gap_length(g)) return '\0';
    if (pos < g->gap_start) return g->buf[pos];
    return g->buf[g->gap_end + (pos - g->gap_start)];
}

/* -------- undo/redo implementation -------- */

void history_init(struct editHistory *h) {
    h->undoStack = NULL;
    h->redoStack = NULL;
    h->grouping = 0;
}

void history_free_stack(struct edit *stack) {
    while (stack) {
        struct edit *next = stack->next;
        free(stack);
        stack = next;
    }
}

void history_free(struct editHistory *h) {
    history_free_stack(h->undoStack);
    history_free_stack(h->redoStack);
}

void history_push(struct editHistory *h, enum editType type, int pos, char ch) {
    struct edit *e = malloc(sizeof(struct edit));
    e->type = type;
    e->pos = pos;
    e->ch = ch;
    e->next = h->undoStack;
    e->prev = NULL;
    if (h->undoStack) h->undoStack->prev = e;
    h->undoStack = e;
    
    // Clear redo stack when new edit is made
    history_free_stack(h->redoStack);
    h->redoStack = NULL;
}

int history_undo(struct editHistory *h) {
    if (!h->undoStack) return 0;
    
    struct edit *e = h->undoStack;
    h->undoStack = e->next;
    if (h->undoStack) h->undoStack->prev = NULL;
    
    // Move to redo stack
    e->next = h->redoStack;
    e->prev = NULL;
    if (h->redoStack) h->redoStack->prev = e;
    h->redoStack = e;
    
    // Apply inverse operation
    gap_move(&g, e->pos);
    switch (e->type) {
        case EDIT_INSERT:
            gap_delete(&g);
            break;
        case EDIT_DELETE:
            gap_insert(&g, e->ch);
            break;
        case EDIT_INSERT_NEWLINE:
            gap_delete(&g);
            break;
        case EDIT_DELETE_NEWLINE:
            gap_insert(&g, '\n');
            break;
    }
    
    return 1;
}

int history_redo(struct editHistory *h) {
    if (!h->redoStack) return 0;
    
    struct edit *e = h->redoStack;
    h->redoStack = e->next;
    if (h->redoStack) h->redoStack->prev = NULL;
    
    // Move back to undo stack
    e->next = h->undoStack;
    e->prev = NULL;
    if (h->undoStack) h->undoStack->prev = e;
    h->undoStack = e;
    
    // Reapply operation
    gap_move(&g, e->pos);
    switch (e->type) {
        case EDIT_INSERT:
            gap_insert(&g, e->ch);
            break;
        case EDIT_DELETE:
            gap_delete(&g);
            break;
        case EDIT_INSERT_NEWLINE:
            gap_insert(&g, '\n');
            break;
        case EDIT_DELETE_NEWLINE:
            gap_delete(&g);
            break;
    }
    
    return 1;
}

/* -------- selection & clipboard -------- */

void selection_start(struct selection *sel, int row, int col) {
    sel->active = 1;
    sel->start_row = sel->end_row = row;
    sel->start_col = sel->end_col = col;
}

void selection_update(struct selection *sel, int row, int col) {
    sel->end_row = row;
    sel->end_col = col;
}

void selection_clear(struct selection *sel) {
    sel->active = 0;
}

int selection_contains(struct selection *sel, int row, int col) {
    if (!sel->active) return 0;
    
    int sr = sel->start_row, sc = sel->start_col;
    int er = sel->end_row, ec = sel->end_col;
    
    // Normalize (ensure start < end)
    if (sr > er || (sr == er && sc > ec)) {
        int tmp = sr; sr = er; er = tmp;
        tmp = sc; sc = ec; ec = tmp;
    }
    
    if (row < sr || row > er) return 0;
    if (row == sr && row == er) return col >= sc && col < ec;
    if (row == sr) return col >= sc;
    if (row == er) return col < ec;
    return 1;
}

void clipboard_copy(struct clipboard *clip, struct selection *sel);
void clipboard_free(struct clipboard *clip) {
    if (clip->data) free(clip->data);
    clip->data = NULL;
    clip->len = 0;
}

/* -------- append buffer -------- */
static char abuf[ABUF_SIZE];
static int abuf_len = 0;
void abufAppend(const char *s, int len) { 
    if (abuf_len + len < ABUF_SIZE) {
        memcpy(abuf + abuf_len, s, len); 
        abuf_len += len; 
    }
}
void abufFlush(void) { write(STDOUT_FILENO, abuf, abuf_len); abuf_len = 0; }

/* -------- raw mode -------- */
void disableRawMode(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios); }
void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) exit(1);
    atexit(disableRawMode);
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0; 
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* -------- terminal size -------- */
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) return -1;
    *cols = ws.ws_col; *rows = ws.ws_row; return 0;
}

/* -------- syntax highlighting -------- */

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

enum editorHighlight get_highlight(const char *content, int len, int pos, const char *filename) {
    if (pos >= len) return HL_NORMAL;
    
    // Determine file type
    int is_c = 0;
    if (filename) {
        char *ext = strrchr(filename, '.');
        if (ext && (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0 || 
                    strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0)) {
            is_c = 1;
        }
    }
    
    if (!is_c) return HL_NORMAL;
    
    char c = content[pos];
    
    // Check for comment
    if (pos + 1 < len && content[pos] == '/' && content[pos + 1] == '/') {
        return HL_COMMENT;
    }
    
    // Numbers
    if (isdigit(c)) {
        if (pos == 0 || is_separator(content[pos - 1])) {
            return HL_NUMBER;
        }
    }
    
    // Strings
    static int in_string = 0;
    if (c == '"') in_string = !in_string;
    if (in_string) return HL_STRING;
    
    // Keywords (simple prefix matching)
    static const char *keywords[] = {
        "if", "else", "while", "for", "return", "int", "char", "void",
        "struct", "enum", "static", "const", "break", "continue", "switch",
        "case", "default", "sizeof", "typedef", NULL
    };
    
    for (int i = 0; keywords[i]; i++) {
        int klen = strlen(keywords[i]);
        if (pos + klen <= len && memcmp(content + pos, keywords[i], klen) == 0) {
            if (pos + klen >= len || is_separator(content[pos + klen])) {
                if (pos == 0 || is_separator(content[pos - 1])) {
                    return HL_KEYWORD;
                }
            }
        }
    }
    
    return HL_NORMAL;
}

const char* highlight_to_color(enum editorHighlight hl) {
    switch (hl) {
        case HL_KEYWORD: return "\x1b[33m";  // Yellow
        case HL_STRING:  return "\x1b[32m";  // Green
        case HL_COMMENT: return "\x1b[36m";  // Cyan
        case HL_NUMBER:  return "\x1b[31m";  // Red
        default:         return "\x1b[37m";  // White
    }
}

/* -------- cursor/position conversion -------- */

void pos_to_rowcol(int pos, int *row, int *col) {
    *row = 0;
    *col = 0;
    for (int i = 0; i < pos; i++) {
        char c = gap_char_at(&g, i);
        if (c == '\n') {
            (*row)++;
            *col = 0;
        } else {
            (*col)++;
        }
    }
}

int rowcol_to_pos(int row, int col) {
    int pos = 0;
    int cur_row = 0;
    int cur_col = 0;
    int len = gap_length(&g);
    
    while (pos < len && cur_row < row) {
        if (gap_char_at(&g, pos) == '\n') {
            cur_row++;
            cur_col = 0;
        } else {
            cur_col++;
        }
        pos++;
    }
    
    while (pos < len && cur_col < col) {
        char c = gap_char_at(&g, pos);
        if (c == '\n') break;
        cur_col++;
        pos++;
    }
    
    return pos;
}

int get_line_length(int row) {
    int pos = rowcol_to_pos(row, 0);
    int len = gap_length(&g);
    int line_len = 0;
    
    while (pos < len) {
        char c = gap_char_at(&g, pos);
        if (c == '\n') break;
        line_len++;
        pos++;
    }
    
    return line_len;
}

int get_line_indent(int row) {
    int pos = rowcol_to_pos(row, 0);
    int len = gap_length(&g);
    int indent = 0;
    
    while (pos < len) {
        char c = gap_char_at(&g, pos);
        if (c == ' ') indent++;
        else if (c == '\t') indent += TAB_STOP;
        else break;
        pos++;
    }
    
    return indent;
}

int count_rows(void) {
    int len = gap_length(&g);
    int rows = 1;
    for (int i = 0; i < len; i++) {
        if (gap_char_at(&g, i) == '\n') rows++;
    }
    return rows;
}

/* -------- file I/O -------- */

void editorOpen(char *filename) {
    E.filename = strdup(filename);
    
    FILE *fp = fopen(filename, "r");
    if (!fp) return;
    
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        for (ssize_t i = 0; i < linelen; i++) {
            gap_insert(&g, line[i]);
        }
    }
    
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave(void) {
    if (E.filename == NULL) {
        // TODO: Prompt for filename
        snprintf(E.statusmsg, sizeof(E.statusmsg), "No filename!");
        return;
    }
    
    char tmp[65536];
    int len = gap_get(&g, tmp, sizeof(tmp));
    
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, tmp, len) == len) {
                close(fd);
                E.dirty = 0;
                snprintf(E.statusmsg, sizeof(E.statusmsg), "Saved! %d bytes", len);
                return;
            }
        }
        close(fd);
    }
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Save failed!");
}

/* -------- search -------- */

void editorSearch(void) {
    if (!E.search_query) return;
    
    char tmp[65536];
    int len = gap_get(&g, tmp, sizeof(tmp));
    
    int start_pos = rowcol_to_pos(E.cy, E.cx);
    if (E.search_direction > 0) start_pos++;
    
    for (int offset = 0; offset < len; offset++) {
        int pos = (start_pos + offset * E.search_direction + len) % len;
        
        int match = 1;
        for (int i = 0; E.search_query[i]; i++) {
            if (pos + i >= len || tmp[pos + i] != E.search_query[i]) {
                match = 0;
                break;
            }
        }
        
        if (match) {
            pos_to_rowcol(pos, &E.cy, &E.cx);
            E.search_match_pos = pos;
            return;
        }
    }
    
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Not found: %s", E.search_query);
}

/* -------- clipboard operations -------- */

void clipboard_copy(struct clipboard *clip, struct selection *sel) {
    if (!sel->active) return;
    
    int sr = sel->start_row, sc = sel->start_col;
    int er = sel->end_row, ec = sel->end_col;
    
    // Normalize
    if (sr > er || (sr == er && sc > ec)) {
        int tmp = sr; sr = er; er = tmp;
        tmp = sc; sc = ec; ec = tmp;
    }
    
    int start_pos = rowcol_to_pos(sr, sc);
    int end_pos = rowcol_to_pos(er, ec);
    int copy_len = end_pos - start_pos;
    
    if (copy_len <= 0) return;
    
    clipboard_free(clip);
    clip->data = malloc(copy_len + 1);
    clip->len = copy_len;
    
    for (int i = 0; i < copy_len; i++) {
        clip->data[i] = gap_char_at(&g, start_pos + i);
    }
    clip->data[copy_len] = '\0';
}

void clipboard_paste(struct clipboard *clip) {
    if (!clip->data || clip->len == 0) return;
    
    int pos = rowcol_to_pos(E.cy, E.cx);
    gap_move(&g, pos);
    
    for (int i = 0; i < clip->len; i++) {
        gap_insert(&g, clip->data[i]);
        history_push(&E.history, EDIT_INSERT, pos + i, clip->data[i]);
    }
    
    E.dirty = 1;
}

void selection_delete(struct selection *sel) {
    if (!sel->active) return;
    
    int sr = sel->start_row, sc = sel->start_col;
    int er = sel->end_row, ec = sel->end_col;
    
    if (sr > er || (sr == er && sc > ec)) {
        int tmp = sr; sr = er; er = tmp;
        tmp = sc; sc = ec; ec = tmp;
    }
    
    int start_pos = rowcol_to_pos(sr, sc);
    int end_pos = rowcol_to_pos(er, ec);
    
    gap_move(&g, start_pos);
    for (int i = start_pos; i < end_pos; i++) {
        char ch = gap_char_at(&g, start_pos);
        gap_delete(&g);
        history_push(&E.history, EDIT_DELETE, start_pos, ch);
    }
    
    E.cy = sr;
    E.cx = sc;
    selection_clear(sel);
    E.dirty = 1;
}

/* -------- status bar -------- */

void editorDrawStatusBar(void) {
    abufAppend("\x1b[7m", 4);
    
    char status[80];
    char rstatus[80];
    int len = snprintf(status, sizeof(status), " %.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]",
        count_rows(),
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d,%d ", E.cy + 1, E.cx + 1);
    
    if (len > E.screencols) len = E.screencols;
    abufAppend(status, len);
    
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abufAppend(rstatus, rlen);
            break;
        } else {
            abufAppend(" ", 1);
            len++;
        }
    }
    
    abufAppend("\x1b[m", 3);
    abufAppend("\r\n", 2);
    
    // Message bar
    abufAppend("\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen) abufAppend(E.statusmsg, msglen);
}

/* -------- welcome screen -------- */

const char* welcome_lines[] = {
    "~",
    "~     ########  #### ########     ###    ",
    "~     ##     ##  ##  ##     ##   ## ##   ",
    "~     ##     ##  ##  ##     ##  ##   ##  ",
    "~     ##     ##  ##  ########  ##     ## ",
    "~     ##     ##  ##  ##   ##   ######### ",
    "~     ##     ##  ##  ##    ##  ##     ## ",
    "~     ########  #### ##     ## ##     ## ",
    "~",
    "~            DIRA version 1.0",
    "~        Terminal Text Editor",
    "~",
    "~",
    "~ ┌────────────────────────────────────────────────────────────────────┐",
    "~ │                        QUICK START GUIDE                           │",
    "~ ├────────────────────────────────────────────────────────────────────┤",
    "~ │                                                                    │",
    "~ │  BASIC EDITING              SELECTION & CLIPBOARD                 │",
    "~ │  ══════════════              ══════════════════════                │",
    "~ │  Arrow Keys ....... Move     Shift+Arrows ..... Select text       │",
    "~ │  Home/End ......... Line     Ctrl-A ........... Select all        │",
    "~ │  Page Up/Down ..... Scroll   Ctrl-C ........... Copy              │",
    "~ │  Backspace/Delete . Remove   Ctrl-X ........... Cut               │",
    "~ │  Tab .............. Spaces   Ctrl-V ........... Paste             │",
    "~ │  Enter ............ New line Escape ........... Clear selection   │",
    "~ │                                                                    │",
    "~ │  FILE OPERATIONS            EDITING COMMANDS                      │",
    "~ │  ════════════════            ════════════════                      │",
    "~ │  Ctrl-S ........... Save     Ctrl-Z ........... Undo              │",
    "~ │  Ctrl-Q ........... Quit     Ctrl-Y ........... Redo              │",
    "~ │  ./editor <file> .. Open     Ctrl-F ........... Find (coming!)    │",
    "~ │                                                                    │",
    "~ │  FEATURES                                                          │",
    "~ │  ════════                                                          │",
    "~ │  • Syntax highlighting for C/C++ files                            │",
    "~ │  • Line numbers with dynamic width                                │",
    "~ │  • Auto-indentation (matches previous line)                       │",
    "~ │  • Efficient gap buffer for large files                           │",
    "~ │  • Memory-efficient undo/redo system                              │",
    "~ │                                                                    │",
    "~ └────────────────────────────────────────────────────────────────────┘",
    "~",
    "~              Press any key to start editing...",
    "~",
    NULL
};

void drawWelcomeScreen(void) {
    abuf_len = 0;
    abufAppend("\x1b[?25l", 6); // Hide cursor
    abufAppend("\x1b[H", 3);    // Move to top-left
    abufAppend("\x1b[2J", 4);   // Clear screen
    
    int welcome_lines_count = 0;
    while (welcome_lines[welcome_lines_count] != NULL) {
        welcome_lines_count++;
    }
    
    int padding = (E.screenrows - welcome_lines_count) / 2;
    if (padding < 0) padding = 0;
    
    // Add vertical padding
    for (int i = 0; i < padding && i < E.screenrows - 2; i++) {
        abufAppend("~\x1b[K\r\n", 6);
    }
    
    // Draw welcome content
    for (int i = 0; welcome_lines[i] != NULL && padding + i < E.screenrows - 2; i++) {
        const char *line = welcome_lines[i];
        int len = strlen(line);
        
        // Skip the leading ~ for actual content
        const char *content = line;
        if (len > 0 && line[0] == '~') {
            content = line + 1;
            len--;
        }
        
        // Center the line horizontally
        int content_width = len;
        int left_padding = 0;
        if (content_width < E.screencols) {
            left_padding = (E.screencols - content_width) / 2;
        }
        
        // Add left padding
        for (int j = 0; j < left_padding; j++) {
            abufAppend(" ", 1);
        }
        
        // Apply colors based on content
        if (strstr(content, "#") != NULL && strstr(content, "##") != NULL) {
            // ASCII art logo
            abufAppend("\x1b[1;36m", 7); // Bold cyan
        } else if (strstr(content, "DIRA version") != NULL) {
            abufAppend("\x1b[1;33m", 7); // Bold yellow
        } else if (strstr(content, "Terminal Text Editor") != NULL) {
            abufAppend("\x1b[90m", 5); // Gray
        } else if (strstr(content, "QUICK START GUIDE") != NULL) {
            abufAppend("\x1b[1;32m", 7); // Bold green
        } else if (strstr(content, "┌") != NULL || strstr(content, "└") != NULL || 
                   strstr(content, "├") != NULL || strstr(content, "│") != NULL ||
                   strstr(content, "─") != NULL || strstr(content, "┐") != NULL ||
                   strstr(content, "┘") != NULL || strstr(content, "┤") != NULL) {
            abufAppend("\x1b[34m", 5); // Blue for box
        } else if (strstr(content, "BASIC EDITING") != NULL || 
                   strstr(content, "SELECTION") != NULL ||
                   strstr(content, "FILE OPERATIONS") != NULL || 
                   strstr(content, "EDITING COMMANDS") != NULL ||
                   strstr(content, "FEATURES") != NULL) {
            abufAppend("\x1b[1;37m", 7); // Bold white
        } else if (strstr(content, "Press any key") != NULL) {
            abufAppend("\x1b[1;35m", 7); // Bold magenta
        }
        
        // Write the content
        int write_len = len;
        if (left_padding + len > E.screencols) {
            write_len = E.screencols - left_padding;
        }
        if (write_len > 0) {
            abufAppend(content, write_len);
        }
        
        abufAppend("\x1b[0m", 4); // Reset color
        abufAppend("\x1b[K\r\n", 5); // Clear to end of line and newline
    }
    
    // Fill remaining rows
    int drawn_lines = padding + welcome_lines_count;
    while (drawn_lines < E.screenrows - 2) {
        abufAppend("~\x1b[K\r\n", 6);
        drawn_lines++;
    }
    
    // Status bar
    abufAppend("\x1b[7m", 4); // Invert colors
    char status[] = " Welcome to DIRA - Press any key to start";
    int slen = strlen(status);
    if (slen > E.screencols) slen = E.screencols;
    abufAppend(status, slen);
    while (slen < E.screencols) {
        abufAppend(" ", 1);
        slen++;
    }
    abufAppend("\x1b[m\r\n", 5); // Reset colors
    
    // Message bar
    abufAppend("\x1b[K", 3);
    
    abufAppend("\x1b[?25h", 6); // Show cursor
    abufFlush();
}

/* -------- refresh screen -------- */

void editorScroll(void) {
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows - 2) {
        E.rowoff = E.cy - E.screenrows + 3;
    }
    
    if (E.cx < E.coloff) {
        E.coloff = E.cx;
    }
    if (E.cx >= E.coloff + E.screencols - 5) {
        E.coloff = E.cx - E.screencols + 6;
    }
}

void editorRefreshScreen(void) {
    // Show welcome screen if no file is loaded
    if (E.show_welcome) {
        drawWelcomeScreen();
        return;
    }
    
    editorScroll();
    
    abuf_len = 0;
    abufAppend("\x1b[?25l", 6);
    abufAppend("\x1b[H", 3);

    char tmp[65536];
    int len = gap_get(&g, tmp, sizeof(tmp));
    
    int row = 0, col = 0;
    int screen_row = 0;
    
    // Line number width
    int num_width = snprintf(NULL, 0, "%d", count_rows()) + 1;
    
    // Draw line numbers
    char linenum[16];
    int ln_len = snprintf(linenum, sizeof(linenum), "%*d ", num_width, row + 1);
    abufAppend("\x1b[36m", 5);
    abufAppend(linenum, ln_len);
    abufAppend("\x1b[0m", 4);
    
    enum editorHighlight prev_hl = HL_NORMAL;
    
    for (int i = 0; i < len && screen_row < E.screenrows - 2; i++) {
        if (row < E.rowoff) {
            if (tmp[i] == '\n') row++;
            continue;
        }
        
        if (tmp[i] == '\n') {
            abufAppend("\x1b[K\r\n", 5);
            row++;
            col = 0;
            screen_row++;
            
            if (screen_row < E.screenrows - 2) {
                ln_len = snprintf(linenum, sizeof(linenum), "%*d ", num_width, row + 1);
                abufAppend("\x1b[36m", 5);
                abufAppend(linenum, ln_len);
                abufAppend("\x1b[0m", 4);
            }
        } else {
            if (col >= E.coloff && col < E.coloff + E.screencols - num_width - 1) {
                // Apply selection highlight
                if (selection_contains(&E.sel, row, col)) {
                    abufAppend("\x1b[7m", 4); // Invert
                    abufAppend(&tmp[i], 1);
                    abufAppend("\x1b[27m", 5); // Un-invert
                } else {
                    // Apply syntax highlighting
                    enum editorHighlight hl = get_highlight(tmp, len, i, E.filename);
                    if (hl != prev_hl) {
                        abufAppend(highlight_to_color(hl), 5);
                        prev_hl = hl;
                    }
                    abufAppend(&tmp[i], 1);
                }
            }
            col++;
        }
    }
    
    abufAppend("\x1b[0m", 4);
    
    while (screen_row < E.screenrows - 2) {
        abufAppend("~", 1);
        abufAppend("\x1b[K\r\n", 5);
        screen_row++;
    }
    
    editorDrawStatusBar();
    
    char buf[32];
    int l = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 
                     (E.cy - E.rowoff) + 1, 
                     (E.cx - E.coloff) + 1 + num_width + 1);
    abufAppend(buf, l);
    abufAppend("\x1b[?25h", 6);
    
    abufFlush();
}

/* -------- input -------- */

int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) exit(1);
    }
    
    if (c == '\x1b') {
        char seq[3];
        
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
                // Shift + arrow keys (e.g., Shift+Up = \x1b[1;2A)
                else if (seq[2] == 'A') return ARROW_UP | 0x1000;
                else if (seq[2] == 'B') return ARROW_DOWN | 0x1000;
                else if (seq[2] == 'C') return ARROW_RIGHT | 0x1000;
                else if (seq[2] == 'D') return ARROW_LEFT | 0x1000;
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        
        return '\x1b';
    }
    
    return c;
}

/* Check if key has shift modifier */
int is_shift_arrow(int key) {
    return key & 0x1000;
}

int get_base_key(int key) {
    return key & 0xFFF;
}

/* -------- cursor movement -------- */

void editorMoveCursor(int key) {
    int total_rows = count_rows();
    
    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = get_line_length(E.cy);
            }
            break;
            
        case ARROW_RIGHT: {
            int line_len = get_line_length(E.cy);
            if (E.cx < line_len) {
                E.cx++;
            } else if (E.cy < total_rows - 1) {
                E.cy++;
                E.cx = 0;
            }
            break;
        }
        
        case ARROW_UP:
            if (E.cy > 0) {
                E.cy--;
                int line_len = get_line_length(E.cy);
                if (E.cx > line_len) E.cx = line_len;
            }
            break;
            
        case ARROW_DOWN:
            if (E.cy < total_rows - 1) {
                E.cy++;
                int line_len = get_line_length(E.cy);
                if (E.cx > line_len) E.cx = line_len;
            }
            break;
            
        case HOME_KEY:
            E.cx = 0;
            break;
            
        case END_KEY:
            E.cx = get_line_length(E.cy);
            break;
            
        case PAGE_UP:
            E.cy = E.rowoff;
            for (int i = 0; i < E.screenrows - 2; i++) {
                if (E.cy > 0) E.cy--;
            }
            break;
            
        case PAGE_DOWN:
            E.cy = E.rowoff + E.screenrows - 2;
            if (E.cy > total_rows - 1) E.cy = total_rows - 1;
            for (int i = 0; i < E.screenrows - 2; i++) {
                if (E.cy < total_rows - 1) E.cy++;
            }
            break;
    }
}

/* -------- editor operations -------- */

void editorInsertChar(char c) {
    int pos = rowcol_to_pos(E.cy, E.cx);
    gap_move(&g, pos);
    gap_insert(&g, c);
    history_push(&E.history, EDIT_INSERT, pos, c);
    E.cx++;
    E.dirty = 1;
}

void editorInsertNewline(void) {
    int pos = rowcol_to_pos(E.cy, E.cx);
    gap_move(&g, pos);
    gap_insert(&g, '\n');
    history_push(&E.history, EDIT_INSERT_NEWLINE, pos, '\n');
    
    // Auto-indent: match previous line's indentation
    int prev_indent = get_line_indent(E.cy);
    E.cy++;
    E.cx = 0;
    
    for (int i = 0; i < prev_indent; i++) {
        gap_insert(&g, ' ');
        history_push(&E.history, EDIT_INSERT, pos + 1 + i, ' ');
        E.cx++;
    }
    
    E.dirty = 1;
}

void editorDelChar(void) {
    if (E.cx > 0) {
        int pos = rowcol_to_pos(E.cy, E.cx);
        gap_move(&g, pos);
        char ch = gap_char_at(&g, pos - 1);
        if (gap_backspace(&g)) {
            history_push(&E.history, EDIT_DELETE, pos - 1, ch);
            E.cx--;
            E.dirty = 1;
        }
    } else if (E.cy > 0) {
        int prev_line_len = get_line_length(E.cy - 1);
        int pos = rowcol_to_pos(E.cy, 0);
        gap_move(&g, pos);
        if (gap_backspace(&g)) {
            history_push(&E.history, EDIT_DELETE_NEWLINE, pos - 1, '\n');
            E.cy--;
            E.cx = prev_line_len;
            E.dirty = 1;
        }
    }
}

void editorProcessKeypress(void) {
    int c = editorReadKey();
    
    // Dismiss welcome screen on any keypress
    if (E.show_welcome) {
        E.show_welcome = 0;
        E.statusmsg[0] = '\0';
        return;
    }
    
    int shift_pressed = is_shift_arrow(c);
    int base_key = get_base_key(c);
    
    switch (base_key) {
        case '\x11': // Ctrl-Q
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
            
        case '\x13': // Ctrl-S
            editorSave();
            break;
            
        case '\x1a': // Ctrl-Z (undo)
            if (history_undo(&E.history)) {
                pos_to_rowcol(g.gap_start, &E.cy, &E.cx);
                E.dirty = 1;
            }
            selection_clear(&E.sel);
            break;
            
        case '\x19': // Ctrl-Y (redo)
            if (history_redo(&E.history)) {
                pos_to_rowcol(g.gap_start, &E.cy, &E.cx);
                E.dirty = 1;
            }
            selection_clear(&E.sel);
            break;
            
        case '\x03': // Ctrl-C (copy)
            if (E.sel.active) {
                clipboard_copy(&E.clip, &E.sel);
                snprintf(E.statusmsg, sizeof(E.statusmsg), "Copied %d bytes", E.clip.len);
                selection_clear(&E.sel);
            }
            break;
            
        case '\x16': // Ctrl-V (paste)
            if (E.sel.active) {
                selection_delete(&E.sel);
            }
            clipboard_paste(&E.clip);
            break;
            
        case '\x18': // Ctrl-X (cut)
            if (E.sel.active) {
                clipboard_copy(&E.clip, &E.sel);
                selection_delete(&E.sel);
                snprintf(E.statusmsg, sizeof(E.statusmsg), "Cut %d bytes", E.clip.len);
            }
            break;
            
        case '\x01': // Ctrl-A (select all)
            selection_start(&E.sel, 0, 0);
            E.cy = count_rows() - 1;
            E.cx = get_line_length(E.cy);
            selection_update(&E.sel, E.cy, E.cx);
            snprintf(E.statusmsg, sizeof(E.statusmsg), "Selected all");
            break;
            
        case '\x06': // Ctrl-F (find)
            E.statusmsg[0] = '\0';
            break;
            
        case '\r': // Enter
            if (E.sel.active) {
                selection_delete(&E.sel);
            }
            editorInsertNewline();
            break;
            
        case 127: // Backspace
        case '\x08': // Ctrl-H
            if (E.sel.active) {
                selection_delete(&E.sel);
            } else {
                editorDelChar();
            }
            break;
            
        case DEL_KEY:
            if (E.sel.active) {
                selection_delete(&E.sel);
            } else {
                int pos = rowcol_to_pos(E.cy, E.cx);
                gap_move(&g, pos);
                char ch = gap_char_at(&g, pos);
                if (gap_delete(&g)) {
                    history_push(&E.history, EDIT_DELETE, pos, ch);
                    E.dirty = 1;
                }
            }
            break;
            
        case '\t': // Tab (insert spaces)
            if (E.sel.active) {
                selection_delete(&E.sel);
            }
            for (int i = 0; i < TAB_STOP; i++) {
                editorInsertChar(' ');
            }
            break;
            
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            // Start or extend selection if shift is held
            if (shift_pressed) {
                if (!E.sel.active) {
                    selection_start(&E.sel, E.cy, E.cx);
                }
                editorMoveCursor(base_key);
                selection_update(&E.sel, E.cy, E.cx);
            } else {
                // Clear selection if moving without shift
                if (E.sel.active) {
                    selection_clear(&E.sel);
                }
                editorMoveCursor(base_key);
            }
            break;
            
        case HOME_KEY:
        case END_KEY:
            if (shift_pressed && !E.sel.active) {
                selection_start(&E.sel, E.cy, E.cx);
            }
            editorMoveCursor(base_key);
            if (shift_pressed) {
                selection_update(&E.sel, E.cy, E.cx);
            } else {
                selection_clear(&E.sel);
            }
            break;
            
        case PAGE_UP:
        case PAGE_DOWN:
            if (shift_pressed && !E.sel.active) {
                selection_start(&E.sel, E.cy, E.cx);
            }
            editorMoveCursor(base_key);
            if (shift_pressed) {
                selection_update(&E.sel, E.cy, E.cx);
            } else {
                selection_clear(&E.sel);
            }
            break;
            
        case '\x1b': // Escape - clear selection
            selection_clear(&E.sel);
            E.statusmsg[0] = '\0';
            break;
            
        default:
            if (base_key >= 32 && base_key < 127) {
                if (E.sel.active) {
                    selection_delete(&E.sel);
                }
                editorInsertChar((char)base_key);
            }
            break;
    }
}

/* -------- main -------- */

int main(int argc, char *argv[]) {
    enableRawMode();
    E.cx = E.cy = 0;
    E.rowoff = E.coloff = 0;
    E.filename = NULL;
    E.dirty = 0;
    E.statusmsg[0] = '\0';
    E.search_query = NULL;
    E.search_direction = 1;
    E.search_match_pos = -1;
    E.show_welcome = 0;
    
    history_init(&E.history);
    selection_clear(&E.sel);
    E.clip.data = NULL;
    E.clip.len = 0;
    
    getWindowSize(&E.screenrows, &E.screencols);
    E.screenrows -= 2; // Status bar + message bar
    
    gap_init(&g, 1024);
    
    if (argc >= 2) {
        editorOpen(argv[1]);
        snprintf(E.statusmsg, sizeof(E.statusmsg), 
                 "Ctrl-S=save | Ctrl-Q=quit | Shift+Arrows=select | Ctrl-A=all | Esc=clear");
    } else {
        // Show welcome screen if no file specified
        E.show_welcome = 1;
    }
    
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    
    for (;;) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    
    history_free(&E.history);
    clipboard_free(&E.clip);
    gap_free(&g);
    return 0;
}
