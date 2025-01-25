#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <semaphore.h>

// ---------------------------------
// Konfiguracje
// ---------------------------------

#define SHM_SIZE 1024

// Struktura wiadomości w kolejce
struct msgBuf {
    long mtype;    // typ wiadomości
    int  signo;    // numer sygnału
};

int  msgid;        // ID kolejki komunikatów
int  shmid;        // ID pamięci współdzielonej
pid_t mainPID;     // PID procesu macierzystego
pid_t p1PID, p2PID, p3PID; // PID-y procesów

// Semafory
sem_t *sem2 = NULL;
sem_t *sem3 = NULL;

// ---------------------------------
// Funkcje pomocnicze do kolejki
// ---------------------------------

void sendSignalToQueue(int signo, long mtype) {
    struct msgBuf message;
    message.mtype = mtype;
    message.signo = signo;
    if (msgsnd(msgid, &message, sizeof(message.signo), 0) == -1) {
        perror("[sendSignalToQueue] msgsnd error");
    }
}

int receiveSignalFromQueue(long mtype) {
    struct msgBuf message;
    if (msgrcv(msgid, &message, sizeof(message.signo), mtype, 0) == -1) {
        perror("[receiveSignalFromQueue] msgrcv error");
        return -1;
    }
    return message.signo;
}

// ---------------------------------
// Deklaracje handlerów sygnałów
// ---------------------------------

void sigusr2_handler_main(int signo);
void sigusr1_handler_p1(int signo);
void sigusr1_handler_p2(int signo);
void sigusr1_handler_p3(int signo);
void sigtstp_handler_p3(int signo);

// ---------------------------------
// Proces 1
// ---------------------------------

void process1(int fd_write) {
    p1PID = getpid();
    printf("[Process 1] Starting (PID=%d)\n", p1PID);

    // Każdy proces potomny wchodzi do własnej grupy procesów
    setpgid(0, 0);

    // Ustawiamy handler sygnału SIGUSR1
    signal(SIGUSR1, sigusr1_handler_p1);

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

// ---------------------------------
// Proces 2
// ---------------------------------

void process2(int fd_read) {
    p2PID = getpid();
    printf("[Process 2] Starting (PID=%d)\n", p2PID);

    // Każdy proces potomny wchodzi do własnej grupy procesów
    setpgid(0, 0);

    // Ustawiamy handler sygnału SIGUSR1
    signal(SIGUSR1, sigusr1_handler_p2);

    // Inicjalizacja pamięci współdzielonej
    shmid = shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT);
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

    sem2 = sem_open("/sem2", O_CREAT, 0666, 0);
    sem3 = sem_open("/sem3", O_CREAT, 0666, 0);
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

        sem_post(sem3);
        printf("[Process 2] Signaled process 3 (sem3)\n");
        sem_wait(sem2);
        printf("[Process 2] Received signal from process 3 (sem2)\n");
    }

    printf("[Process 2] No more data from pipe, signaling end to process 3\n");
    strcpy(shared_memory, "END");
    sem_post(sem3);

    shmdt(shared_memory);
    close(fd_read);
    exit(0);
}

// ---------------------------------
// Proces 3
// ---------------------------------

void process3() {
    p3PID = getpid();
    printf("[Process 3] Starting (PID=%d)\n", p3PID);

    // Każdy proces potomny wchodzi do własnej grupy procesów
    setpgid(0, 0);

    // Obsługa sygnałów: SIGUSR1 (łańcuch powiadomień) i SIGTSTP (od użytkownika)
    signal(SIGUSR1, sigusr1_handler_p3);
    signal(SIGTSTP, sigtstp_handler_p3);

    // Pamięć współdzielona
    shmid = shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT);
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

    sem2 = sem_open("/sem2", O_CREAT, 0666, 0);
    sem3 = sem_open("/sem3", O_CREAT, 0666, 0);
    if (sem2 == SEM_FAILED || sem3 == SEM_FAILED) {
        perror("[Process 3] sem_open failed");
        exit(1);
    }

    // Pętla odczytująca dane z pamięci współdzielonej
    while (1) {
        sem_wait(sem3); 
        printf("[Process 3] sem3 received\n");

        if (strcmp(shared_memory, "END") == 0) {
            printf("[Process 3] Received END signal, exiting\n");
            break;
        }

        printf("[Process 3] Received length: %s\n", shared_memory);
        sem_post(sem2);
        printf("[Process 3] Signaled process 2 (sem2)\n");
    }

    shmdt(shared_memory);
    exit(0);
}

// ---------------------------------
// Handlery sygnałów
// ---------------------------------

// (1) Handler w Main dla sygnału SIGUSR2 (wysyłany przez Process 3)
void sigusr2_handler_main(int signo) {
    printf("[Main] Otrzymano sygnał SIGUSR2 od Process 3. Zapisuję sygnał (SIGTSTP) do kolejki.\n");

    // Zapisujemy w kolejce komunikat, np. SIGTSTP=20
    sendSignalToQueue(SIGTSTP, 1);

    // Powiadomienie procesu 1 (SIGUSR1)
    printf("[Main] Wysyłam SIGUSR1 do Process 1 (PID=%d)\n", p1PID);
    kill(p1PID, SIGUSR1);
}

