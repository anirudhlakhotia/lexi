/***  includes  ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define LEXI_VERSION "0.0.1"
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

struct editorConfig
{
  int cursor_x, cursor_y; // cursor x and y
  int screenrows;
  int screencols;
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

void editorDrawRows(struct append_buffer *ab)
{ // handle drawing each row of text being edited
  int y;
  for (y = 0; y < E.screenrows; y++)
  {
    if (y == E.screenrows / 3)
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
    abAppend(ab, "\x1b[K", 3); // erases line one at a time
    if (y < E.screenrows - 1)
    { // last line handled separately
      abAppend(ab, "\r\n", 2);
    }
  }
}
void editorRefreshScreen()
{
  struct append_buffer ab = append_buffer_INIT;
  abAppend(&ab, "\x1b[?25l", 6); // hide cursor
  abAppend(&ab, "\x1b[H", 3);
  editorDrawRows(&ab);
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cursor_y + 1, E.cursor_x + 1);
  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6);      // show cursor
  write(STDOUT_FILENO, ab.b, ab.len); // write() and STDOUT_FILENO come from <unistd.h>
  abFree(&ab);
}
/*** input ***/

void editorMoveCursor(int key)
{
  switch (key)
  {
  case ARROW_LEFT:
    if (E.cursor_x != 0)
    {
      E.cursor_x--;
    }
    break;
  case ARROW_RIGHT:
    if (E.cursor_x != E.screencols - 1)
    {
      E.cursor_x++;
    }
    break;
  case ARROW_UP:
    if (E.cursor_y != 0)
    {
      E.cursor_y--;
    }
    break;
  case ARROW_DOWN:
    if (E.cursor_y != E.screenrows - 1)
    {
      E.cursor_y++;
    }
    break;
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
  case END_KEY:
    E.cursor_x = E.screencols - 1;
    break;
  case PAGE_UP:
  case PAGE_DOWN:
  {
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
  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}
int main()
{
  enableRawMode();
  initEditor();

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
