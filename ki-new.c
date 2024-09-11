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
  int len;

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

Piece *minValueNode(Piece *node) {
    while (node->left != NULL)
        node = node->left;
    return node;
}

void replaceNode(Piece **root, Piece *u, Piece *v) {
    if (u->parent == NULL)
        *root = v;
    else if (u == u->parent->left)
        u->parent->left = v;
    else
        u->parent->right = v;

    if (v != NULL)
        v->parent = u->parent;
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

void fixDelete(Piece **root, Piece *x) {
    while (x != *root && x->color == BLACK) {
        if (x == x->parent->left) {
            Piece *w = x->parent->right;
            if (w->color == RED) {
                w->color = BLACK;
                x->parent->color = RED;
                leftRotate(root, x->parent);
                w = x->parent->right;
            }

            if (w->left->color == BLACK && w->right->color == BLACK) {
                w->color = RED;
                x = x->parent;
            } else {
                if (w->right->color == BLACK) {
                    w->left->color = BLACK;
                    w->color = RED;
                    rightRotate(root, w);
                    w = x->parent->right;
                }
                w->color = x->parent->color;
                x->parent->color = BLACK;
                w->right->color = BLACK;
                leftRotate(root, x->parent);
                x = *root;
            }
        } else {
            Piece *w = x->parent->left;
            if (w->color == RED) {
                w->color = BLACK;
                x->parent->color = RED;
                rightRotate(root, x->parent);
                w = x->parent->left;
            }

            if (w->right->color == BLACK && w->left->color == BLACK) {
                w->color = RED;
                x = x->parent;
            } else {
                if (w->left->color == BLACK) {
                    w->right->color = BLACK;
                    w->color = RED;
                    leftRotate(root, w);
                    w = x->parent->left;
                }
                w->color = x->parent->color;
                x->parent->color = BLACK;
                w->left->color = BLACK;
                rightRotate(root, x->parent);
                x = *root;
            }
        }
    }
    if(x)
      x->color = BLACK;
}

void insertPiece(Piece * current, Piece * node) {
  if(current == NULL) {
    E.PT->root = node;
    return;
  }
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

void deletePiece(Piece ** root, Piece * z) {
  Piece *y = z;
  Piece *x;
  int origColor = y->color;

  if (z->left == NULL) {
      x = z->right;
      replaceNode(root, z, z->right);
  } else if (z->right == NULL) {
      x = z->left;
      replaceNode(root, z, z->left);
  } else {
      y = minValueNode(z->right);
      origColor = y->color;
      x = y->right;
      if (y->parent == z && x != NULL)
          x->parent = y;
      else {
          replaceNode(root, y, y->right);
          y->right = z->right;
          if(y->right != NULL)
            y->right->parent = y;
      }
      replaceNode(root, z, y);
      y->left = z->left;
      y->left->parent = y;
      y->color = z->color;
  }

  if (origColor == BLACK)
      fixDelete(root, x);
}

Piece * searchForPiece(int pos, Piece * current) {
  // Find the smallest node that has a cumulativeLen larger than or equal to pos
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

void updateRightSubtree(Piece * current) {
  if (current == NULL)
    return;
  else {
    current->cumulativeLen++;
    updateRightSubtree(current->left);
    updateRightSubtree(current->right);
  }
}

void decrementRightSubtree(Piece * current) {
  if(current == NULL)
    return;
  else {
    current->cumulativeLen--;
    decrementRightSubtree(current->left);
    decrementRightSubtree(current->right);
  }
}

void inOrderTraversal(Piece *root) {
    if (root == NULL)
        return;

    // Traverse the left subtree
    inOrderTraversal(root->left);

    // Print the current node's data and color
    printf("cumulative len: %d \r\ncolor: (%s) \r\nstart: %d \r\nlength: %d\r\n\r\n", root->cumulativeLen, (root->color == RED) ? "Red" : "Black", root->start, root->length);

    // Traverse the right subtree
    inOrderTraversal(root->right);
}

void updateCumulativeLengths(int cumulativeLen, Piece * current, int recursiveCount) {
  // from the current node, move upwards. If the node has a larger cumulativeLength, update it
  // and its right subtree. Move up again until NULL

  if (current == NULL)
    return;
  
  // special case of the original node is the root or parent smaller than origianl node
  if ( (current == E.PT->root && recursiveCount == 0) || (current->right && current->right->cumulativeLen > cumulativeLen && recursiveCount == 0) ) {
    updateRightSubtree(current->right);
    return;
  }

  if (current->cumulativeLen > cumulativeLen) {
    current->cumulativeLen++;
    updateRightSubtree(current->right);
  }
  updateCumulativeLengths(cumulativeLen, current->parent, recursiveCount + 1);

}

void decrementCumulativeLengths(int cumulativeLen, Piece * current, int recursiveCount) {
  // same as above, except subtracting

  if (current == NULL)
    return;
  
  if ( (current == E.PT->root && recursiveCount == 0) || (current->right && current->right->cumulativeLen > cumulativeLen && recursiveCount == 0) ) {
    decrementRightSubtree(current->right);
    return;
  }

  if (current->cumulativeLen > cumulativeLen) {
    current->cumulativeLen--;
    decrementRightSubtree(current->right);
  }
  decrementCumulativeLengths(cumulativeLen, current->parent, recursiveCount + 1);


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
  PT->lineRoot->len = 0;
  PT->rowX = 0;
  PT->rowY = 0;
	PT->bufferIndex = 0;
  PT->addIndex = -1;
  PT->location = 0;

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

        current->len = i;
        current->next = lineNode;
        lineNode->prev = current;
        current = lineNode;
        i = 0;
      }
      i++;

    }

		fclose(fp);
		PT->root = getNewPieceNode(0, originalSize, originalSize, 0);
	}
	else {
		PT->original = "";
		PT->root = getNewPieceNode(0, 0, 0, 0);
	}

	PT->root->color = BLACK;
	PT->current = PT->lineRoot;

	return PT;
}

