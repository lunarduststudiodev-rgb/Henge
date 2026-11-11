#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int counter = 41;
static const char *greeting = "hello from a Mac binary";
__thread int tls_value = 7;
__attribute__((constructor)) static void init(void) { counter++; }

int main(int argc, char **argv) {
    char *copy = strdup(greeting);
    printf("%s, counter %d, tls %d, args %d\n", copy, counter, tls_value, argc);
    for (int i = 1; i < argc; i++) printf("arg %s\n", argv[i]);
    free(copy);
    tls_value++;
    printf("PASS hello %d\n", tls_value);
    return 0;
}
