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
#include <stdio.h>

void print_with_carriage_returns(const char* text) {
    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] == '\n') {
            putchar('\r');  // Insert the carriage return before the newline
        }
        putchar(text[i]);
    }
}

int main() {
    const char* original_text = "Hello\nWorld\nThis is a test.";

    printf("Original text:\n%s\n\n", original_text);
    printf("Text with carriage returns:\n");
    print_with_carriage_returns(original_text);

    return 0;
}