void insertChar(int pos, char character, PieceTable ** PT) {
	if(pos < 0 || pos > (*PT)->max_location)
		err ("invalid insertion location");
	
	Piece * newNode;
	Piece * curr = searchForPiece(pos, (*PT)->root);
  
  (*PT)->max_location++;
  (*PT)->addIndex = (*PT)->addIndex+1;
  
  // create new lineNode
  if(character == '\n') {
    Node * lineNode = (Node *)malloc(sizeof(Node));
    lineNode->len = 0;
    lineNode->prev = E.PT->current;
    lineNode->next = E.PT->current->next;
    lineNode->next->prev = lineNode;
    lineNode->prev->next = lineNode;
  }

  if(pos == curr->cumulativeLen - curr->length) {
    // inserting on front boundary always requires creating a new add node
    newNode = getNewPieceNode((*PT)->addIndex, 1, pos+1, 1);
    insertPiece((*PT)->root, newNode);
    fixViolation(&((*PT)->root), newNode);
    updateCumulativeLengths(pos+1, newNode, 0);

    (*PT)->added[(*PT)->addIndex] = character;
    return;
  
  }
  
  if(pos == curr->cumulativeLen) {
    // inserting on end boundary allows extending the existing node if current end is one less
    // than addIndex and both are add nodes
    if(curr->bufferType == 1 && curr->length + curr->start == (*PT)->addIndex) {
      curr->length++;
      curr->cumulativeLen++;
      (*PT)->added[(*PT)->addIndex] = character;
      updateCumulativeLengths(curr->cumulativeLen, curr, 0);
      // need to adjust cumulative lengths
      return;
    }
    else {
      // otherwise we need to create a new node
      newNode = getNewPieceNode((*PT)->addIndex, 1, curr->cumulativeLen+1, 1);
      insertPiece((*PT)->root, newNode);
      fixViolation(&((*PT)->root), newNode);
      updateCumulativeLengths(pos+1, newNode, 0);
      
      (*PT)->added[(*PT)->addIndex] = character;
      return;
    }

  }
  // inserting into the middle of a Piece, need to split
  else {

    // Piece * getNewPieceNode(int start, int length, int cumulativeLen, int bufferType)
    int num_before_split = pos - (curr->cumulativeLen - curr->length);
    int num_after_split = curr->cumulativeLen - pos;

    Piece * back = getNewPieceNode(curr->start, num_before_split, pos, curr->bufferType);
    Piece * middle = getNewPieceNode((*PT)->addIndex, 1, pos + 1, 1);
    Piece * front = getNewPieceNode(curr->start + num_before_split, num_after_split, curr->cumulativeLen, curr->bufferType);
    
    
    deletePiece(&((*PT)->root), curr);
    insertPiece((*PT)->root, back);
    fixViolation(&((*PT)->root), back);
    
    insertPiece((*PT)->root, middle);
    fixViolation(&((*PT)->root), middle);
    
    insertPiece((*PT)->root, front);
    fixViolation(&((*PT)->root), front);
    
    updateCumulativeLengths(middle->cumulativeLen, middle, 0);
    (*PT)->added[(*PT)->addIndex] = character;

    return;	
  }
}

