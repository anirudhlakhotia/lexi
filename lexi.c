/***  includes  ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
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

#define LEXI_VERSION "0.0.1"
#define LEXI_TAB_STOP 8
#define CTRL_KEY(k) ((k)&0x1f)
enum editorKey
{
  ARROW_LEFT = 1000, // given value higher than 'char' range, so it does not clash with any other key press
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

typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow; // stores a line of text as a pointer

struct editorConfig
{
  int cursor_x, cursor_y; // cursor x and y
  int rx;
  int rowoff; //for vertical scrolling
  int coloff; //for horizontal scrolling
  int screenrows;
  int screencols;
  int numrows;
  erow *row; // pointer to erow
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
};
struct editorConfig E;

/*** terminal ***/

// Terminal starts in canonical mode by default (Input sent only when enter is pressed)
// For a text editor, raw mode is used to allow input to be sent when any key is pressed

void die(const char *s)
{
  write(STDOUT_FILENO, "\x1b[2J", 4); // erase entire screen
  write(STDOUT_FILENO, "\x1b[H", 3);  // move cursor to 0,0
  perror(s);                          // print error message based on global errno
  exit(1);
}

void disableRawMode()
{
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) // tcsetattr() for setting terminal attributes
    die("tcsetattr");
}

void enableRawMode()
{
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode); // call function when program is terminated , comes from <stdlib.h>

  struct termios raw = E.orig_termios;                      // copy of original terminal state stored in orig_termios
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // disable all input processing  (input flags)
  raw.c_oflag &= ~(OPOST);                                  // disable all output processing (output flags) (\n to \r\n translation)
  raw.c_cflag |= (CS8);                                     // 8-bit chars
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);          // disable echo, canonical input, extended input, and signal processing i.e. ctrl+C, ctrl+Z (defined in termios.h)
  raw.c_cc[VMIN] = 0;                                       // minimum number of characters to read before returning
  raw.c_cc[VTIME] = 1;                                      // timeout in deciseconds before returning

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}
int editorReadKey() // read key from terminal
{
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
  {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }
  if (c == '\x1b')
  {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';
    if (seq[0] == '[')
    {
      if (seq[1] >= '0' && seq[1] <= '9')
      {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~')
        {
          switch (seq[1])
          {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP; // moves cursor to top of the screen
          case '6':
            return PAGE_DOWN; // moves cursor to bottom of the screen
          case '7':
            return HOME_KEY; // moves cursor to left edge of the screen
          case '8':
            return END_KEY; // moves cursor to right edge of the screen
          }
        }
      }
      else
      {
        switch (seq[1])
        {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    }
    else if (seq[0] == 'O')
    {
      switch (seq[1])
      {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }

    return '\x1b';
  }
  else
  {
    return c;
  }
}
int getCursorPosition(int *rows, int *cols)
{
  char buf[32];
  unsigned int i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;
  while (i < sizeof(buf) - 1)
  {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0'; // add null character
  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) // reads data from buffer into required location
    return -1;                                   // comes from <stdio.h>,passing it the string to put the values into the rows and cols variables

  return 0;
}

int getWindowSize(int *rows, int *cols)
{
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
  { // Terminal IOCtl(Input/Output Control) Get WINdow SiZe)
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  }
  else
  {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cursor_x) {
  int rx = 0;
  int j;
  for (j = 0; j < cursor_x; j++) {
    if (row->chars[j] == '\t')
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    rx++;
  }
  return rx;
} //converts a chars index into a render index

void editorUpdateRow(erow *row) {
  //rendering tabs
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;
  free(row->render);
  row->render = malloc(row->size + tabs*(LEXI_TAB_STOP-1) + 1);
  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % LEXI_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
  //allocates space for a new row
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

/*** file i/o ***/

void editorOpen(char *filename) {
  // allows the user to open a file
  free(E.filename);
  E.filename = strdup(filename);
  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
} 

/*** append buffer ***/

struct append_buffer
{ // acts as a dynamic string
  char *b;
  int len;
};
#define append_buffer_INIT \
  {                        \
    NULL, 0                \
  } // acts as an empty buffer

void abAppend(struct append_buffer *ab, const char *s, int len)
{
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len); // memcpy() comes from <string.h>, and copies the string s at the end of the current data in the buffer
  ab->b = new;
  ab->len += len;
}
void abFree(struct append_buffer *ab)
{ // de-allocates memory used by append_buffer
  free(ab->b);
}

/*** output ***/

void editorScroll() {
  // ensures cursor is inside the visible window
  E.rx = 0;
  if (E.cursor_y < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cursor_y], E.cursor_x);
  }
  if (E.cursor_y < E.rowoff) {
    E.rowoff = E.cursor_y;
  }
  if (E.cursor_y >= E.rowoff + E.screenrows) {
    E.rowoff = E.cursor_y - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void editorDrawRows(struct append_buffer *ab)
{ // handles drawing each row or column of text being edited
  int y;
  for (y = 0; y < E.screenrows; y++)
  { int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) //welcome message only displays if the text buffer is completely empty
      {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome), "Lexi editor -- version %s", LEXI_VERSION); // comes from <stdio.h>
        if (welcomelen > E.screencols)
          welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2; // center the text
        if (padding)
        {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--)
          abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      }
      else
      {
        abAppend(ab, "~", 1);
    }
  }  else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    } 
    abAppend(ab, "\x1b[K", 3); // erases line one at a time
    
    // last line handled separately
    abAppend(ab, "\r\n", 2);
    
  }
}

