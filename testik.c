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
#include <errno.h>

// ---------------------------------
// Konfiguracje
// ---------------------------------
#define SHM_SIZE 1024

// Struktura wiadomości w kolejce
struct msgBuf {
    long mtype;    // typ wiadomości (np. 1,2,3)
    int  signo;    // numer sygnału (np. SIGTSTP = 20)
};

// Zmienne globalne
int  msgid;      // ID kolejki komunikatów
int  shmid;      // ID pamięci współdzielonej
pid_t mainPID;   // PID procesu macierzystego
pid_t p1PID, p2PID, p3PID; // PID-y procesów 1,2,3

// Semafory do komunikacji p2 <-> p3
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
    // Każdy proces potomny w osobnej grupie:
    setpgid(0, 0);

    printf("[Process 1] Starting (PID=%d)\n", p1PID);

    // Obsługa SIGUSR1 (powiadomienie z Main)
    signal(SIGUSR1, sigusr1_handler_p1);

    char buffer[256];
    while (1) {
        if (!fgets(buffer, sizeof(buffer), stdin)) {
            if (feof(stdin)) {
                printf("[Process 1] End of input (EOF), closing pipe\n");
            } else {
                perror("[Process 1] Error reading from stdin");
            }
            break;
        }

        size_t len = strlen(buffer);
        printf("[Process 1] Read input: %s", buffer);

        if (write(fd_write, buffer, len) == -1) {
            perror("[Process 1] write failed");
            close(fd_write);
            exit(1);
        }
        printf("[Process 1] Wrote %lu bytes to pipe\n", len);
    }

    close(fd_write);
    exit(0);
}

// Handler w Procesie 1 dla SIGUSR1
// Odczytuje sygnał z kolejki (typ=1), jeśli to SIGTSTP => przekazuje do p2 (typ=2), po czym sam się zatrzymuje.
void sigusr1_handler_p1(int signo) {
    printf("[Process 1] Received SIGUSR1 -> reading from queue (mtype=1)\n");
    int sig_from_queue = receiveSignalFromQueue(1);
    if (sig_from_queue == SIGTSTP) {
        printf("[Process 1] Detected SIGTSTP in queue -> notifying Process 2\n");

        // Wstawiamy ponownie do kolejki (typ=2), aby p2 mogło odczytać
        sendSignalToQueue(sig_from_queue, 2);

        // Powiadomienie procesu 2
        kill(p2PID, SIGUSR1);

        // Zatrzymanie (SIGSTOP) tylko procesu 1
        printf("[Process 1] Stopping (SIGSTOP)\n");
        kill(getpid(), SIGSTOP);
    }
}

