#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <fcntl.h>

#define SHM_SIZE 1024

// Funkcja dla Procesu 3
void process3() {
    // Otwieranie pamięci współdzielonej i semafora
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

    while (1) {
        sem_wait(sem3); // Czekaj na sygnał od Procesu 2

        // Sprawdzenie zakończenia
        if (shared_memory[0] == '\0') {
            break;
        }

        printf("Proces 3 odebrał: %s\n", shared_memory);

        sem_post(sem2); // Informuje Proces 2 o zakończeniu pracy
    }

    // Sprzątanie
    shmdt(shared_memory);
    sem_close(sem2);
    sem_close(sem3);
}

// Proces macierzysty
int main() {
    // Tworzenie pamięci współdzielonej
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

    // Tworzenie semaforów
    sem_t *sem1 = sem_open("/sem1", O_CREAT, 0666, 1);
    sem_t *sem2 = sem_open("/sem2", O_CREAT, 0666, 0);
    sem_t *sem3 = sem_open("/sem3", O_CREAT, 0666, 0);

    if (sem1 == SEM_FAILED || sem2 == SEM_FAILED || sem3 == SEM_FAILED) {
        perror("sem_open");
        exit(1);
    }

    // Tworzenie procesów
    pid_t pid1 = fork();
    if (pid1 == 0) {
        execlp("./process1", "process1", NULL);
        perror("execlp");
        exit(1);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        execlp("./process2", "process2", NULL);
        perror("execlp");
        exit(1);
    }

    pid_t pid3 = fork();
    if (pid3 == 0) {
        process3();
        exit(0);
    }

    // Oczekiwanie na zakończenie procesów
    waitpid(pid1, NULL, 0);
    waitpid(pid2, NULL, 0);
    waitpid(pid3, NULL, 0);

    // Sprzątanie
    sem_unlink("/sem1");
    sem_unlink("/sem2");
    sem_unlink("/sem3");

    shmctl(shmid, IPC_RMID, NULL);

    return 0;
}
