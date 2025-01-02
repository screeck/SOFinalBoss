#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <fcntl.h>

#define SHM_SIZE 1024

void process1(int fd_write) {
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        size_t len = strlen(buffer);
        if (write(fd_write, buffer, len) == -1) {
            perror("write");
            close(fd_write);
            exit(1);
        }
    }
    close(fd_write);
    exit(0);
}

void process2(int fd_read) {
    int shmid = shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }

    char *shared_memory = (char *)shmat(shmid, NULL, 0);
    if (shared_memory == (char *)-1) {
        perror("shmat");
        exit(1);
    }

    sem_t *sem2 = sem_open("/sem2", O_CREAT, 0666, 0);
    sem_t *sem3 = sem_open("/sem3", O_CREAT, 0666, 0);
    if (sem2 == SEM_FAILED || sem3 == SEM_FAILED) {
        perror("sem_open");
        exit(1);
    }

    char buffer[256];
    ssize_t bytes_read;
    while ((bytes_read = read(fd_read, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0'; // Upewniamy się, że dane są zakończone NULL
        char *newline = strchr(buffer, '\n');
        if (newline) *newline = '\0'; // Usunięcie znaku nowej linii
        size_t len = strlen(buffer);

        snprintf(shared_memory, SHM_SIZE, "%lu", len);

        sem_post(sem3); // Powiadom proces 3
        sem_wait(sem2); // Czekaj na odpowiedź procesu 3
    }

    strcpy(shared_memory, "END"); // Sygnalizacja końca
    sem_post(sem3);

    shmdt(shared_memory);
    close(fd_read);
    exit(0);
}

void process3() {
    int shmid = shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }

    char *shared_memory = (char *)shmat(shmid, NULL, 0);
    if (shared_memory == (char *)-1) {
        perror("shmat");
        exit(1);
    }

    sem_t *sem2 = sem_open("/sem2", O_CREAT, 0666, 0);
    sem_t *sem3 = sem_open("/sem3", O_CREAT, 0666, 0);

    while (1) {
        sem_wait(sem3); // Czekaj na dane od procesu 2

        if (strcmp(shared_memory, "END") == 0) {
            break;
        }

        printf("Proces 3 odebrał: %s znaków\n", shared_memory);
        sem_post(sem2); // Powiadom proces 2
    }

    shmdt(shared_memory);
    exit(0);
}

int main() {
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("pipe");
        exit(1);
    }

    pid_t pid1 = fork();
    if (pid1 == 0) {
        close(pipe_fd[0]);
        process1(pipe_fd[1]);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        close(pipe_fd[1]);
        process2(pipe_fd[0]);
    }

    pid_t pid3 = fork();
    if (pid3 == 0) {
        process3();
    }

    close(pipe_fd[0]);
    close(pipe_fd[1]);

    waitpid(pid1, NULL, 0);
    waitpid(pid2, NULL, 0);
    waitpid(pid3, NULL, 0);

    sem_unlink("/sem2");
    sem_unlink("/sem3");

    shmctl(shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT), IPC_RMID, NULL);

    return 0;
}
