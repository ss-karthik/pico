/* Libraries */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Preprocessing */

#define PICO_VER "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_CONFIRM 2
#define CTRL_KEY(k) ((k) & 0x1f) //ie 31 & 'a' returns 1 => CTRL+a

enum editorKey{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    END_KEY,
    HOME_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* Data */

typedef struct erow {
    char* chars;
    char* render;
    int size;
    int rsize;
}erow;

struct editorConfig{
    int cx, cy;
    int rx; //render row index
    int coloff;
    int rowoff;
    int screenrows;
    int screencols;
    int numrows;
    int dirty;
    erow *row;
    char* filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios og_termios;
};

struct editorConfig E;

/* Prototypes */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char* editorPrompt(char* prompt);


/* Terminal */
void die(const char* s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.og_termios)==-1)
        die("tcsetattr");
}

void enableRawMode(){
    //turn off echoing on terminal
    if (tcgetattr(STDIN_FILENO, &E.og_termios)==-1)
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.og_termios;
    //turn off canonical mode - going to raw mode - negating all the in, out, control and local flags
    raw.c_iflag &= ~(BRKINT| ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // These are used to set a 10ms Timeout for any command reading character from terminal. If no character read in 10ms, read from main function returns i.e. c = 0
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)==-1)
        die("tcsetattr");
}

