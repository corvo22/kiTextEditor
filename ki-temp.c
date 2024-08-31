/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define _BSD_SOURCE
#define _GNU_SOURCE
#define BUFF_SIZE 1000
#define KI_VERSION "0.0.4"
#define KI_TAB_STOP 4
#define KI_QUIT_TIMES 1
#define ABUF_INIT {NULL, 0}
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
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

/*** data ***/

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

struct Node {
  int newline;
  int buffer;
  struct Node * next;
  struct Node * prev;
}; typedef struct Node Node;

struct Piece {
	int start;
	int end;
	int bufferType;
	struct Piece * next;
	struct Piece * prev;
}; typedef struct Piece Piece;

struct PieceTable {
	int bufferIndex;
  int addIndex;
  int rowX;
  int rowY;
	char * original;
	char * added;
  Node * root;
  Node * current;
	Piece * head;
	Piece * tail;
}; typedef struct PieceTable PieceTable;

struct editorConfig {
  int location;
  int max_location;
	int screenrows;
	int screencols;
	int cx, cy;
	int rx;
	int numrows;
	int rowoff;
	int coloff;
	unsigned int dirty : 1;
	unsigned int edit : 1;

	erow * row;
	erow * cutRow;
	char * filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
  PieceTable * PT;
}; typedef struct editorConfig editorConfig;

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

/*** terminal ***/

void err(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

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
  char seq[3];
  
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) err("read");
  }

  if (c == '\x1b') {

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
  } else {
    return c;
  }
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

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** PieceTable Manipulation ***/

Piece * getNewPieceNode(int start, int end, int bufferType) {
	Piece * piece = (Piece *)malloc(sizeof(Piece));
	piece->start = start;
	piece->end = end;
	piece->bufferType = bufferType;

	return piece;
}

PieceTable * initPieceTable(char * filename) {
	Piece * newNode;
	PieceTable * PT = (PieceTable *)malloc(sizeof(PieceTable));
	PT->head = getNewPieceNode(-1,-1,0);
	PT->tail = getNewPieceNode(-1,-1,0);
	PT->added = (char *)malloc(BUFF_SIZE * sizeof(char));	
  PT->root = (Node *)malloc(sizeof(Node));
  PT->current = PT->root;
  PT->root->newline = 0;
  PT->rowX = 0;
  PT->rowY = 0;
	PT->bufferIndex = 0;
  PT->addIndex = -1;

	FILE *fp = fopen(filename, "r");
  if (fp) {
		fseek(fp, 0, SEEK_END);
		long originalSize = ftell(fp);
		fseek(fp, 0, SEEK_SET);
	
    E.max_location = originalSize;
		char * originalBuffer = (char *) malloc (sizeof(char) * originalSize);
		if (originalBuffer) {
			fread(originalBuffer, 1, originalSize, fp);
		}
		fclose(fp);
	
		PT->original = (char *)malloc(originalSize);
		newNode = getNewPieceNode(0, originalSize - 1, 0);
	}
	else {
		PT->original = "";
		newNode = getNewPieceNode(0, 0, 0);
	}

	PT->head->next = newNode;
	PT->tail->prev = newNode;
	newNode->next = PT->tail;
	newNode->prev = PT->head;	

	return PT;
}

void insertNewLine() {
  E.location++;
  E.max_location++;
  E.PT->rowY++;
  E.PT->rowX = 0;

  Node * newnode = (Node *)malloc(sizeof(Node));
  newnode->newline = E.PT->addIndex;
  newnode->prev = E.PT->current;
  newnode->buffer = 1;
  if(E.PT->current->next) {
    newnode->next = E.PT->current->next;
    E.PT->current->next->prev = newnode;
  }
  else {
    newnode->next = NULL;
  }
  
  E.PT->current->next = newnode;
  E.PT->current = newnode;
}

