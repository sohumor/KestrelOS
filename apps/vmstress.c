/* vmstress - exercise lazy heap commitment and, at larger sizes, swap. */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>

#define CHUNK_MIB 4
#define MAX_CHUNKS 32
#define PAGE_SIZE 4096UL

int main(int argc, char **argv)
{
    unsigned long mib = 8;
    unsigned char *chunks[MAX_CHUNKS];
    unsigned long nchunks;

    if (argc > 1) {
        int n = atoi(argv[1]);
        if (n > 0)
            mib = (unsigned long)n;
    }
    nchunks = (mib + CHUNK_MIB - 1) / CHUNK_MIB;
    if (nchunks > MAX_CHUNKS) {
        printf("vmstress: maximum is %d MiB\n", MAX_CHUNKS * CHUNK_MIB);
        return 1;
    }

    for (unsigned long i = 0; i < nchunks; i++) {
        chunks[i] = malloc(CHUNK_MIB * 1024UL * 1024UL);
        if (!chunks[i]) {
            printf("vmstress: allocation failed after %lu MiB\n",
                   i * CHUNK_MIB);
            return 1;
        }
        for (unsigned long off = 0; off < CHUNK_MIB * 1024UL * 1024UL;
             off += PAGE_SIZE)
            chunks[i][off] = (unsigned char)(i ^ (off / PAGE_SIZE) ^ 0xA5);
    }

    /* Walk backwards so a pressure run must bring older pages back in. */
    for (unsigned long i = nchunks; i-- > 0;) {
        for (unsigned long off = 0; off < CHUNK_MIB * 1024UL * 1024UL;
             off += PAGE_SIZE) {
            unsigned char want =
                (unsigned char)(i ^ (off / PAGE_SIZE) ^ 0xA5);
            if (chunks[i][off] != want) {
                printf("vmstress: corruption at chunk %lu page %lu\n",
                       i, off / PAGE_SIZE);
                return 1;
            }
        }
    }

    printf("vmstress: demand paging verified across %lu MiB\n",
           nchunks * CHUNK_MIB);
    return 0;
}
