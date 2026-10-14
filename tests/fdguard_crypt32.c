/* Stand-in for Wine's crypt32.so in tests/test_fdguard.py: built as a library named
 * crypt32.so. Reads a file the way import_certs_from_path() does: fdopen(), fclose(),
 * then close() of the same fd. between() runs where another thread could take the
 * number. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int fake_import_certs(const char *path, void (*between)(void))
{
    int fd = open(path, O_RDONLY);
    FILE *fp = fdopen(fd, "r");
    if (!fp) return -2;
    fclose(fp);
    if (between) between();
    return close(fd);
}
