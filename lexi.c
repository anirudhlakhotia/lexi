/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

struct termios orig_termios;

/*** terminal ***/
// Terminal starts in canonical mode by default (Input sent only when enter is pressed)
// For a text editor, raw mode is used to allow input to be sent when any key is pressed

void die(const char *s)
{
  perror(s); // print error message based on global errno
  exit(1);
}

void disableRawMode()
{
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) // tcgetattr() for setting terminal attributes
    die("tcsetattr");
}

void enableRawMode()
{
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = orig_termios;                        // copy of original terminal state stored in orig_termios
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // disable all input processing  (input flags)
  raw.c_oflag &= ~(OPOST);                                  // disable all output processing (output flags) (\n to \r\n translation)
  raw.c_cflag |= (CS8);                                     // 8-bit chars
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);          // disable echo, canonical input, extended input, and signal processing i.e. ctrl+C, ctrl+Z (defined in termios.h)
  raw.c_cc[VMIN] = 0;                                       // minimum number of characters to read before returning
  raw.c_cc[VTIME] = 1;                                      // timeout in deciseconds before returning

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

/*** init ***/

int main()
{
  enableRawMode();

  while (1)
  {
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
    if (c == 'q')
      break;
  }

  return 0;
}