// (2) Handler w Procesie 1 (SIGUSR1)
// - odczyt sygnału z kolejki (mtype=1)
// - wstawienie go ponownie (mtype=2) dla procesu 2
// - wysłanie SIGUSR1 do procesu 2
// - samemu: kill(getpid(), SIGTSTP)
void sigusr1_handler_p1(int signo) {
    printf("[Process 1] Otrzymałem SIGUSR1.\n");

    int sig_from_queue = receiveSignalFromQueue(1);
    if (sig_from_queue == SIGTSTP) {
        printf("[Process 1] W kolejce był sygnał SIGTSTP (%d). Przekazuję dalej.\n", sig_from_queue);

        // Odkładamy sygnał do kolejki z mtype=2
        sendSignalToQueue(sig_from_queue, 2);

        // Powiadomienie procesu 2
        printf("[Process 1] Wysyłam SIGUSR1 do Process 2 (PID=%d)\n", p2PID);
        kill(p2PID, SIGUSR1);

        // Zatrzymanie się przez SIGTSTP (zamiast SIGSTOP)
        printf("[Process 1] Wykonuję SIGTSTP.\n");
        kill(getpid(), SIGTSTP);
    }
}

// (3) Handler w Procesie 2 (SIGUSR1)
// - odczyt sygnału (mtype=2)
// - wstawienie do kolejki (mtype=3)
// - SIGUSR1 do Process 3
// - kill(getpid(), SIGTSTP)
void sigusr1_handler_p2(int signo) {
    printf("[Process 2] Otrzymałem SIGUSR1.\n");

    int sig_from_queue = receiveSignalFromQueue(2);
    if (sig_from_queue == SIGTSTP) {
        printf("[Process 2] W kolejce był sygnał SIGTSTP (%d). Przekazuję dalej.\n", sig_from_queue);

        // Odkładamy do kolejki dla procesu 3
        sendSignalToQueue(sig_from_queue, 3);

        // Powiadamiamy proces 3
        printf("[Process 2] Wysyłam SIGUSR1 do Process 3 (PID=%d)\n", p3PID);
        kill(p3PID, SIGUSR1);

        // Zatrzymanie przez SIGTSTP
        printf("[Process 2] Wykonuję SIGTSTP.\n");
        kill(getpid(), SIGTSTP);
    }
}

// (4) Handler w Procesie 3 (SIGUSR1)
// - odczyt sygnału (mtype=3)
// - SIGTSTP na siebie
void sigusr1_handler_p3(int signo) {
    printf("[Process 3] Otrzymałem SIGUSR1.\n");

    int sig_from_queue = receiveSignalFromQueue(3);
    if (sig_from_queue == SIGTSTP) {
        printf("[Process 3] W kolejce był sygnał SIGTSTP (%d). Zatrzymuję się.\n", sig_from_queue);
        kill(getpid(), SIGTSTP);
    }
}

// (5) Handler w Procesie 3 (SIGTSTP) - gdy dostaje go „z zewnątrz” (np. Ctrl+Z)
void sigtstp_handler_p3(int signo) {
    printf("[Process 3] Otrzymałem SIGTSTP od użytkownika. Informuję Main przez SIGUSR2.\n");
    // Wysyłamy do Main informację, że 3 dostał TSTP
    kill(mainPID, SIGUSR2);
    // Uwaga: nie wywołujemy tu od razu kill(getpid(), SIGTSTP),
    // bo chcemy, aby Main zdążył wpisać do kolejki i powiadomić
    // proces 1 o SIGTSTP. Logikę można dopasować wg potrzeb.
}

// ---------------------------------
// main
// ---------------------------------

int main() {
    printf("[Main] Starting main process (PID=%d)\n", getpid());
    mainPID = getpid();

    // Kolejka komunikatów
    key_t msgKey = ftok("msgqueue", 65);
    msgid = msgget(msgKey, 0666 | IPC_CREAT);
    if (msgid == -1) {
        perror("[Main] msgget failed");
        exit(1);
    }

    // Handler dla sygnału SIGUSR2 (od procesu 3)
    signal(SIGUSR2, sigusr2_handler_main);

    // Potok
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("[Main] pipe failed");
        exit(1);
    }

    // Proces 1
    pid_t pid1 = fork();
    if (pid1 == 0) {
        close(pipe_fd[0]);
        process1(pipe_fd[1]);
    }
    p1PID = pid1;

    // Proces 2
    pid_t pid2 = fork();
    if (pid2 == 0) {
        close(pipe_fd[1]);
        process2(pipe_fd[0]);
    }
    p2PID = pid2;

    // Proces 3
    pid_t pid3 = fork();
    if (pid3 == 0) {
        process3();
    }
    p3PID = pid3;

    close(pipe_fd[0]);
    close(pipe_fd[1]);

    // Czekamy na zakończenie procesów potomnych
    waitpid(pid1, NULL, 0);
    printf("[Main] Process 1 finished\n");
    waitpid(pid2, NULL, 0);
    printf("[Main] Process 2 finished\n");
    waitpid(pid3, NULL, 0);
    printf("[Main] Process 3 finished\n");

    // Usuwamy semafory
    sem_unlink("/sem2");
    sem_unlink("/sem3");

    // Usuwamy pamięć współdzieloną
    shmctl(shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT), IPC_RMID, NULL);

    // Usuwamy kolejkę komunikatów
    msgctl(msgid, IPC_RMID, NULL);

    printf("[Main] Cleaned up resources and exiting\n");
    return 0;
}
