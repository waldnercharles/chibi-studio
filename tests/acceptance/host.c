#include "chibi/application.h"
#include <stdbool.h>

bool studio_acceptance_frame(void *);

static int acceptance_application_frame(void *state)
{
    int status = chibi_application_frame(state);
    if (status < 0) {
        return status;
    }
    return studio_acceptance_frame(state) ? status : -1;
}

/* Preserve the real host's window, resource, frame and shutdown implementation. */
#define chibi_application_frame acceptance_application_frame
#define main studio_acceptance_host_main
#include "../../src/host/main.c"
#undef main
#undef chibi_application_frame
