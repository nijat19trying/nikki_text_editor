#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <sys/select.h>
#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_BUF 1000000
#define MAX_SEARCH 256
#define ARROW_LEFT 1000
#define ARROW_RIGHT 1001
#define ARROW_UP 1002
#define ARROW_DOWN 1003
#define DEL_KEY 1004
#define HOME_KEY 1005
#define END_KEY 1006
#define PAGE_UP 1007
#define PAGE_DOWN 1008
#define BACKSPACE 127
#define ESC 27
#define MOUSE_CLICK 2000
struct termios orig_termios;
char buffer[MAX_BUF];
int buf_len = 0;
char filename[256];
int modified = 0;
struct winsize ws;
int cx = 0;
int cy = 0;
int rowoff = 0;
int coloff = 0;
char search_query[MAX_SEARCH] = {0};
int search_len = 0;
int search_mode = 0;
int match_start = -1;
int match_end = -1;
void die(const char *msg) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(msg);
    exit(1);
}
void disableRawMode() {
    write(STDOUT_FILENO, "\x1b[?1000l", 9);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}
void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    write(STDOUT_FILENO, "\x1b[?1000h", 9);
}
void getWindowSize() {
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        ws.ws_row = 24;
        ws.ws_col = 80;
    }
}
void loadFile(const char *name) {
    int fd = open(name, O_RDONLY);
    if (fd == -1) return;
    buf_len = read(fd, buffer, MAX_BUF - 1);
    if (buf_len < 0) buf_len = 0;
    close(fd);
    cx = cy = rowoff = coloff = 0;
    modified = 0;
}
void saveFile() {
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return;
    write(fd, buffer, buf_len);
    close(fd);
    modified = 0;
}
int editorGetNumRows() {
    int rows = 1;
    for (int i = 0; i < buf_len; i++) {
        if (buffer[i] == '\n') rows++;
    }
    return rows;
}
int getRowLength(int row) {
    int offset = 0;
    int r = 0;
    while (r < row && offset < buf_len) {
        if (buffer[offset++] == '\n') r++;
    }
    int len = 0;
    while (offset + len < buf_len && buffer[offset + len] != '\n') len++;
    return len;
}
int editorCursorToPos(int row, int col) {
    int pos = 0;
    for (int r = 0; r < row; r++) {
        pos += getRowLength(r) + 1;
    }
    pos += col;
    return pos;
}
void bufferPosToCursor(int pos, int *row, int *col) {
    *row = 0;
    *col = 0;
    for (int i = 0; i < pos; i++) {
        if (buffer[i] == '\n') {
            (*row)++;
            *col = 0;
        } else {
            (*col)++;
        }
    }
}
void editorMoveCursor(int key) {
    int rowlen = getRowLength(cy);
    int numrows = editorGetNumRows();
    switch (key) {
        case ARROW_LEFT:
            if (cx > 0) cx--;
            else if (cy > 0) {
                cy--;
                cx = getRowLength(cy);
            }
            break;
        case ARROW_RIGHT:
            if (cx < rowlen) cx++;
            else if (cy < numrows - 1) {
                cy++;
                cx = 0;
            }
            break;
        case ARROW_UP:
            if (cy > 0) cy--;
            break;
        case ARROW_DOWN:
            if (cy < numrows - 1) cy++;
            break;
        case HOME_KEY:
            cx = 0;
            break;
        case END_KEY:
            cx = getRowLength(cy);
            break;
        case PAGE_UP:
            cy -= ws.ws_row - 1;
            if (cy < 0) cy = 0;
            break;
        case PAGE_DOWN:
            cy += ws.ws_row - 1;
            if (cy >= numrows) cy = numrows - 1;
            break;
    }
    rowlen = getRowLength(cy);
    if (cx > rowlen) cx = rowlen;
    if (cy < rowoff) rowoff = cy;
    if (cy >= rowoff + ws.ws_row - 1) rowoff = cy - (ws.ws_row - 1) + 1;
    if (cx < coloff) coloff = cx;
    if (cx >= coloff + ws.ws_col) coloff = cx - ws.ws_col + 1;
}
void reset_search() {
    search_len = 0;
    search_query[0] = '\0';
    match_start = match_end = -1;
}
int findNextMatch(int start) {
    if (search_len == 0) return -1;
    for (int i = start; i <= buf_len - search_len; i++) {
        if (memcmp(&buffer[i], search_query, search_len) == 0) {
            return i;
        }
    }
    return -1;
}
int editorReadKey() {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) die("read");
    if (c == '\x1b') {
        char seq[6];
        struct timeval tv = {0L, 0L};
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(STDIN_FILENO, &fdset);
        if (select(1, &fdset, NULL, NULL, &tv) != 1) return ESC;
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return ESC;
        if (seq[0] == '[') {
            FD_ZERO(&fdset);
            FD_SET(STDIN_FILENO, &fdset);
            if (select(1, &fdset, NULL, NULL, &tv) != 1) return ESC;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return ESC;
            if (seq[1] == 'M') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return ESC;
                if (read(STDIN_FILENO, &seq[3], 1) != 1) return ESC;
                if (read(STDIN_FILENO, &seq[4], 1) != 1) return ESC;
                char button = seq[2] - 32;
                int mx = seq[3] - 32 - 1;
                int my = seq[4] - 32 - 1;
                if (button == 0) {
                    int target_cy = rowoff + my;
                    int target_cx = coloff + mx;
                    int numrows = editorGetNumRows();
                    if (target_cy >= numrows) target_cy = numrows - 1;
                    if (target_cy < 0) target_cy = 0;
                    cy = target_cy;
                    int rowlen = getRowLength(cy);
                    if (target_cx > rowlen) target_cx = rowlen;
                    cx = target_cx;
                    if (cy < rowoff) rowoff = cy;
                    if (cy >= rowoff + ws.ws_row - 1) rowoff = cy - (ws.ws_row - 1) + 1;
                    if (cx < coloff) coloff = cx;
                    if (cx >= coloff + ws.ws_col) coloff = cx - ws.ws_col + 1;
                } else if (button == 64) {
                    rowoff -= 3;
                    if (rowoff < 0) rowoff = 0;
                } else if (button == 65) {
                    int numrows = editorGetNumRows();
                    int screen_rows = ws.ws_row - 1;
                    rowoff += 3;
                    if (rowoff > numrows - screen_rows) rowoff = numrows - screen_rows;
                    if (rowoff < 0) rowoff = 0;
                }
                return MOUSE_CLICK;
            } else if (seq[1] >= '0' && seq[1] <= '9') {
                FD_ZERO(&fdset);
                FD_SET(STDIN_FILENO, &fdset);
                if (select(1, &fdset, NULL, NULL, &tv) != 1) return ESC;
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return ESC;
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
            FD_ZERO(&fdset);
            FD_SET(STDIN_FILENO, &fdset);
            if (select(1, &fdset, NULL, NULL, &tv) != 1) return ESC;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return ESC;
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return ESC;
    } else {
        return c;
    }
}
void refreshScreen() {
    getWindowSize();
    write(STDOUT_FILENO, "\x1b[?25l", 6);
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    int screen_rows = ws.ws_row - 1;
    int y;
    for (y = 0; y < screen_rows; y++) {
        int filerow = rowoff + y;
        if (filerow >= editorGetNumRows()) {
            write(STDOUT_FILENO, "~", 1);
        } else {
            int rowlen = getRowLength(filerow);
            int start_col = coloff;
            int chars_to_draw = ws.ws_col;
            int buf_index = 0;
            int crow = 0;
            while (crow < filerow && buf_index < buf_len) {
                if (buffer[buf_index++] == '\n') crow++;
            }
            buf_index += start_col;
            int drew = 0;
            while (drew < chars_to_draw && buf_index < buf_len && buffer[buf_index] != '\n') {
                char ch = buffer[buf_index];
                if (match_start >= 0 && buf_index >= match_start && buf_index < match_end) {
                    write(STDOUT_FILENO, "\x1b[7m", 4);
                }
                write(STDOUT_FILENO, &ch, 1);
                if (match_start >= 0 && buf_index >= match_start && buf_index < match_end) {
                    write(STDOUT_FILENO, "\x1b[m", 3);
                }
                buf_index++;
                drew++;
            }
        }
        write(STDOUT_FILENO, "\x1b[K", 3);
        if (y < screen_rows - 1) write(STDOUT_FILENO, "\r\n", 2);
    }
    write(STDOUT_FILENO, "\x1b[J", 3);
    char cmd[20];
    snprintf(cmd, sizeof(cmd), "\x1b[%d;1H", ws.ws_row);
    write(STDOUT_FILENO, cmd, strlen(cmd));
    write(STDOUT_FILENO, "\x1b[2K", 4);
    char status[300];
    char *statusmsg;
    if (search_mode) {
        statusmsg = search_query;
        snprintf(status, sizeof(status),
                 "[SEARCH] %s (Esc to exit, Enter = next) ", statusmsg);
    } else {
        snprintf(status, sizeof(status),
                 "NIKKI %s%s | Ctrl+F Find | Ctrl+S Save | Ctrl+Q Quit",
                 filename, modified ? " [*]" : "");
    }
    write(STDOUT_FILENO, "\x1b[7m", 4);
    write(STDOUT_FILENO, status, strlen(status));
    char clearrest[32];
    snprintf(clearrest, sizeof(clearrest), "%*s", (int)(ws.ws_col - strlen(status)), "");
    write(STDOUT_FILENO, clearrest, strlen(clearrest));
    write(STDOUT_FILENO, "\x1b[m", 3);
    int screen_cy = cy - rowoff + 1;
    int screen_cx = cx - coloff + 1;
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", screen_cy, screen_cx);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\x1b[?25h", 6);
}
void editorDeleteLine() {
    int numrows = editorGetNumRows();
    if (numrows == 0) return;
    int start = editorCursorToPos(cy, 0);
    int len = getRowLength(cy);
    int end = start + len;
    int remove_nl = (end < buf_len && buffer[end] == '\n') ? 1 : 0;
    memmove(&buffer[start], &buffer[end + remove_nl], buf_len - (end + remove_nl));
    buf_len -= (len + remove_nl);
    modified = 1;
    numrows = editorGetNumRows();
    if (cy >= numrows) {
        cy = numrows - 1;
        if (cy < 0) cy = 0;
    }
    cx = 0;
    if (cy < rowoff) rowoff = cy;
    if (cy >= rowoff + ws.ws_row - 1) rowoff = cy - (ws.ws_row - 1) + 1;
}
void editor() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    enableRawMode();
    getWindowSize();
    while (1) {
        refreshScreen();
        int c = editorReadKey();
        if (c == MOUSE_CLICK) continue;
        if (search_mode) {
            if (c == ESC || c == CTRL_KEY('f')) {
                search_mode = 0;
                reset_search();
            } else if (c == BACKSPACE) {
                if (search_len > 0) {
                    search_len--;
                    search_query[search_len] = '\0';
                    match_start = match_end = -1;
                }
            } else if (c == '\r') {
                int start = (match_start >= 0) ? match_start + search_len : 0;
                int pos = findNextMatch(start);
                if (pos == -1) pos = findNextMatch(0);
                if (pos >= 0) {
                    match_start = pos;
                    match_end = pos + search_len;
                    int row, col;
                    bufferPosToCursor(pos, &row, &col);
                    cy = row;
                    cx = col;
                    rowoff = (cy - (ws.ws_row - 1) / 3 < 0) ? 0 : cy - (ws.ws_row - 1) / 3;
                }
            } else if (isprint(c) && search_len < MAX_SEARCH - 1) {
                search_query[search_len++] = c;
                search_query[search_len] = '\0';
                match_start = match_end = -1;
            }
            continue;
        }
        switch (c) {
            case CTRL_KEY('q'):
                if (modified) {
                    write(STDOUT_FILENO, "\x1b[2K", 4);
                    write(STDOUT_FILENO, "Save changes? (y/n) ", 20);
                    int qc;
                    while ((qc = editorReadKey())) {
                        if (tolower(qc) == 'y') {
                            saveFile();
                            break;
                        }
                        if (tolower(qc) == 'n') break;
                    }
                }
                write(STDOUT_FILENO, "\x1b[2J", 4);
                write(STDOUT_FILENO, "\x1b[H", 3);
                return;
            case CTRL_KEY('s'):
                saveFile();
                break;
            case CTRL_KEY('f'):
                search_mode = 1;
                reset_search();
                break;
            case CTRL_KEY('d'):
                editorDeleteLine();
                break;
            case BACKSPACE: {
                if (cx == 0 && cy == 0) break;
                int pos = editorCursorToPos(cy, cx);
                memmove(&buffer[pos - 1], &buffer[pos], buf_len - pos);
                buf_len--;
                editorMoveCursor(ARROW_LEFT);
                modified = 1;
                break;
            }
            case DEL_KEY: {
                if (cx == getRowLength(cy) && cy == editorGetNumRows() - 1) break;
                int pos = editorCursorToPos(cy, cx);
                memmove(&buffer[pos], &buffer[pos + 1], buf_len - pos - 1);
                buf_len--;
                modified = 1;
                break;
            }
            case '\r': {
                int pos = editorCursorToPos(cy, cx);
                memmove(&buffer[pos + 1], &buffer[pos], buf_len - pos);
                buffer[pos] = '\n';
                buf_len++;
                cy++;
                cx = 0;
                modified = 1;
                if (cy >= rowoff + ws.ws_row - 1) rowoff = cy - (ws.ws_row - 1) + 1;
                break;
            }
            case ARROW_LEFT:
            case ARROW_RIGHT:
            case ARROW_UP:
            case ARROW_DOWN:
            case HOME_KEY:
            case END_KEY:
            case PAGE_UP:
            case PAGE_DOWN:
                editorMoveCursor(c);
                break;
            default: {
                if (isprint(c) || c == '\t') {
                    int pos = editorCursorToPos(cy, cx);
                    memmove(&buffer[pos + 1], &buffer[pos], buf_len - pos);
                    buffer[pos] = c;
                    buf_len++;
                    cx++;
                    modified = 1;
                }
                break;
            }
        }
    }
}
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: nikki <filename>\n");
        return 1;
    }
    strncpy(filename, argv[1], sizeof(filename) - 1);
    loadFile(filename);
    editor();
    return 0;
}