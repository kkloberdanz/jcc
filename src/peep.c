#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "peep.h"
#include "util.h"

char *peep_lines[PEEP_MAX];
int peep_n = 0;

static FILE *peep_out_file = NULL;
static int (*peep_rules)(void) = NULL;
static int peep_enabled = 1;

void peep_begin(FILE *out, int (*rules)(void), int enabled) {
    peep_out_file = out;
    peep_rules = rules;
    peep_enabled = enabled;
}

/* write the first `count` lines and drop them from the window */
static void peep_out(int count) {
    int i;
    for (i = 0; i < count; i++) {
        fputs(peep_lines[i], peep_out_file);
        fputc('\n', peep_out_file);
        free(peep_lines[i]);
    }
    memmove(peep_lines, peep_lines + count, (size_t)(peep_n - count) * sizeof(*peep_lines));
    peep_n -= count;
}

void peep_flush(void) {
    peep_out(peep_n);
}

void peep_replace(int from, int count, const char *const *with, int count_with) {
    int i;
    for (i = from; i < from + count; i++) {
        free(peep_lines[i]);
    }
    memmove(peep_lines + from + count_with, peep_lines + from + count,
            (size_t)(peep_n - from - count) * sizeof(*peep_lines));
    for (i = 0; i < count_with; i++) {
        peep_lines[from + i] = xstrdup(with[i]);
    }
    peep_n += count_with - count;
}

void peep_add(const char *line) {
    if (!peep_enabled || line[0] != '\t' || line[1] == '.') {
        /* a label or directive: a barrier */
        peep_flush();
        fputs(line, peep_out_file);
        fputc('\n', peep_out_file);
        return;
    }
    if (peep_n == PEEP_MAX) {
        peep_out(PEEP_MAX / 2);
    }
    peep_lines[peep_n++] = xstrdup(line);
    while (peep_rules()) {
        /* keep going while something changed */
    }
}
