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
    printf("[Process 1] Starting process 1\n");
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        size_t len = strlen(buffer);
        printf("[Process 1] Read input: %s", buffer);
        if (write(fd_write, buffer, len) == -1) {
            perror("[Process 1] write failed");
            close(fd_write);
            exit(1);
        }
        printf("[Process 1] Wrote %lu bytes to pipe\n", len);
    }
    printf("[Process 1] End of input, closing pipe\n");
    close(fd_write);
    exit(0);
}

void process2(int fd_read) {
    printf("[Process 2] Starting process 2\n");
    int shmid = shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT);
    if (shmid == -1) {
        perror("[Process 2] shmget failed");
        exit(1);
    }

    char *shared_memory = (char *)shmat(shmid, NULL, 0);
    if (shared_memory == (char *)-1) {
        perror("[Process 2] shmat failed");
        exit(1);
    }
    printf("[Process 2] Shared memory attached\n");

    sem_t *sem2 = sem_open("/sem2", O_CREAT, 0666, 0);
    sem_t *sem3 = sem_open("/sem3", O_CREAT, 0666, 0);
    if (sem2 == SEM_FAILED || sem3 == SEM_FAILED) {
        perror("[Process 2] sem_open failed");
        exit(1);
    }

    char buffer[256];
    ssize_t bytes_read;
    while ((bytes_read = read(fd_read, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        char *newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';
        size_t len = strlen(buffer);

        printf("[Process 2] Read from pipe: %s (length: %lu)\n", buffer, len);

        snprintf(shared_memory, SHM_SIZE, "%lu", len);
        printf("[Process 2] Wrote length %lu to shared memory\n", len);

        sem_post(sem3); // Notify process 3
        printf("[Process 2] Signaled process 3\n");
        sem_wait(sem2); // Wait for response from process 3
        printf("[Process 2] Received signal from process 3\n");
    }

    printf("[Process 2] No more data from pipe, signaling end to process 3\n");
    strcpy(shared_memory, "END");
    sem_post(sem3);

    shmdt(shared_memory);
    close(fd_read);
    exit(0);
}

void process3() {
    printf("[Process 3] Starting process 3\n");
    int shmid = shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT);
    if (shmid == -1) {
        perror("[Process 3] shmget failed");
        exit(1);
    }

    char *shared_memory = (char *)shmat(shmid, NULL, 0);
    if (shared_memory == (char *)-1) {
        perror("[Process 3] shmat failed");
        exit(1);
    }
    printf("[Process 3] Shared memory attached\n");

    sem_t *sem2 = sem_open("/sem2", O_CREAT, 0666, 0);
    sem_t *sem3 = sem_open("/sem3", O_CREAT, 0666, 0);

    while (1) {
        sem_wait(sem3); // Wait for data from process 2
        printf("[Process 3] Signal received from process 2\n");

        if (strcmp(shared_memory, "END") == 0) {
            printf("[Process 3] Received END signal, exiting\n");
            break;
        }

        printf("[Process 3] Received length: %s\n", shared_memory);
        sem_post(sem2); // Notify process 2
        printf("[Process 3] Signaled process 2\n");
    }

    shmdt(shared_memory);
    exit(0);
}

int main() {
    printf("[Main] Starting main process\n");
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("[Main] pipe failed");
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
    printf("[Main] Process 1 finished\n");
    waitpid(pid2, NULL, 0);
    printf("[Main] Process 2 finished\n");
    waitpid(pid3, NULL, 0);
    printf("[Main] Process 3 finished\n");

    sem_unlink("/sem2");
    sem_unlink("/sem3");

    shmctl(shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT), IPC_RMID, NULL);
    printf("[Main] Cleaned up resources and exiting\n");

    return 0;
}
