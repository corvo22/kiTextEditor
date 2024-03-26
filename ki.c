#include <ctype.h>	
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT {NULL, 0}
#define KI_VERSION "0.0.1"
#define KI_TAB_STOP 4

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

struct abuf {
    char *b;
	int len;
}; typedef struct abuf abuf;

struct erow {
	int size;
	int rsize;
	char * chars;
	char * render;
}; typedef struct erow erow;

struct editorConfig {
	int screenrows;
	int screencols;
	int cx, cy;
	int rx;
	int numrows;
	int rowoff;
	int coloff;

	erow *row;
	char * filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
}; typedef struct editorConfig editorConfig;

editorConfig E;

// helpers

void err(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);	
	write(STDOUT_FILENO, "\x1b[H", 3);
  	perror(s);
  	exit(1);
}

// add data to abuf structure, a dynamic string buffer
void abAppend(struct abuf *ab, const char *s, int len) {
 	char *new = realloc(ab->b, ab->len + len);
  	if (new == NULL) return;
  	memcpy(&new[ab->len], s, len);
  	ab->b = new;
  	ab->len += len;
}

void abFree(struct abuf *ab) {
	 free(ab->b);
}

// terminal negotiation section

void disableRawMode() {
  	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    	err("tcsetattr");
}

void enableRawMode() {
	struct termios raw = E.orig_termios;
  	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) err("tcgetattr");
  		atexit(disableRawMode);

  	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  	raw.c_oflag &= ~(OPOST);
  	raw.c_cflag |= (CS8);
  	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  	raw.c_cc[VMIN] = 0;
  	raw.c_cc[VTIME] = 1;

  	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) err("tcsetattr");
}

int editorReadKey() {
  	int nread;
  	char c;
  	
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    	if (nread == -1 && errno != EAGAIN) err("read");
 	
	/* if given an escape key, read the next two bytes. If [ means we have
	 * so type of escape sequence, starts with a number then we read another
	 * expecting ~, test if 5 or 6 to determine page up or down. Otherwise, 
	 * check if an arrow key
	 */

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
			}
			else {
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}	
		}
		else if (seq[0] == 'O') {
			switch(seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	} 
  	return c;
}
int getCursorPosition(int *rows, int *cols) {
  	char buf[32];
  	unsigned int i = 0;
  	
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  	
	while (i < sizeof(buf) - 1) {
    	if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    	if (buf[i] == 'R') break;
    	i++;
  	
	}
  
	buf[i] = '\0';
	
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  	
	return 0;  
}


int getWindowSize(int * rows, int * cols) {
	struct winsize ws;
	
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	}
	else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

// row ops{

int editorRowCxToRx(erow * row, int cx) {
	int j;
	int rx = 0;

	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t')
			rx += (KI_TAB_STOP - 1) + (rx % KI_TAB_STOP);
		rx++;
	}
	return rx;
}

void editorUpdateRow(erow * row) {
	int j;
	int idx = 0;
	int tabs = 0;
	
	for (j = 0; j < row->rsize; j++)
		if (row->chars[j] == '\t') tabs++;

	free(row->render);
	row->render = malloc(row->size + tabs * (KI_TAB_STOP - 1) + 1);
	
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
      		row->render[idx++] = ' ';
      		while (idx % KI_TAB_STOP != 0) row->render[idx++] = ' ';
    	} 
		else 
      		row->render[idx++] = row->chars[j];
  	}
		
	row->render[idx] = '\0';
	row->rsize = idx;

}

void editorAppendRow(char * s, size_t len) {
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	int at = E.numrows;

	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
}

// file io

