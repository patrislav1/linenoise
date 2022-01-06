#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "linenoise.h"

/* Functions your implementation needs to provide */

/* linenoise_completion: Optional
 * Provide completions to expand when Tab is pressed
 */

void linenoise_completion(const char *buf, linenoiseCompletions *lc) {
    if (buf[0] == 'h') {
        linenoiseAddCompletion(lc,"hello");
        linenoiseAddCompletion(lc,"hello there");
    }
    if (!strcasecmp(buf, "/q")) {
        linenoiseAddCompletion(lc,"/quit");
    }
    if (!strcasecmp(buf, "/c")) {
        linenoiseAddCompletion(lc,"/count");
    }
}

/* linenoise_hints: Optional
 * Provide hints to pop up as you type
 */

const char *world[] = { "World", "- Displays a traditional greeting"};
const char *quit[] = { "/Quit", "- Exits this example"};
const char *count[] = { "/Count", "- Prints the background counter"};

const char **linenoise_hints(const char *buf) {
    if (!strcasecmp(buf,"hello")) {
        return world;
    }
    if (!strcasecmp(buf,"/q")) {
        return quit;
    }
    if (!strcasecmp(buf,"/c")) {
        return count;
    }
    return NULL;
}

/* linenoise_getch: Required
 * Returns a character from the keyboard device if one is available,
 * otherwise -1 if nothing has been entered
 */

int linenoise_getch(void) {
    static int i=0;

    /* Linux doesn't have a nonblocking keyboard scan function like kbhit()
     * so simulate that by returning no character 99 times out of 100 and
     * blocking for the 100th.  This means the main loop counter should be
     * 100 times the number of keys pressed */
    i++;
    if (i % 100==0) {
      i=0;
      return getchar();
    } else {
      return -1;
    }
}

/* linenoise_write: Required
 * User-provided console write() function, bounded by length
 */
void linenoise_write(const char *buf, size_t n) {
    printf("%.*s", (int) n, buf);
}



int main(int argc, char **argv) {
    char *prgname = argv[0];
    char line[1024];

    /* Parse options */
    while(argc > 1) {
        argc--;
        argv++;
        if (!strcmp(*argv,"--keycodes")) {
            linenoisePrintKeyCodes();
            exit(0);
        } else {
            fprintf(stderr, "Usage: %s [--keycodes]\n", prgname);
            exit(1);
        }
    }

    printf("Press Ctrl-D or type '/quit' to quit\r\n");
    printf("Unix users: Make sure terminal is in raw mode: eg 'stty raw -echo'\r\n");
    printf("When you have quit, blind-type 'reset' to reset your terminal\r\n");

    /* Load history from file. The history file is just a plain text file
     * where entries are separated by newlines. */
    linenoiseHistoryLoad("history.txt");
    /* If we don't have history.txt, need at least one existing history
    entry to set up the buffer correctly */
    linenoiseHistoryAdd("previously-entered");

    /* Now this is the main loop of the typical linenoise-based application.
     * linenoiseEdit won't block to allow other work to continue if no keys are pressed
     */

     int ret = 0;
     int something_else = 0;
     do {
        ret = linenoiseEdit(line, sizeof(line),"hello> ");
        /* ret is the number of characters returned,
         * -1 that the line was incomplete, -2 for Ctrl-D was pressed.
         * Do something with the returned string if valid. */
        if (ret > 0) {
            if (line[0] != '\0' && line[0] != '/') {
                printf("\r\necho: '%s'\r\n", line);
                linenoiseHistoryAdd(line); /* Add to the history. */
                linenoiseHistorySave("history.txt"); /* Save the history on disk. */
            } else if (!strncmp(line,"/historylen",11)) {
                /* The "/historylen" command will change the history len. */
                int len = atoi(line+11);
                linenoiseHistorySetMaxLen(len);
            } else if (!strncmp(line,"/count",6)) {
                /* Print the counter that's our background work to do */
                printf("\r\nCounter: %d\r\n", something_else);
            } else if (!strncmp(line,"/quit",5)) {
                printf("\r\nQuit command received. Exiting now.\r\n");
                ret = -2;
            } else if (line[0] == '/') {
                printf("\r\nUnreconized command: %s\r\n", line);
            }
        }
        /* Do some other work in the meantime, to show that linenoiseEdit doesn't block
         * (although your implementation of linenoise_getch() might, as above)
         */
        something_else++;
    } while (ret != -2);

    return 0;
}
