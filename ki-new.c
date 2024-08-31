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
#define BUFF_SIZE 65536
#define KI_VERSION "0.0.4"
#define KI_TAB_STOP 4
#define KI_QUIT_TIMES 1
#define ABUF_INIT {NULL, 0}
#define CTRL_KEY(k) ((k) & 0x1f)
#define RED 1
#define BLACK 0

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

struct Node {
  int newline;
  int buffer;

  struct Node * next;
  struct Node * prev;
}; typedef struct Node Node;

struct Piece {
	int start;
	int length;
  int cumulativeLen;
	int bufferType;
  int color;
	struct Piece * left;
	struct Piece * right;
  struct Piece * parent;
}; typedef struct Piece Piece;

struct PieceTable {
  int bufferIndex;
  int addIndex;
  int rowX;
  int rowY;
  int renderX;
  int location;
  int max_location;
	char * original;
	char * added;
  
  Node * lineRoot;
  Node * current;
	Piece * root;
}; typedef struct PieceTable PieceTable;

struct editorConfig {
	int screenrows;
	int screencols;
	int numrows;
	int rowoff;
	int coloff;
	unsigned int dirty : 1;
	unsigned int edit : 1;

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


/*** editor management ***/

Piece * getNewPieceNode(int start, int length, int cumulativeLen, int bufferType) {
	Piece * piece = (Piece *)malloc(sizeof(Piece));
	piece->start = start;
	piece->length = length;
	piece->bufferType = bufferType;
  piece->color = RED;
  piece->cumulativeLen = cumulativeLen;
  piece->left = piece->right = piece->parent = NULL;

	return piece;
}

int getCumulativeLength(Piece * node) {
    return node ? node->cumulativeLen : 0;
}

void updateNode(Piece * node) {
    if (node) {
        node->cumulativeLen = node->length + getCumulativeLength(node->left) + getCumulativeLength(node->right);
    }
}

void leftRotate(Piece ** root, Piece * x) {
  Piece * y = x->right;
  x->right = y->left;

  if (y->left != NULL)
      y->left->parent = x;

  y->parent = x->parent;

  if (x->parent == NULL)
      * root = y;
  else if (x == x->parent->left)
      x->parent->left = y;
  else
      x->parent->right = y;

  y->left = x;
  x->parent = y;

}

void rightRotate(Piece ** root, Piece * y) {
  Piece * x = y->left;
  y->left = x->right;

  if (x->right != NULL)
      x->right->parent = y;

  x->parent = y->parent;

  if (y->parent == NULL)
      * root = x;
  else if (y == y->parent->left)
      y->parent->left = x;
  else
      y->parent->right = x;

  x->right = y;
  y->parent = x;

}

void fixViolation(Piece ** root, Piece * node) {
    Piece * parent = NULL;
    Piece * grandparent = NULL;
    Piece * uncle;

    while ((node != *root) && (node->color != BLACK) && (node->parent->color == RED)) {
        parent = node->parent;
        grandparent = node->parent->parent;

        /* Case A: Parent of node is left child of Grand-parent of node */
        if (parent == grandparent->left) {
            uncle = grandparent->right;

            /* Case 1: The uncle of node is also red, only recoloring required */
            if (uncle != NULL && uncle->color == RED) {
                grandparent->color = RED;
                parent->color = BLACK;
                uncle->color = BLACK;
                node = grandparent;
            } else {
                /* Case 2: Node is right child of its parent, left-rotation required */
                if (node == parent->right) {
                    leftRotate(root, parent);
                    node = parent;
                    parent = node->parent;
                }

                /* Case 3: Node is left child of its parent, right-rotation required */
                rightRotate(root, grandparent);
                char t = parent->color;
                parent->color = grandparent->color;
                grandparent->color = t;
                node = parent;
            }
        }

        /* Case B: Parent of node is right child of Grand-parent of node */
        else {
            uncle = grandparent->left;

            /* Case 1: The uncle of node is also red, only recoloring required */
            if (uncle != NULL && uncle->color == RED) {
                grandparent->color = RED;
                parent->color = BLACK;
                uncle->color = BLACK;
                node = grandparent;
            } else {
                /* Case 2: Node is left child of its parent, right-rotation required */
                if (node == parent->left) {
                    rightRotate(root, parent);
                    node = parent;
                    parent = node->parent;
                }

                /* Case 3: Node is right child of its parent, left-rotation required */
                leftRotate(root, grandparent);
                char t = parent->color;
                parent->color = grandparent->color;
                grandparent->color = t;
                node = parent;
            }
        }
    }

    (*root)->color = BLACK;
}

void insertPiece(Piece * current, Piece * node) {
  if(current->cumulativeLen < node->cumulativeLen) {
    if(current->right) {
      insertPiece(current->right, node);
    }
    else {
      current->right = node;
      node->parent = current;
    }
  }
  else {
    if(current->left) {
      insertPiece(current->left, node);
    }
    else {
      current->left = node;
      node->parent = current;
    }
  }
}

Piece * searchForPiece(int pos, Piece * current) {
  // Find the smallest node that is larger than or equal to cumLen of current
  Piece * possible;

  if(current == NULL)
    return NULL;
  
  if(current->cumulativeLen == pos)
    return current;

  if(current->cumulativeLen > pos) {
      possible = searchForPiece(pos, current->left);
      if(possible == NULL)
        return current;
      else
        return possible;
  }
  else {
    return searchForPiece(pos, current->right);
  }

}

void * updateRightSubtree(Piece * current) {
  if (current == NULL)
    return;
  else {
    current->cumulativeLen++;
    updateRightSubtree(current->left);
    updateRightSubtree(current->right);
  }
}

void * updateCumulativeLengths(int cumulativeLen, Piece * current) {
  // from the current node, move upwards. If the node has a larger cumulativeLength, update it
  // and its right subtree. Move up again until NULL

  if (current == NULL)
    return;
  
  if (current->cumulativeLen > cumulativeLen) {
    current->cumulativeLen++;
    updateRightSubtree(current);
  }
  updateCumulativeLengths(cumulativeLen, current->parent);

}

PieceTable * initPieceTable(char * filename) {
  long originalSize;
  int i = 0;
  int c;
  Node * current;
	PieceTable * PT = (PieceTable *)malloc(sizeof(PieceTable));
	PT->added = (char *)malloc(BUFF_SIZE * sizeof(char));	
  PT->lineRoot = (Node *)malloc(sizeof(Node));
  current = PT->lineRoot;
  PT->lineRoot->newline = 0;
  PT->rowX = 0;
  PT->rowY = 0;
	PT->bufferIndex = 0;
  PT->addIndex = -1;
  PT->location = 0;
  PT->renderX = 0;

	FILE *fp = fopen(filename, "r");
  if (fp) {
		fseek(fp, 0, SEEK_END);
		originalSize = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		
    PT->max_location = originalSize;
		PT->original = (char *) malloc (sizeof(char) * (originalSize + 1));
		if (PT->original) {
			fread(PT->original, 1, originalSize, fp);
		}

    fseek(fp, 0, SEEK_SET);
    while ((c = fgetc(fp)) != EOF) {
      if(c == 10 || c == 13) {
        Node * lineNode = malloc(sizeof(Node));

        lineNode->newline = i;
        lineNode->prev = current;
        lineNode->next = NULL;
        current->next = lineNode;
        current = lineNode;
      }
      i++;

    }

		fclose(fp);
		PT->root = getNewPieceNode(0, 0, originalSize, 0);
	}
	else {
		PT->original = "";
		PT->root = getNewPieceNode(0, 0, 0, 0);
	}

	PT->root->color = BLACK;
	

	return PT;
}

void insertChar(int pos, char character, PieceTable ** PT) {
	if(pos < 0 || pos > (*PT)->max_location)
		err ("invalid insertion location");
	
	Piece * newNode;
	Piece * curr = searchForPiece(pos, (*PT)->root);
  
  if(pos == curr->cumulativeLen - curr->length) {
    // inserting on front boundary always requires creating a new add node
    (*PT)->addIndex += 1;
    newNode = getNewPieceNode((*PT)->addIndex, 1, pos+1, 1);
    insertPiece((*PT)->root, newNode);
    fixViolation(&((*PT)->root), newNode);
    updateCumulativeLengths(pos+1, newNode);

    (*PT)->added[(*PT)->addIndex] = character;
    return;
  
  }
  
  if(pos == curr->cumulativeLen) {
    // inserting on end boundary allows extending the existing node if current end is one less
    // than addIndex and both are add nodes
    (*PT)->addIndex = (*PT)->addIndex+1;
    if(curr->bufferType == 1 && curr->length + curr->start == (*PT)->addIndex) {
      curr->length++;
      curr->cumulativeLen++;
      (*PT)->added[(*PT)->addIndex] = character;
      updateCumulativeLengths(curr->cumulativeLen, curr);
      // need to adjust cumulative lengths
      return;
    }
    else {
      // otherwise we need to create a new node
      newNode = getNewPieceNode((*PT)->addIndex, 1, curr->cumulativeLen+1, 1);
      fixViolation(&((*PT)->root), newNode);
      updateCumulativeLengths(pos+1, newNode);
      
      (*PT)->added[(*PT)->addIndex] = character;
      return;
    }

  }
  // inserting into the middle of a Piece, need to split
  else {
    (*PT)->addIndex = (*PT)->addIndex+1;

    //Piece * back = getNewPieceNode(curr->start, curr->end - (unified_top_range-pos), curr->bufferType);
    //Piece * middle = getNewPieceNode((*PT)->addIndex, (*PT)->addIndex+1, 1);
    //Piece * front = getNewPieceNode( curr->end - (unified_top_range-pos), curr->end, curr->bufferType);

    Piece * back = getNewPieceNode(curr->start, num_before_split, pos, 1);
    Piece * middle = getNewPieceNode((*PT)->addIndex, 1, pos + 1, 1);
    Piece * front = getNewPieceNode(curr->start + num_before_split, num_after_split, curr->cumulativeLen+1, 1)
    
    return;	
  }
}
/*
void deleteChar(int pos, PieceTable ** PT) {
  // three situations, delete last char in node, delete edge of node, delete mid of node
  Piece * curr = (*PT)->root->next;
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
*/

/** open/save ***/

/*** io ***/
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

void initEditor() {
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	E.dirty = 0;
	E.edit = 0;
	
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) err("getWindowSize");
	E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    //NOTE: when displaying text, \n will not get written as \r\n so we will need to handle that
    //      when displaying, look at test.c
    //TODO: Port keyboard event loop, update insert/delete
    
    enableRawMode();
  	initEditor();
  	if (argc >= 2)
		  E.PT = initPieceTable(argv[1]);

    abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, E.PT->original, E.PT->max_location);
    abAppend(&ab, "\x1b[K", 3);
    abAppend(&ab, "\r\n", 2);
    abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);


  return 0;
}