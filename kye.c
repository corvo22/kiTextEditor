#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT {NULL, 0}
#define KYE_VERSION "0.0.1"

struct abuf {
	char *b;
	int len;
}; typedef struct abuf abuf;

struct editorConfig {
	int screenrows;
	int screencols;

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

char editorReadKey() {
  	int nread;
  	char c;
  	
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    	if (nread == -1 && errno != EAGAIN) err("read");
  
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

// output

void drawEmptyRows(abuf * ab) {
	char welcome[80];
	int welcomelen = snprintf(welcome, sizeof(welcome), "KYE editor -- version %s", KYE_VERSION);	
	int padding;

	for(int i = 0; i < E.screenrows - 1; i++) {
		if (i == E.screenrows / 3) {
			padding = (E.screencols - welcomelen) / 2;
			if (welcomelen > E.screencols) welcomelen = E.screencols;
			while(padding --) abAppend(ab, " ", 1);
			abAppend(ab, welcome, welcomelen);
			
			welcomelen = snprintf(welcome, sizeof(welcome), "Based on Kilo");
		}

		if (i == (E.screenrows / 3) + 1) {
			padding = (E.screencols - welcomelen) / 2;
			if (welcomelen > E.screencols) welcomelen = E.screencols;
			while(padding --) abAppend(ab, " ", 1);
			abAppend(ab, welcome, welcomelen);
		} 
		
		else
			abAppend(ab, "~", 1); 
		
		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);
	}
	abAppend(ab, "~", 1);
		
}

void editorRefreshScreen() {
	abuf ab = ABUF_INIT;
	
	abAppend(&ab, "\x1b[?25l", 6);	
	abAppend(&ab, "\x1b[H", 3);		
	
	drawEmptyRows(&ab);
	
	abAppend(&ab, "\x1b[H", 3);		
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

// input

void editorProcessKeypress() {
  	char c = editorReadKey();

  	switch (c) {
    	case CTRL_KEY('q'):
      		write(STDOUT_FILENO, "\x1b[2J", 4);
    		write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
      		break;
  	}
}

void initEditor() {
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) err("getWindowSize");
}

int main() {
  enableRawMode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