void insertChar(int pos, char character, PieceTable ** PT) {
	FILE * fp;
	if(pos < 0)
		err ("invalid insertion location");
	fp = fopen("debug", "w");
	fprintf(fp, "Position: %d", pos);
	fclose(fp);
	
	Piece * curr = (*PT)->head->next;
  int unified_bot_range = 0;
  int unified_top_range = 0;

	while(curr != NULL) {
		// inserting on boundary
    unified_bot_range = unified_top_range;
    unified_top_range = unified_top_range + curr->end - curr->start;

		if(pos == unified_bot_range) {
      // inserting on front boundary always requires creating a new add node
      (*PT)->addIndex += 1;
      Piece * newNode = getNewPieceNode((*PT)->addIndex, (*PT)->addIndex+1, 1);
      newNode->next = curr;
      newNode->prev = curr->prev;
      curr->prev->next = newNode;
      curr->prev = newNode;
      (*PT)->added[(*PT)->addIndex] = character;
      return;
    }
    if(pos == unified_top_range) {
      // inserting on end boundary allows extending the existing node if current end is one less
      // than addIndex and both are add nodes
      (*PT)->addIndex = (*PT)->addIndex+1;
      if(curr->bufferType == 1 && curr->end == (*PT)->addIndex) {
        curr->end++;
        (*PT)->added[(*PT)->addIndex] = character;
        return;
      }
      else {
        Piece * newNode = getNewPieceNode((*PT)->addIndex, (*PT)->addIndex+1, 1);
        newNode->next = curr->next;
        newNode->prev = curr;
        curr->next->prev = newNode;
        curr->next = newNode;
        (*PT)->added[(*PT)->addIndex] = character;
        return;
      }

    }
		// inserting into the middle of a Piece, need to split
		else if(pos < unified_top_range && pos > unified_bot_range) {
      (*PT)->addIndex = (*PT)->addIndex+1;

      Piece * back = getNewPieceNode(curr->start, curr->end - (unified_top_range-pos), curr->bufferType);
      Piece * middle = getNewPieceNode((*PT)->addIndex, (*PT)->addIndex+1, 1);
      Piece * front = getNewPieceNode( curr->end - (unified_top_range-pos), curr->end, curr->bufferType);
			
      back->next = middle;
      back->prev = curr->prev;
      middle->next = front;
      middle->prev = back;
      front->next = curr->next;
      front->prev = middle;
      
      curr->prev->next = back;
			curr->next->prev = front;
			curr->prev = NULL;
			curr->next = NULL;
      free(curr);
			(*PT)->added[(*PT)->addIndex] = character;
			return;	
		}
		else
			curr = curr->next;
	}

  if (character == '\n') {
    insertNewLine();
  }

}

void deleteChar(int pos, PieceTable ** PT) {
  // three situations, delete last char in node, delete edge of node, delete mid of node
  Piece * curr = (*PT)->head->next;
  int unified_bot_range = 0;
  int unified_top_range = 0;
  
  if (pos == 0) return;
  
  while(curr != NULL) {
    unified_bot_range = unified_top_range;
    unified_top_range = unified_top_range + curr->end - curr->start;

    // Deleting off front of node
    if(pos - 1 == unified_bot_range) {
      curr->start = curr->start + 1;
      // delete whole node
      if (curr->start == curr->end) {
        curr->prev->next = curr->next;
        curr->next->prev = curr->prev;
        curr->next = NULL;
        curr->prev = NULL;
        free(curr);
      }
      return;
    }
    // deleting off end
    else if (pos == unified_top_range)
    {
      curr->end = curr->end - 1;
      // delete whole node
      if (curr->start == curr->end) {
        curr->prev->next = curr->next;
        curr->next->prev = curr->prev;
        curr->next = NULL;
        curr->prev = NULL;
        free(curr);
      }
      return;
    }
    // deleting middle, requires splitting node,but not creation of a new one
    else if(pos < unified_top_range && pos > unified_bot_range) {
      Piece * back = getNewPieceNode(curr->start, curr->end - (unified_top_range-pos), curr->bufferType);
      Piece * front = getNewPieceNode(curr->end - (unified_top_range-pos) + 1, curr->end, curr->bufferType);

      back->prev = curr->prev;
      back->next = front;
      front->prev = back;
      front->next = curr->next;
      curr->next->prev = front;
      curr->prev->next = back;

      curr->next = NULL;
      curr->prev = NULL;
      free(curr);
      return;
    }
    
    else
			curr = curr->next;
  }

}

/*** row operations ***/

