/***  includes  ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

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

#define LEXI_VERSION "0.0.1"
#define LEXI_TAB_STOP 8
#define LEXI_QUIT_TIMES 3
#define CTRL_KEY(k) ((k)&0x1f)
enum editorKey
{
  BACKSPACE = 127,
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

typedef struct editor_row
{
  int size;
  int rsize;
  char *chars;
  char *render;
} editor_row; // stores a line of text as a pointer

struct editorConfig
{
  int cursor_x, cursor_y; // cursor x and y
  int rx;
  int rowoff; // for vertical scrolling
  int coloff; // for horizontal scrolling
  int screenrows;
  int screencols;
  int numrows;
  editor_row *row; // pointer to editor_row
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
};
struct editorConfig E;

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int)); // takes a callback function as argument 
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

int editorRowCxToRx(editor_row *row, int cursor_x)
{
  int rx = 0;
  int j;
  for (j = 0; j < cursor_x; j++)
  {
    if (row->chars[j] == '\t')
      rx += (LEXI_TAB_STOP - 1) - (rx % LEXI_TAB_STOP);
    rx++;
  }
  return rx;
} // converts a chars index into a render index

int editorRowRxToCx(editor_row *row, int rx)
{
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; cx++)
  {
    if (row->chars[cx] == '\t')
      cur_rx += (LEXI_TAB_STOP - 1) - (cur_rx % LEXI_TAB_STOP);
    cur_rx++;
    if (cur_rx > rx)
      return cx;
  }
  return cx;
}

void editorUpdaterow(editor_row *row)
{
  // rendering tabs
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t')
      tabs++;
  free(row->render);
  row->render = malloc(row->size + tabs * (LEXI_TAB_STOP - 1) + 1);
  int idx = 0;
  for (j = 0; j < row->size; j++)
  {
    if (row->chars[j] == '\t')
    {
      row->render[idx++] = ' ';
      while (idx % LEXI_TAB_STOP != 0)
        row->render[idx++] = ' ';
    }
    else
    {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len)
{
  if (at < 0 || at > E.numrows)
    return;
  E.row = realloc(E.row, sizeof(editor_row) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(editor_row) * (E.numrows - at));
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdaterow(&E.row[at]);
  E.numrows++;
  E.dirty++;
}

void editorFreerow(editor_row *row) // frees the memory allocated to an erow
{
  free(row->render);
  free(row->chars);
}
void editorDelRow(int at) //
{
  if (at < 0 || at >= E.numrows)
    return;
  editorFreerow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(editor_row) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorRowDelChar(editor_row *row, int at)
{
  if (at < 0 || at >= row->size)
    return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdaterow(row);
  E.dirty++;
}

void editorRowInsertChar(editor_row *row, int at, int c)
{
  if (at < 0 || at > row->size)
    at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdaterow(row);
  E.dirty++;
}

void editorRowAppendString(editor_row *row, char *s, size_t len) // append string to the end of a row
{
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdaterow(row);
  E.dirty++;
}

/*** editor operations ***/
void editorInsertChar(int c)
{
  if (E.cursor_y == E.numrows)
  {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cursor_y], E.cursor_x, c);
  E.cursor_x++;
}

void editorInsertNewline()
{
  if (E.cursor_x == 0)
  {
    editorInsertRow(E.cursor_y, "", 0);
  }
  else
  {
    editor_row *row = &E.row[E.cursor_y];
    editorInsertRow(E.cursor_y + 1, &row->chars[E.cursor_x], row->size - E.cursor_x);
    row = &E.row[E.cursor_y];
    row->size = E.cursor_x;
    row->chars[row->size] = '\0';
    editorUpdaterow(row);
  }
  E.cursor_y++;
  E.cursor_x = 0;
}

void editorDelChar()
{
  if (E.cursor_y == E.numrows)
    return;
  if (E.cursor_x == 0 && E.cursor_y == 0)
    return;
  editor_row *row = &E.row[E.cursor_y];
  if (E.cursor_x > 0)
  {
    editorRowDelChar(row, E.cursor_x - 1);
    E.cursor_x--;
  }
  else
  {
    E.cursor_x = E.row[E.cursor_y - 1].size;
    editorRowAppendString(&E.row[E.cursor_y - 1], row->chars, row->size);
    editorDelRow(E.cursor_y);
    E.cursor_y--;
  }
}
/*** file i/o ***/