void deleteChar(int pos, PieceTable ** PT) {
  // three situations, delete last char in node, delete edge of node, delete mid of node
  Piece * curr = searchForPiece(pos, (*PT)->root);
  Piece * front;
  Piece * back;
  int charPos = curr->start + pos - curr->start;
  (*PT)->max_location--;
  if (pos == 0) return;


  if( (curr->bufferType && (*PT)->added[charPos] == '\n') || (curr->bufferType == 0 && (*PT)->original[charPos] == '\n') ) {
    Node * copy = (*PT)->current;
    (*PT)->current->prev->len = (*PT)->current->prev->len + (*PT)->current->len;
    (*PT)->current->prev->next = (*PT)->current->next;
    (*PT)->current->next->prev = (*PT)->current->prev;
    (*PT)->current = (*PT)->current->prev;
    copy->next = NULL;
    copy->prev = NULL;
    free(copy);
  }
  // delete whole node
  if(curr->length == 1) {
    decrementCumulativeLengths(curr->cumulativeLen, curr, 0);
    deletePiece(&((*PT)->root), curr);
  }

  // delete from end of node
  else if (pos == curr->cumulativeLen) {
    curr->length = curr->length - 1;
    curr->cumulativeLen = curr->cumulativeLen - 1;
    decrementCumulativeLengths(curr->cumulativeLen, curr, 0);
  }
  // delete from start of node
  else if (pos == curr->cumulativeLen - curr->length) {
    curr->length = curr->length - 1;
    curr->cumulativeLen = curr->cumulativeLen - 1;
    curr->start = curr->start + 1;
    decrementCumulativeLengths(curr->cumulativeLen, curr, 0);
  }

  // deleting from middle of node
  else {
    int num_before_split = pos - (curr->cumulativeLen - curr->length);
    int num_after_split = curr->cumulativeLen - pos - 1;

    back = getNewPieceNode(curr->start, num_before_split, pos, curr->bufferType);
    front = getNewPieceNode(curr->start + num_before_split + 1, num_after_split, curr->cumulativeLen - 1, curr->bufferType);

    deletePiece(&((*PT)->root), curr);
    insertPiece((*PT)->root, back);
    fixViolation(&((*PT)->root), back);
    
    insertPiece((*PT)->root, front);
    fixViolation(&((*PT)->root), front);
    
    decrementCumulativeLengths(back->cumulativeLen, back, 0);
    
  }
}

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

void editorAddChars(abuf * ab, Piece * curr) {
  char * start;
  char * end;
  char * substr;
  
  if (curr == NULL)
        return;

  // Traverse the left subtree
  editorAddChars(ab, curr->left);

  // add chars in piece to ab
  if(curr->bufferType) {
    // append buff
    start = &(E.PT->added[curr->start]);
    end = &(E.PT->added[curr->start + curr->length]);
  }
  else {
    // orig buff
    start = &(E.PT->original[curr->start]);
    end = &(E.PT->original[curr->start + curr->length]);
  }
  // merge
  substr = (char *)calloc(1, end - start);
  memcpy(substr, start, end - start);
  abAppend(ab, substr, curr->start + curr->length);

  // Traverse the right subtree
  editorAddChars(ab, curr->right);
}

void editorRefreshScreen() {
  abuf ab = ABUF_INIT;
  char buf[32];

  disableRawMode();
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorAddChars(&ab, E.PT->root);
  
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.PT->rowY + 1, E.PT->rowX + 1);
  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
  enableRawMode();
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

void editorMoveCursor(int key) {
  switch(key) {
    case ARROW_LEFT:
      if(E.PT->rowX != 0) {
        E.PT->rowX--;
        E.PT->location--;
      }
      break;
    case ARROW_RIGHT:
      if(E.PT->rowX < E.PT->current->len) {
        E.PT->rowX++;
        E.PT->location++;
      }
      break;
    case ARROW_UP:
      if(E.PT->rowY != 0) {
        E.PT->rowY--;
        if(E.PT->current->prev->len <= E.PT->rowX) {
          E.PT->location = E.PT->location - E.PT->current->len;
          E.PT->rowX = E.PT->current->prev->len;
        }
        else {
          E.PT->location = E.PT->location - E.PT->current->prev->len;
        }
        E.PT->current = E.PT->current->prev;
      }
      break;
    case ARROW_DOWN:
      if(E.PT->current->next) {
        E.PT->rowY++;
        if(E.PT->current->next->len <= E.PT->rowX) {
          E.PT->location = E.PT->location + E.PT->current->next->len;
          E.PT->rowX = E.PT->current->next->len;
        }
        else {
          E.PT->location = E.PT->location + E.PT->current->len;
        }
        E.PT->current = E.PT->current->next;
      }
      break;
  }
}

void editorProcessKeypress() {
  static int quit_times = KI_QUIT_TIMES;
  int c = editorReadKey();

  switch (c) {
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      return;
      break;
  }
}

int main(int argc, char *argv[]) {
    //      might be an off by one error in pos for delete (easily handled)
    // TODO: Port keyboard event loop
    
    enableRawMode();
  	initEditor();
  	if (argc >= 2)
		  E.PT = initPieceTable(argv[1]);

    insertChar(6, 'A', &(E.PT));
    insertChar(7, 'B', &(E.PT));
    insertChar(8, 'C', &(E.PT));

    while(1) {
      editorRefreshScreen();
      editorProcessKeypress();
    }
    

  return 0;
}