//creating a status bar to display details about the file
void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80],rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
    E.filename ? E.filename : "[No Name]", E.numrows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cursor_y + 1, E.numrows);
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

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen()
{
  editorScroll();
  struct append_buffer ab = append_buffer_INIT;
  abAppend(&ab, "\x1b[?25l", 6); // hide cursor
  abAppend(&ab, "\x1b[H", 3);
  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6);      // show cursor
  write(STDOUT_FILENO, ab.b, ab.len); // write() and STDOUT_FILENO come from <unistd.h>
  abFree(&ab);
}

//displaying messages to the user using a status message
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** input ***/

void editorMoveCursor(int key)
{
  erow *row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
  switch (key)
  {
  case ARROW_LEFT:
    if (E.cursor_x != 0)
    {
      E.cursor_x--;
    } else if (E.cursor_y > 0) {
      E.cursor_y--;
      E.cursor_x = E.row[E.cursor_y].size;
    }
    break;
  case ARROW_RIGHT:
    if(row && E.cursor_x < row->size)
    {
      E.cursor_x++;
    } else if (row && E.cursor_x == row->size) {
      E.cursor_y++;
      E.cursor_x = 0;
    }
    break;
  case ARROW_UP:
    if (E.cursor_y != 0)
    {
      E.cursor_y--;
    }
    break;
  case ARROW_DOWN:
    if (E.cursor_y < E.numrows)
    {
      E.cursor_y++;
    }
    break;
  }
  //snapping cursor to the end of line
  row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
  int rowlen = row ? row->size : 0;
  if (E.cursor_x > rowlen) {
    E.cursor_x = rowlen;
  }
}
void editorProcessKeypress() // to wait for a keypress and handles it
{
  int c = editorReadKey(); // to wait for one keypress and return it
  switch (c)
  {
  case CTRL_KEY('q'): // exit point
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  case HOME_KEY:
    E.cursor_x = 0;
    break;
  case END_KEY: //moves the cursor to the end of the current line
    if (E.cursor_y < E.numrows)
      E.cursor_x = E.row[E.cursor_y].size;
    break;
  case PAGE_UP:
  case PAGE_DOWN:
  {
    if (c == PAGE_UP) {
      E.cursor_y = E.rowoff;
    } else if (c == PAGE_DOWN) {
      E.cursor_y = E.rowoff + E.screenrows - 1;
      if (E.cursor_y > E.numrows) E.cursor_y = E.numrows;
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

/*** init ***/

void initEditor()
{ // initialize all the fields in the E struct
  E.cursor_x = 0;
  E.cursor_y = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL; //initializing pointer to null
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
  E.screenrows -= 2;
}
int main(int argc, char *argv[])
{
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);}

  editorSetStatusMessage("HELP: Ctrl-Q = quit");
  
  while (1)
  {
    editorRefreshScreen(); //
    editorProcessKeypress();
    char c = '\0';
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
      die("read");
    if (iscntrl(c)) // check if character is a control character
    {
      printf("%d\r\n", c);
    }
    else
    {
      printf("%d ('%c')\r\n", c, c);
    }
    if (c == CTRL_KEY('q'))
      break;
  }

  return 0;
}