char *editorRowsToString(int *buflen) // write the erow structures from array to a file by converting them into a single string
{
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++)
  {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorOpen(char *filename)
{
  // allows the user to open a file
  free(E.filename);
  E.filename = strdup(filename);
  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1)
  {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() // save the file to the disk
{
  if (E.filename == NULL)
  {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
    if (E.filename == NULL)
    {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }
  int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644); // O_CREAT : Create if it doesn't exist, O_RDWR : Read and write
  if (fd != -1)
  {
    if (ftruncate(fd, len) != -1)
    {
      if (write(fd, buf, len) == len)
      {
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
/*** find ***/
void editorFindCallback(char *query, int key) //callback function for editor prompt
{
  static int last_match = -1;
  static int direction = 1;
  if (key == '\r' || key == '\x1b')
  {
    last_match = -1;
    direction = 1; // setting direction to 1 makes user always search in the forward direction
    return;
  }
  else if (key == ARROW_RIGHT || key == ARROW_DOWN) // arrow keys will go to the next match
  {
    direction = 1;
  }
  else if (key == ARROW_LEFT || key == ARROW_UP) // arrow keys will go to the previous match
  {
    direction = -1;
  }
  else
  {
    last_match = -1;
    direction = 1;
  }
  if (last_match == -1)
    direction = 1;
  int current = last_match;
  int i;
  for (i = 0; i < E.numrows; i++)
  {
    current += direction;
    if (current == -1)
      current = E.numrows - 1;
    else if (current == E.numrows)
      current = 0;
    editor_row *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match)
    {
      last_match = current; // once match is found we set last match to current so if the user presses the arrow keys, we will start the next search from there
      E.cursor_y = current; // current is index of current row the user is searching
      E.cursor_x = editorRowRxToCx(row, match - row->render);
      E.rowoff = E.numrows;
      break;
    }
  }
}
void editorFind()
{
  int saved_cx = E.cursor_x; // saves the cursor position incase user clicks escape key
  int saved_cy = E.cursor_y;
  int saved_coloff = E.coloff; // saves the scroll position incase user clicks escape key
  int saved_rowoff = E.rowoff;
  char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
  if (query)
  {
    free(query);
  }
  else
  {
    E.cursor_x = saved_cx;
    E.cursor_y = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
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

void editorScroll()
{
  // ensures cursor is inside the visible window
  E.rx = 0;
  if (E.cursor_y < E.numrows)
  {
    E.rx = editorRowCxToRx(&E.row[E.cursor_y], E.cursor_x);
  }
  if (E.cursor_y < E.rowoff)
  {
    E.rowoff = E.cursor_y;
  }
  if (E.cursor_y >= E.rowoff + E.screenrows)
  {
    E.rowoff = E.cursor_y - E.screenrows + 1;
  }
  if (E.rx < E.coloff)
  {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols)
  {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void editorDrawRows(struct append_buffer *ab)
{ // handles drawing each row or column of text being edited
  int y;
  for (y = 0; y < E.screenrows; y++)
  {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows)
    {
      if (E.numrows == 0 && y == E.screenrows / 3) // welcome message only displays if the text buffer is completely empty
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
    }
    else
    {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }
    abAppend(ab, "\x1b[K", 3); // erases line one at a time

    // last line handled separately
    abAppend(ab, "\r\n", 2);
  }
}

// creating a status bar to display details about the file
void editorDrawStatusBar(struct append_buffer *ab)
{
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
                      E.cursor_y + 1, E.numrows);
  if (len > E.screencols)
    len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols)
  {
    if (E.screencols - len == rlen)
    {
      abAppend(ab, rstatus, rlen);
      break;
    }
    else
    {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct append_buffer *ab)
{
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols)
    msglen = E.screencols;
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

// displaying messages to the user using a status message
void editorSetStatusMessage(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  while (1)
  {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();
    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
    {
      if (buflen != 0)
        buf[--buflen] = '\0';
    }
    else if (c == '\x1b')
    {
      editorSetStatusMessage("");
      if (callback)
        callback(buf, c);
      free(buf);
      return NULL;
    }
    else if (c == '\r')
    {
      if (buflen != 0)
      {
        editorSetStatusMessage("");
        if (callback)
          callback(buf, c);
        return buf;
      }
    }
    else if (!iscntrl(c) && c < 128)
    {
      if (buflen == bufsize - 1)
      {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
    if (callback)
      callback(buf, c);
  }
}

void editorMoveCursor(int key)
{
  editor_row *row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
  switch (key)
  {
  case ARROW_LEFT:
    if (E.cursor_x != 0)
    {
      E.cursor_x--;
    }
    else if (E.cursor_y > 0)
    {
      E.cursor_y--;
      E.cursor_x = E.row[E.cursor_y].size;
    }
    break;
  case ARROW_RIGHT:
    if (row && E.cursor_x < row->size)
    {
      E.cursor_x++;
    }
    else if (row && E.cursor_x == row->size)
    {
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
  // snapping cursor to the end of line
  row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
  int rowlen = row ? row->size : 0;
  if (E.cursor_x > rowlen)
  {
    E.cursor_x = rowlen;
  }
}
void editorProcessKeypress() // to wait for a keypress and handles it
{
  static int quit_times = LEXI_QUIT_TIMES;
  int c = editorReadKey(); // to wait for one keypress and return it
  switch (c)
  {
  case '\r':
    editorInsertNewline();
    break;
  case CTRL_KEY('e'): // exit point
    if (E.dirty && quit_times > 0)
    {
      editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                             "Press Ctrl-E %d more times to quit.",
                             quit_times);
      quit_times--;
      return;
    }
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;

  case CTRL_KEY('s'):
    editorSave();
    break;

  case HOME_KEY:
    E.cursor_x = 0;
    break;
  case END_KEY: // moves the cursor to the end of the current line
    if (E.cursor_y < E.numrows)
      E.cursor_x = E.row[E.cursor_y].size;
    break;
  case CTRL_KEY('f'):
    editorFind();
    break;
  case BACKSPACE:
  case CTRL_KEY('h'):
  case DEL_KEY:
    if (c == DEL_KEY)
      editorMoveCursor(ARROW_RIGHT);
    editorDelChar();
    break;
  case PAGE_UP:
  case PAGE_DOWN:
  {
    if (c == PAGE_UP)
    {
      E.cursor_y = E.rowoff;
    }
    else if (c == PAGE_DOWN)
    {
      E.cursor_y = E.rowoff + E.screenrows - 1;
      if (E.cursor_y > E.numrows)
        E.cursor_y = E.numrows;
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
  case CTRL_KEY('l'):
  case '\x1b':
    break;

  default:
    editorInsertChar(c);
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
  E.row = NULL; // initializing pointer to null
  E.dirty = 0;
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
  if (argc >= 2)
  {
    editorOpen(argv[1]);
  }
  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-E = quit | Ctrl-F = find");
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
    if (c == CTRL_KEY('e'))
      break;
  }

  return 0;
}
