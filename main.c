#define _GNU_SOURCE

#include "lib.h"

int main()
{
    struct qsbr *qsbr = qsbr_create();

    free(qsbr);
    return 0;
}