void editorOpen(char * filename) {
	FILE *fp = fopen(filename, "r");
	char * line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	free(E.filename);
	E.filename = strdup(filename);
		
	if (!fp) err("fopen");
	
	while((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen--;
		editorAppendRow(line, linelen);
		
	}
	free(line);
	fclose(fp);
}

// output

void editorScroll() {
	E.rx = 0;
	if (E.cy < E.numrows)
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	if (E.cy < E.rowoff)
		E.rowoff = E.cy;
	if (E.cy >= E.rowoff + E.screenrows)
		E.rowoff = E.cy - E.screenrows + 1;
	if (E.rx < E.coloff)
		E.coloff = E.rx;
	if (E.rx >= E.coloff + E.screencols)
		E.coloff = E.rx - E.screencols + 1;
}

void drawRows(abuf * ab) {
	char welcome[80];
	int welcomelen = snprintf(welcome, sizeof(welcome), "KI editor -- version %s", KI_VERSION);	
	int padding;
	int filerow;

	for(int i = 0; i < E.screenrows; i++) {
		filerow = i + E.rowoff;
		if (filerow >= E.numrows) {
			if (E.numrows == 0 && i == E.screenrows / 3) {
				padding = (E.screencols - welcomelen) / 2;
				if (welcomelen > E.screencols) welcomelen = E.screencols;
				while(padding --) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			
				welcomelen = snprintf(welcome, sizeof(welcome), "Based on Kilo");
			}

			if (E.numrows == 0 && i == (E.screenrows / 3) + 1) {
				padding = (E.screencols - welcomelen) / 2;
				if (welcomelen > E.screencols) welcomelen = E.screencols;
				while(padding --) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			} 
		
			else
				abAppend(ab, "~", 1); 
		}
		else {
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}
		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);
	}
		
}

void drawStatus(abuf * ab) {
  	char status[80], rstatus[80];
  	int len = snprintf(status, sizeof(status), "%.20s - %d lines",
    	E.filename ? E.filename : "[No Name]", E.numrows);
  	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    	E.cy + 1, E.numrows);
  
	abAppend(ab, "\x1b[7m", 4);
	if (len > E.screencols) len = E.screencols;
  	abAppend(ab, status, len);
 	while (len < E.screencols) {
    	if (E.screencols - len == rlen) {
      		abAppend(ab, rstatus, rlen);
      		break;
		}
    	else {
      		abAppend(ab, " ", 1);
      		len++;
		}
    }
  	
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void drawMessageBar(struct abuf *ab) {
  	abAppend(ab, "\x1b[K", 3);
  	int msglen = strlen(E.statusmsg);
  	if (msglen > E.screencols) msglen = E.screencols;
  	if (msglen && time(NULL) - E.statusmsg_time < 5)
   		abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
	abuf ab = ABUF_INIT;

	editorScroll();
	
	abAppend(&ab, "\x1b[?25l", 6);	
	abAppend(&ab, "\x1b[H", 3);		
	
	drawRows(&ab);
	drawStatus(&ab);
	drawMessageBar(&ab);
    
    // add one to convert between c and terminal indexing, resets cursor
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1, E.rx - E.coloff + 1);
    abAppend(&ab, buf, strlen(buf));
	
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  	va_list ap;
  	va_start(ap, fmt);
  	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  	va_end(ap);
 	E.statusmsg_time = time(NULL);
}
// input

void editorMoveCursor(int key) {
	// row = current row or null if off end of file
	erow * row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch(key) {
		case ARROW_LEFT:
      		if (E.cx != 0) {
        		E.cx--;
      		}
      		break;
    	case ARROW_RIGHT:
			if (row && E.cx < row -> size)
        		E.cx++;
      		break;
    	case ARROW_UP:
      		if (E.cy != 0) {
        		E.cy--;
      		}
      		break;
    	case ARROW_DOWN:
      		if (E.cy < E.numrows) {
        		E.cy++;
      		}
      		break;
    }
	// reset row as we could be on a dif row (arrow up/dwn)
	// check if moving up/dwn put us off the end of the new row and correct
	row = (E.cy >= E.numrows) ?NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen) {
		E.cx = rowlen;
	}
}

void editorProcessKeypress() {
  	int c = editorReadKey();

  	switch (c) {
    	case CTRL_KEY('q'):
      		write(STDOUT_FILENO, "\x1b[2J", 4);
    		write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
      		break;

		case HOME_KEY:
      		E.cx = 0;
      		break;
    
		case END_KEY:
			if (E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
      		break;
		
		case PAGE_UP:
    	case PAGE_DOWN:
      	{
			if (c == PAGE_UP)
				E.cy = E.rowoff;
			else if (c == PAGE_DOWN) {
				E.cy = E.rowoff + E.screenrows - 1;
				if (E.cy > E.numrows)
					E.cy = E.numrows;
			}
        	int times = E.screenrows;
        	while (times--)
          		editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      	}
      		break;
	
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
  	}
}

void initEditor() {
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) err("getWindowSize");
	E.screenrows -= 2;
}

int main(int argc, char * argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2)
  	editorOpen(argv[1]);

  editorSetStatusMessage("HELP: Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