int editorReadKey(){
    char ch;
    int nread;
    while((nread = read(STDIN_FILENO, &ch, 1))!=1){
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if(ch=='\x1b'){
        char seq[3];

        if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if(seq[0]=='['){
            if(seq[1]>='0' && seq[1]<='9'){
                if(read(STDIN_FILENO, &seq[2], 1)!=1) return '\x1b';
                if(seq[2]=='~'){
                    switch(seq[1]){
                        case '3': return DEL_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else {
                switch(seq[1]){
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if(seq[0]=='O'){
            switch(seq[1]){
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return ch;
    }
}

int getCursorPosition(int* rows, int* cols){
    if(write(STDOUT_FILENO, "\x1b[6n", 4)!=4) return -1;
    char buf[32];
    unsigned int i=0;
    while(i<sizeof(buf)-1){
        if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if(buf[i]=='R') break;
        i++;
    }
    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1]!='[') return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols)!=2) return -1;
    return 0;
}

int getWindowSize(int* rows, int* cols){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)==-1 || ws.ws_col == 0){
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else{
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/* Text Row Operations */
void editorUpdateRow(erow* row){
    int tabs = 0;
    for(int i=0; i<row->size; i++){
        if(row->chars[i] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    //adding 7 more chars since we are printing tabs as spaces
    row->render = malloc(row->size + tabs*KILO_TAB_STOP + 1);


    int j = 0;
    for(int i=0; i<row->size; i++){
        if(row->chars[i]=='\t'){
            row->render[j++] = ' ';
            while(j%KILO_TAB_STOP != 0) row->render[j++] = ' ';
        } else {
            row->render[j++] = row->chars[i];
        }
    }
    row->render[j] = '\0';
    row->rsize = j;
}

void editorInsertRow(int t, char* s, size_t len){
    if(t<0 || t>E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows+1));
    memmove(&E.row[t+1], &E.row[t], sizeof(erow)*(E.numrows - t));

    E.row[t].size = len;
    E.row[t].chars = malloc(len+1);
    memcpy(E.row[t].chars, s, len);
    E.row[t].chars[len] = '\0';

    E.row[t].render = NULL;
    E.row[t].rsize = 0;
    editorUpdateRow(&E.row[t]);

    E.numrows++;
    E.dirty++;
}

int editorRowCxToRx(erow* row, int cx){
    int rx = 0;
    for(int i=0; i<cx; i++){
        if(row->chars[i]=='\t'){
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow* row, int rx){
    int cur_rx = 0;
    int cx;
    for(cx=0; cx<row->size; cx++){
        if(row->chars[cx] == '\t'){
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        }
        cur_rx++;
        if(cur_rx>rx) return cx;
    }
    return cx;
}

void editorRowInsertChar(erow* row, int idx, int c){
    if(idx<0 || idx>row->size) idx = row->size;
    row->chars = realloc(row->chars, row->size+2);
    memmove(&row->chars[idx+1],&row->chars[idx], row->size - idx + 1);
    row->size++;
    row->chars[idx] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow* row, int t){
    if(t<0 && t>=row->size) return;
    memmove(&row->chars[t], &row->chars[t+1], row->size-t);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

void editorFreeRow(erow* row){
    free(row->render);
    free(row->chars);
}

void editorRowAppendString(erow* row, char* s, size_t len){
    row->chars = realloc(row->chars, row->size+len+1);
    memcpy(&row->chars[row->size], s, len);
    row->size+=len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorDelRow(int t){
    if(t<0 || t==E.numrows) return;
    editorFreeRow(&E.row[t]);
    memmove(&E.row[t], &E.row[t+1], sizeof(erow) * (E.numrows-t-1));
    E.numrows--;
    E.dirty++;
}


/* Editor Operations */
void editorInsertChar(int c){
    if(E.cy == E.numrows){
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewLine(){
    if(E.cx==0){
        editorInsertRow(E.cy, "", 0);
    } else {
        erow* row = &E.row[E.cy];
        editorInsertRow(E.cy+1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx=0;
}

void editorDelChar(){
    if(E.cy == E.numrows) return;
    if(E.cx==0 && E.cy==0) return;

    erow* row = &E.row[E.cy];
    if(E.cx>0){
        editorRowDelChar(row, E.cx-1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy-1].size;
        editorRowAppendString(&E.row[E.cy-1], row->chars , row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}



/* File I/O */
char* editorRowsToString(int* buflen){
    int totlen = 0;
    for(int i=0; i<E.numrows; i++){
        totlen+=E.row[i].size + 1;
    }
    *buflen = totlen;

    char* buff = malloc(totlen);
    char* p = buff;
    for(int i=0; i<E.numrows; i++){
        memcpy(p, E.row[i].chars, E.row[i].size);
        p += E.row[i].size;
        *p = '\n';
        p++;
    }
    return buff;
}

void editorOpen(char* filename){
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line, &linecap, fp))!=-1){
        while(linelen>0 && (line[linelen-1]=='\n' || line[linelen-1]=='\r')){
            linelen--;
        }
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave(){
    if(E.filename == NULL) {
        E.filename = editorPrompt("Save As: %s (ESC To Cancel)");
        if(E.filename==NULL){
            editorSetStatusMessage("Save Aborted.");
            return;
        }
    }
    int len;
    char* buff = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd!=-1){
        if(ftruncate(fd, len) != -1){
            if(write(fd, buff, len) == len){
                close(fd);
                free(buff);
                editorSetStatusMessage("%d bytes written to disk", len);
                E.dirty = 0;
                return;
            }
        }
        close(fd);
    }
    free(buff);
    editorSetStatusMessage("Cannot Save! I/0 Error: %s", strerror(errno));
}

/* Find */
void editorFind(){
    char* query = editorPrompt("Search: %s (ESC to cancel)");
    if(query==NULL) return;
    for(int i=0; i<E.numrows; i++){
        erow* row = &E.row[i];
        char* match = strstr(row->render, query);
        if(match){
            E.cy = i;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;
            break;
        }
    }
    free(query);
}

/* Append Buffer */

struct abuf{
    char* str;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf* ab, const char* s, int len){
    char *new = realloc(ab->str, ab->len+len);
    if(new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->str = new;
    ab->len += len;
}

void abFree(struct abuf* ab){
    free(ab->str);
}


/* Output */
void editorScroll(){
    E.rx = 0;
    if(E.cy < E.numrows){
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if(E.cy < E.rowoff){
        E.rowoff = E.cy;
    }
    if(E.cy >= E.rowoff + E.screenrows){
        E.rowoff = E.cy - E.screenrows + 1;
    }

    if(E.rx < E.coloff){
        E.coloff = E.rx;
    }
    if(E.rx >= E.coloff+E.screencols){
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab){
    for(int i=0; i<E.screenrows; i++){
        int filerow = i + E.rowoff;
        if(filerow>=E.numrows){
            if(E.numrows==0 && i== E.screenrows/3){
                char message[80];
                int msglen = snprintf(message, sizeof(message), "Pico -- Light as a Feather -- Version %s", PICO_VER);
                if(msglen > E.screencols) msglen = E.screencols;

                int padding = (E.screencols - msglen)/2;
                if(padding){
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding-->0){
                    abAppend(ab, " ", 1);
                }

                abAppend(ab, message, msglen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if(len<0) len = 0;
            if(len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab, "\x1b[7m", 4); //invert colors

    char statusline[80];
    char rstatus[80];
    int len = snprintf(statusline, sizeof(statusline),
        "%s - %d lines %s",
        E.filename?E.filename:"[No Name]",
        E.numrows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d / %d", E.cy+1, E.numrows);

    if(len>E.screencols) len = E.screencols;
    abAppend(ab, statusline, len);

    while(len<E.screencols){
        if(E.screencols-len == rlen){
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3); //back to normal colors
    abAppend(ab, "\r\n", 2);
}

void editorDrawStatusMessage(struct abuf *ab){
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if(msglen>E.screencols) msglen = E.screencols;
    if(msglen && time(NULL)-E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawStatusMessage(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy-E.rowoff)+1, (E.rx-E.coloff)+1);
    //who tf thought it was a good idea to write cursor escape sequence as y;x instead of x;y
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.str, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...){
    //variadic function - can have any no. of args
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}



/* Input */
char* editorPrompt(char* prompt){
    size_t bufsize = 128;
    char* buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if(buflen!=0) buf[--buflen] = '\0';
        }
        else if (c=='\x1b') {
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        }
        else if(c == '\r'){
            if(buflen!=0){
                editorSetStatusMessage("");
                return buf;
            }
        } else if(!iscntrl(c) && c<128) {
            if(buflen == bufsize-1){
                bufsize*=2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

void editorMoveCursor(int key){
    switch(key){
        case ARROW_UP:
            if(E.cy!=0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows)
                E.cy++;
            break;
        case ARROW_LEFT:
            if(E.cx!=0)
                E.cx--;
            break;
        case ARROW_RIGHT:
            E.cx++;
            break;
    }
}

void editorProcessKeypress(){
    static int quit_times = KILO_QUIT_CONFIRM;
    int ch = editorReadKey();
    switch(ch){
        case '\r':
            editorInsertNewLine();
            break;
        case CTRL_KEY('q'):
            if(E.dirty && quit_times>0){
                editorSetStatusMessage("WARNING! File Has Unsaved Changes. Press CTRL-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case ARROW_UP:
        case ARROW_LEFT:
        case ARROW_DOWN:
        case ARROW_RIGHT:
            editorMoveCursor(ch);
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if(ch==PAGE_UP){
                    E.cy = E.rowoff;
                } else if (ch == PAGE_DOWN){
                    E.cy = E.rowoff + E.screenrows - 1;
                    if(E.cy>E.screenrows) E.cy = E.screenrows;
                }

                int times = E.screenrows;
                while(times--!=0){
                    if(ch==PAGE_UP){
                        editorMoveCursor(ARROW_UP);
                    } else {
                        editorMoveCursor(ARROW_DOWN);
                    }
                }
            }
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if(E.cy<E.numrows)
                E.cx = E.row[E.cy].size;
            break;
        case CTRL_KEY('f'):
            editorFind();
            break;

        case DEL_KEY:
        case CTRL_KEY('h'):
        case BACKSPACE:
            if(ch==DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        default:
            editorInsertChar(ch);
            break;
    }
    quit_times = KILO_QUIT_CONFIRM;
}





/* Init */
void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.dirty = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    if(getWindowSize(&E.screenrows, &E.screencols)==-1) die("getWindowSize");
    E.screenrows -= 2; //status bar and status msg space
}

int main(int argc, char* argv[]){
    enableRawMode();
    initEditor();
    if(argc>=2){
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    };

    return 0;
}
