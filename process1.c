#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "U¿ycie: %s <fd_write>\n", argv[0]);
        exit(1);
    }

    // deskryptor potoku do zapisu
    int fd_write = atoi(argv[1]);

    // Przyk³ad: czytamy ze standardowego wejœcia linia po linii
    // i przekazujemy do procesu 2
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        size_t len = strlen(buffer);
        if (write(fd_write, buffer, len) == -1) {
            perror("write");
            close(fd_write);
            exit(1);
        }
    }

    // Koñczymy zapis, zamykaj¹c deskryptor
    close(fd_write);
    return 0;
}