int editorRowXToRx(Node * row, int rowX) {
  int rx = 0;
  int j;
  char * buffSource;
  Piece * curr = E.PT->head->next;
  /*
    General outline:
    1.  Each nl node has the index of the newline AT THE END OF THE LINE 
        and buffer type, use this to find the right piece (since row may run over multiple pieces)

        The right piece contains the PREVIOUS newline and of the same type
  
    2. For i in range rowX: go from start of row with newline (PRevious nl + 1) and check for tabs

    OOOOOOR lets go make life easier and store the nl at the START of the line and 0 for the first.
   */
  if(row->buffer)
    buffSource = E.PT->added;
  else
    buffSource = E.PT->original;

  while(curr->next) {
    if(curr->bufferType == row->buffer && curr->end > row->newline && curr->start <= row->newline)
      break;
    else
      curr = curr->next;
  }

  for(j = row->newline; j < rowX; j++) {
    if(j >= curr->end) {
      curr = curr->next;
      if(curr->bufferType)
        buffSource = E.PT->added;
      else
        buffSource = E.PT->original;
    }
    if(buffSource[j] == '\t')
      rx += (KI_TAB_STOP - 1) - (rx % KI_TAB_STOP);
    rx++;
  }
  return rx;

}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int idx = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars [j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs*(KI_TAB_STOP - 1) + 1);

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

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorCutRow(int at) {
  if (at < 0 || at >= E.numrows) return;

  E.cutRow->chars = realloc(E.cutRow->chars, E.row[at].size * sizeof(char));
  memmove(E.cutRow->chars, E.row[at].chars, E.row[at].size);
	E.cutRow->size = E.row[at].size;
	E.cutRow->rsize = 0;
	E.cutRow->render = NULL;

	editorUpdateRow(E.cutRow);
	editorDelRow(at);
	
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {

  insertChar(E.location, '\n', &E.PT);

  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cy++;
  E.cx = 0;
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  char *buf = malloc(totlen);
  char *p = buf;
  
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(char *filename, PieceTable * PT) {
  int c;
  int i = 0;
  Node * current = PT->root;

  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) err("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }

  free(line);
  fseek(fp, 0, SEEK_SET);
  while ((c = fgetc(fp)) != EOF) {
    if(c == 10 || c == 13) {
      Node * newNode = malloc(sizeof(Node));
      newNode->newline = i;
      newNode->prev = current;
      newNode->next = NULL;
      current->next = newNode;
      current = newNode;
    }
    i++;
  }
  PT->current = PT->root;

  fclose(fp);
  E.dirty = 0;
}

void editorSave() {

  int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);

  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)");
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }


  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void abAppend(abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(abuf *ab) {
  free(ab->b);
}

/*** output ***/

void editorScroll() {
  E.rx = 0;
  if (E.PT->rowY < E.numrows) 
    // We could grab the set of chars from prev nl to next newline. Thatll give us the character 
    // range we want. For the actual chars, wed need to navigate the piece table till we hit the nth char

    // would it be worth modifying the nl nodes to also include a buffer specificy index? (did this)
    // since we did that, each nl node has the index and buffer, making this a lot easier to do.
    E.rx = editorRowXToRx(E.PT->current, E.PT->rowX);
  
  if (E.PT->rowY < E.rowoff) 
    E.rowoff = E.cy;
  
  if (E.PT->rowY >= E.rowoff + E.screenrows) 
    E.rowoff = E.PT->rowY - E.screenrows + 1;
  
  if (E.rx < E.coloff) 
    E.coloff = E.rx;
  
  if (E.rx >= E.coloff + E.screencols) 
    E.coloff = E.rx - E.screencols + 1;
  
}

void editorDrawRows(abuf *ab) {
  /*int y;
  int filerow;
  char welcome[80];
  int welcomelen;
  int padding;
  int len;

  Piece * curr = E.PT->head->next;
  while(curr->next) {
    char * str;
    len = curr->end - curr->start;
    str = (char *)malloc(sizeof(char) * len);
    
    if(curr->bufferType)
      strncpy(str, E.PT->added + curr->start, len);
    else
      strncpy(str, E.PT->original + curr->start, len);
    
    
  }
  abAppend(ab, "abc", 3);
  abAppend(ab, "\x1b[K", 3);
  abAppend(ab, "\r\n", 2);
  */
 int y;
  int filerow;
  char welcome[80];
  int welcomelen;
  int padding;
  int len;

  for (y = 0; y < E.screenrows; y++) {
    filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        welcomelen = snprintf(welcome, sizeof(welcome),
          "Ki editor -- version %s", KI_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(abuf *ab) {
  char status[80], rstatus[80];
  int len;
  int rlen;

  abAppend(ab, "\x1b[7m", 4);
  len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");
  rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
  char buf[32];

  editorScroll();

  abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  //editorDrawStatusBar(&ab);
  //editorDrawMessageBar(&ab);

  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.PT->rowY - E.rowoff) + 1,
                                            (E.rx - E.coloff) + 1);
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

/*** input ***/

char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  int c;

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
}

void editorMoveCursor(int key) {
  int rowlen;
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
        E.location--;
        if (E.PT->rowX != 0) E.PT->rowX--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (E.location < E.max_location)
        E.location++;
      if (E.PT->current->next) {
        if (E.PT->rowX < E.PT->current->next->newline) {
          E.PT->rowX++;
        }
      }
      else {
        if (E.PT->rowX < E.max_location)
          E.PT->rowX++;
      }
      
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      if (E.PT->rowY != 0) {
        E.PT->rowY--;
        E.PT->current = E.PT->current->prev;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
        E.PT->rowY++;
        E.PT->current = E.PT->current->next;
      }
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress() {
  static int quit_times = KI_QUIT_TIMES;
  	int c = editorReadKey();
	int times;
	
	switch (c) {
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
			return;
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
            times = E.screenrows;
            while (times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
		    case CTRL_KEY('a'):
            E.cx = 0;
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0){
                editorSetStatusMessage("File Has Unsaved Changes: Confirm Input");
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case END_KEY:
		case CTRL_KEY('d'):
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;
        quit_times = KI_QUIT_TIMES;
    }	

	if (E.edit == 0) {
		switch (c) {
			case 'e':
			case 'E':
				E.edit = 1;
				return;
				break;

			case 'w':
			case 'W':
				c = ARROW_UP;
				editorMoveCursor(c);
				break;

			case 'a':
			case 'A':
				c = ARROW_LEFT;
				editorMoveCursor(c);
				break;

			case 's':
			case 'S':
				c = ARROW_DOWN;
				editorMoveCursor(c);
				break;

			case 'd':
			case 'D':
				c = ARROW_RIGHT;
				editorMoveCursor(c);
				break;

			case 'r':
			case 'R':
				editorCutRow(E.cy);
				break;

			case 'f':
			case 'F':
				editorInsertRow(E.cy, E.cutRow->chars, E.cutRow->size);
				break;

			case 'o':
			case 'O':
				editorMoveCursor(ARROW_DOWN);
				editorInsertRow(E.cy, "", 0);
				E.edit = 1;
				return;
				break;
		}
	}

	// ignore all ctrl key presses except for backspace, enter, esc
	if (E.edit && (c > 31 || c == 27 || c == 13 || c == 8)) {
  		switch (c) {
			case '\r':
				editorInsertNewline();
				break;

			case BACKSPACE:
			case DEL_KEY:
				if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
				deleteChar(E.location, &E.PT);
        E.location--;
        if (E.PT->rowX != 0) 
          E.PT->rowX--;
        else {
          if(E.PT->current->prev) {
            Node * temp = E.PT->current;
            E.PT->rowX = E.PT->current->newline - E.PT->current->prev->newline;
            E.PT->current = E.PT->current->prev;
            E.PT->rowY--;
            
            E.PT->current->next = temp->next;
            temp->next->prev = E.PT->current;

            temp->prev = NULL;
            temp->next = NULL;
            free(temp);
          }
          else {
            E.PT->rowX--;
          }
        }
        editorDelChar();
				break;
		
			case '\x1b':
				E.edit = 0;
				break;

			default:
				editorInsertChar(c);
				insertChar(E.location, c , &E.PT);
        E.location++;
        E.max_location++;
        E.PT->rowX++;
				break;
  		}

	}
}

/*** init ***/

void initEditor() {
  E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
  E.max_location = 0;
	E.row = NULL;
	E.cutRow = malloc(sizeof(erow));
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	E.dirty = 0;
	E.edit = 0;
	
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) err("getWindowSize");
	E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    // TODO: Convert to using "rows" based on newline rb tree
  	enableRawMode();
  	initEditor();
  	if (argc >= 2) {
		  E.PT = initPieceTable(argv[1]);
    	editorOpen(argv[1], E.PT);
  	}

  	editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  	while (1) {
    	editorRefreshScreen();
    	editorProcessKeypress();
  	}

  return 0;
}