// ---------------------------------
// Proces 2
// ---------------------------------
void process2(int fd_read) {
    p2PID = getpid();
    // Każdy proces potomny w osobnej grupie:
    setpgid(0, 0);

    printf("[Process 2] Starting (PID=%d)\n", p2PID);

    // Obsługa SIGUSR1
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

    // Semafory
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

        // Powiadomienie p3 przez semafor
        sem_post(sem3);
        printf("[Process 2] Signaled process 3 (sem3)\n");

        // Oczekiwanie na sem2
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

// Handler w Procesie 2 dla SIGUSR1
// Odczytuje SIGTSTP (typ=2) -> przekazuje do p3 (typ=3), a następnie SIGSTOP.
void sigusr1_handler_p2(int signo) {
    printf("[Process 2] Received SIGUSR1 -> reading from queue (mtype=2)\n");
    int sig_from_queue = receiveSignalFromQueue(2);
    if (sig_from_queue == SIGTSTP) {
        printf("[Process 2] Detected SIGTSTP in queue -> notifying Process 3\n");

        // Przekazujemy do kolejki (typ=3)
        sendSignalToQueue(sig_from_queue, 3);

        // Powiadomienie procesu 3
        kill(p3PID, SIGUSR1);

        // Zatrzymujemy się
        printf("[Process 2] Stopping (SIGSTOP)\n");
        kill(getpid(), SIGSTOP);
    }
}

// ---------------------------------
// Proces 3
// ---------------------------------
void process3() {
    p3PID = getpid();
    // Każdy proces potomny w osobnej grupie:
    setpgid(0, 0);

    printf("[Process 3] Starting (PID=%d)\n", p3PID);

    // Chcemy własną obsługę SIGTSTP (zamiast domyślnej)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigtstp_handler_p3;  
    // "Zastąp" domyślne zatrzymywanie przez nasz handler
    sigaction(SIGTSTP, &sa, NULL);

    // Obsługa SIGUSR1 (ostatni etap łańcucha zatrzymania)
    signal(SIGUSR1, sigusr1_handler_p3);

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

// Handler w Procesie 3 (SIGTSTP)
// Zamiast zatrzymywać się od razu, wysyłamy SIGUSR2 do Main
void sigtstp_handler_p3(int signo) {
    printf("[Process 3] Received SIGTSTP from user -> sending SIGUSR2 to Main (PID=%d)\n",
           mainPID);
    kill(mainPID, SIGUSR2);
    // UWAGA: nie wykonujemy tu STOPa! (Zatrzymanie nastąpi dopiero
    // poprzez kolejkę i łańcuch sygnałów 1->2->3)
}

// Handler w Procesie 3 (SIGUSR1)
// Odczytuje SIGTSTP (typ=3) i dopiero wtedy się zatrzymuje
void sigusr1_handler_p3(int signo) {
    printf("[Process 3] Received SIGUSR1 -> reading from queue (mtype=3)\n");
    int sig_from_queue = receiveSignalFromQueue(3);
    if (sig_from_queue == SIGTSTP) {
        printf("[Process 3] Detected SIGTSTP in queue -> stopping now (SIGSTOP)\n");
        kill(getpid(), SIGSTOP);
    }
}

// ---------------------------------
// Handler w Main dla SIGUSR2
// (wysyłany przez Proces 3 po otrzymaniu SIGTSTP "z zewnątrz")
void sigusr2_handler_main(int signo) {
    printf("[Main] Received SIGUSR2 from Process 3 -> writing SIGTSTP to queue (mtype=1)\n");

    // Wstawiamy informację o sygnale SIGTSTP (20) do kolejki
    sendSignalToQueue(SIGTSTP, 1);

    // Powiadamiamy proces 1, by odczytał kolejkę (typ=1)
    printf("[Main] Notifying Process 1 (SIGUSR1)\n");
    kill(p1PID, SIGUSR1);
}

// ---------------------------------
// main
// ---------------------------------
int main() {
    printf("[Main] Starting main process (PID=%d)\n", getpid());
    mainPID = getpid();

    // Tworzymy kolejkę komunikatów
    key_t msgKey = ftok("msgqueue", 65);
    msgid = msgget(msgKey, 0666 | IPC_CREAT);
    if (msgid == -1) {
        perror("[Main] msgget failed");
        exit(1);
    }

    // Handler dla sygnału SIGUSR2 (odbierany od p3)
    signal(SIGUSR2, sigusr2_handler_main);

    // Tworzymy potok
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("[Main] pipe failed");
        exit(1);
    }

    // Uruchamiamy proces 1
    pid_t pid1 = fork();
    if (pid1 == 0) {
        close(pipe_fd[0]);
        process1(pipe_fd[1]);
    }
    p1PID = pid1;

    // Uruchamiamy proces 2
    pid_t pid2 = fork();
    if (pid2 == 0) {
        close(pipe_fd[1]);
        process2(pipe_fd[0]);
    }
    p2PID = pid2;

    // Uruchamiamy proces 3
    pid_t pid3 = fork();
    if (pid3 == 0) {
        // W p3 nie korzystamy z potoku
        process3();
    }
    p3PID = pid3;

    close(pipe_fd[0]);
    close(pipe_fd[1]);

    // Proces macierzysty pozostaje aktywny
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
