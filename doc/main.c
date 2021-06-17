/* simple driver for C code */
#include <inttypes.h>
#include "lzjb.c"

#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>

static char buffer[32768];

int sread (uchar_t *dst, void *fops_ctx, const size_t len, const size_t offset)
{
    int rc = len;

    uchar_t *src = fops_ctx;

    for (size_t i = offset; i < (offset+len); i++) {
        *dst++ =  src[i];
    }

    return rc;
}

int dread (uchar_t *dst, void *fops_ctx, const size_t len, const size_t offset)
{
    int rc = len;

    uchar_t *src = fops_ctx;

    for (size_t i = offset; i < (offset+len); i++) {
        *dst++ =  src[i];
    }

    return rc;
}

int dwrite (void *fops_ctx, const uchar_t *src, const size_t len, const size_t offset)
{
    int rc = len;

    uchar_t *dst = fops_ctx;

    for (size_t i = offset; i < (offset+len); i++) {
        dst[i] =  *src++;
    }

    return rc;
}

uchar_t* writesize(off_t size, int first, uchar_t *dst) {
    uchar_t lsb = size & 0x7F;
    size >>= 7;
    if (size) {
        dst = writesize(size, 0, dst);
    }
    if (first) lsb |= 0x80;
    *dst = lsb;
    return dst+1;
}

int main(int argc, char **argv) {
    int compress = 1, firstarg=1;
    char *infile, *outfile;
    int in_fd, out_fd;
    void *inbuf, *outbuf;
    off_t insize, outsize;

    if (argc>1) {
        if (strcmp("-d", argv[1])==0) {
            compress = 0;
            firstarg++;
        } else if (strcmp("-z", argv[1])==0) {
            compress = 1;
            firstarg++;
        }
    }
    if ((argc-firstarg) < 2) {
        fprintf(stderr, "Usage: %s -d|-z <infile> <outfile>\n", argv[0]);
        return 1;
    }
    infile = argv[firstarg];
    outfile = argv[firstarg+1];

    // snarf in input
    in_fd = open(infile, O_RDONLY);
    if (in_fd < 0) {
        fprintf(stderr, "Can't open %s for reading.\n", infile);
        return 1;
    }
    {
        struct stat s;
        int st;
        st = fstat(in_fd, &s);
        if (st!=0 || s.st_size==0) {
            fprintf(stderr, "Can't read file size of %s.\n", infile);
            return 1;
        }
        insize = s.st_size;
    }
    inbuf = mmap(NULL, insize, PROT_READ, MAP_SHARED, in_fd, 0);
    if (inbuf == MAP_FAILED) {
        fprintf(stderr, "Can't mmap %s\n", infile);
        return 1;
    }
    if (compress) {
        uchar_t *dst;
        off_t outsize_guess;
        out_fd = open(outfile, O_RDWR|O_CREAT, 0666);
        if (out_fd < 0) {
            fprintf(stderr, "Can't open %s for writing.\n", outfile);
            return 1;
        }
        // compressed file can't be bigger than 9/8 * infile + (size bytes)
        outsize_guess = insize + (insize/8) + 17 /*2*NBBY+1*/ + 10 /*size*/;
        ftruncate(out_fd, outsize_guess);
        outbuf = mmap(NULL, outsize_guess, PROT_READ|PROT_WRITE, MAP_SHARED,
                      out_fd, 0);
        if (outbuf == MAP_FAILED) {
            fprintf(stderr, "Can't mmap %s\n", outfile);
            return 1;
        }
        // write the input file size
        dst = writesize(insize+1, 1, (uchar_t*)outbuf);
        // now write the compressed file
        outsize = lzjb_compress(inbuf, dst, insize,
                                (uchar_t*)outbuf+outsize_guess-dst, 0);
        // truncate output
        munmap(outbuf, outsize_guess);
        ftruncate(out_fd, (dst + outsize) - (uchar_t*)outbuf);
        close(out_fd);
    } else { // decompress
        uchar_t *src;
        // read the output file size
        outsize = 0;
        src = inbuf;
        for (src=inbuf; ; src++) {
            if ((*src) & 0x80) {
                outsize |= (*src) & 0x7F;
                break;
            } else {
                outsize = (outsize | (*src)) << 7;
            }
        }
        outsize--; src++;
        if (outsize < 0) {
            fprintf(stderr, "Streaming decompression not implemented.\n");
            return 1;
        }
        // create an output file of the appropriate length
        out_fd = open(outfile, O_RDWR|O_CREAT, 0666);
        if (out_fd < 0) {
            fprintf(stderr, "Can't open %s for writing.\n", outfile);
            return 1;
        }
        ftruncate(out_fd, outsize);
        outbuf = mmap(NULL, outsize, PROT_READ|PROT_WRITE, MAP_SHARED,
                      out_fd, 0);
        if (outbuf == MAP_FAILED) {
            fprintf(stderr, "Can't mmap %s\n", outfile);
            return 1;
        }

        size_t s_len = ((size_t)inbuf) + insize - ((size_t)src);
        printf("[DEBUG] s_len := %zu\n", s_len);
        printf("[DEBUG] outsize := %zu\n", outsize);

        // now decompress!
        if (1) {
            lz_ctx_t lz_ctx;
            int cnt = 0;

            memset(&buffer[0], 0, sizeof(buffer)/sizeof(buffer[0]));

            lz_ctx.sread = sread;
            lz_ctx.dread = dread;
            lz_ctx.dwrite = dwrite;
            lz_ctx.sctx = src;
            lz_ctx.dctx = &buffer[0];
            lz_ctx.scnt = 0;
            lz_ctx.dcnt = 0;
            lz_ctx.dlen = outsize;
            lz_ctx.copymap = 0;
            lz_ctx.copymask = 1 << (NBBY - 1);

            int d_len = outsize;
            do {
                cnt = lzjb_decompress_ext(&lz_ctx, 128U);
                if (cnt > 0)    d_len -= cnt;
                else            break;
            } while (d_len > 0);

            if (!d_len) {
                printf("\n"
                        "==== Beginning of content ===="
                        "\n"
                        "%s"
                        "======= End of content ======="
                        "\n",
                        &buffer[0U]);
            }
            else {
                printf("=== Inflating failed with %i ===\n", d_len);
            }
        }

        lzjb_decompress(src, outbuf, s_len, outsize, 0);

        // unmap, close
        munmap(outbuf, outsize);
        close(out_fd);
    }
    munmap(inbuf, insize);
    close(in_fd);
    return 0;
}
