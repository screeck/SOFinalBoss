#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <fcntl.h>

static volatile sig_atomic_t g_paused = 0;
static volatile sig_atomic_t g_terminate = 0;

// Handler dla SIGTSTP 
void handle_sigtstp(int signo) {
    (void)signo;
    g_paused = 1;
}

// Handler dla SIGCONT 
void handle_sigcont(int signo) {
    (void)signo;
    g_paused = 0;
}

// Handler dla SIGTERM 
void handle_sigterm(int signo) {
    (void)signo;
    g_terminate = 1;
}

int main(int argc, char *argv[])
{
    // Instalacja handlerow sygnalow
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = handle_sigtstp;
    sigaction(SIGTSTP, &sa, NULL);

    sa.sa_handler = handle_sigcont;
    sigaction(SIGCONT, &sa, NULL);

    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, NULL);

    // Otwarcie pamieci wspoldzielonej
    int shmid = shmget(ftok("shmfile", 65), 1024, 0666 | IPC_CREAT);
    if (shmid == -1) {
        perror("shmget");
        exit(EXIT_FAILURE);
    }

    char *shared_memory = (char *)shmat(shmid, NULL, 0);
    if (shared_memory == (char *)-1) {
        perror("shmat");
        exit(EXIT_FAILURE);
    }

    // Otwarcie semaforow
    sem_t *sem2 = sem_open("/sem2", 0);
    sem_t *sem3 = sem_open("/sem3", 0);
    if (sem2 == SEM_FAILED || sem3 == SEM_FAILED) {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }

    // Glowna petla czytania z stdin i wysylania do proces3
    char buffer[1024];
    while (!g_terminate) {
        // Jesli proces jest wstrzymany, czekamy na sygnal wznowienia lub zakonczenia
        while (g_paused && !g_terminate) {
            pause();
        }
        if (g_terminate) {
            break;
        }

        // Proba odczytu linii ze standardowego wejscia
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            break;
        }

        strcpy(shared_memory, buffer);

        // Powiadamiamy proces 3
        sem_post(sem3);

        // Czekamy na sygnal od procesu 3, ze zakonczyl przetwarzanie
        sem_wait(sem2);
    }

    // Sygnalizacja konca (pusty napis w shared_memory)
    shared_memory[0] = 'z0';
    sem_post(sem3);

    // Sprzatanie
    shmdt(shared_memory);
    sem_close(sem2);
    sem_close(sem3);

    return 0;
}
