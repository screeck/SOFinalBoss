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

// Globalne identyfikatory do kolejek, semaforów itp.
int  msgid;        // ID kolejki komunikatów
int  shmid;        // ID pamięci współdzielonej
pid_t mainPID;     // PID procesu macierzystego
pid_t p1PID, p2PID, p3PID; // PID-y poszczególnych procesów

// Semafory
sem_t *sem2 = NULL;
sem_t *sem3 = NULL;

// ---------------------------------
// Funkcje pomocnicze do kolejki
// ---------------------------------

// Funkcja wysyłająca numer sygnału do kolejki (o podanym typie)
void sendSignalToQueue(int signo, long mtype) {
    struct msgBuf message;
    message.mtype = mtype;
    message.signo = signo;
    if (msgsnd(msgid, &message, sizeof(message.signo), 0) == -1) {
        perror("[sendSignalToQueue] msgsnd error");
    }
}

// Funkcja odczytująca numer sygnału z kolejki (o podanym typie)
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
    // Ustawiamy PID procesu 1 globalnie (pomocniczo, żeby było wiadomo, kto jest kim)
    p1PID = getpid();
    printf("[Process 1] Starting (PID=%d)\n", p1PID);

    // Ustawiamy obsługę sygnałów – np. SIGUSR1
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

    // Ustawiamy obsługę sygnału SIGUSR1
    signal(SIGUSR1, sigusr1_handler_p2);

    // Uzyskanie segmentu pamięci współdzielonej
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

        // Wysłanie sygnału do sem3 i oczekiwanie na sem2
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

    // Obsługa sygnałów: SIGUSR1 (powiadomienie z procesów 1/2) oraz SIGTSTP (od użytkownika)
    signal(SIGUSR1, sigusr1_handler_p3);
    signal(SIGTSTP, sigtstp_handler_p3);

    // Uzyskanie segmentu pamięci współdzielonej
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

// ---------------------------------
// Funkcje obsługi sygnałów
// ---------------------------------

// Handler w Main dla sygnału SIGUSR2 od procesu 3
// Gdy proces 3 otrzyma SIGTSTP, wysyła SIGUSR2 do Main,
// a Main umieszcza informację w kolejce i powiadamia proces 1.
void sigusr2_handler_main(int signo) {
    printf("[Main] Otrzymano sygnał SIGUSR2 od Process 3. Zapisuję sygnał (%d) do kolejki.\n", signo);

    // Zapisujemy w kolejce komunikat (np. SIGTSTP = 20)
    sendSignalToQueue(SIGTSTP, 1);  // umownie mtype=1 dla "pierwszego" w kolejce

    // Powiadomienie procesu 1
    printf("[Main] Wysyłam SIGUSR1 do Process 1 (PID=%d)\n", p1PID);
    kill(p1PID, SIGUSR1);
}

// Handler w Procesie 1 (SIGUSR1)
// - odczyt sygnału z kolejki
// - wstawienie go ponownie (by kolejny proces mógł odczytać)
// - powiadomienie Procesu 2
// - SIGSTOP
void sigusr1_handler_p1(int signo) {
    printf("[Process 1] Otrzymałem SIGUSR1.\n");

    int sig_from_queue = receiveSignalFromQueue(1); // Odczytujemy z mtype=1
    if (sig_from_queue == SIGTSTP) {
        printf("[Process 1] W kolejce był sygnał SIGTSTP (%d). Przekazuję dalej.\n", sig_from_queue);

        // Odkładamy z powrotem do kolejki (tym razem np. mtype=2 dla p2)
        sendSignalToQueue(sig_from_queue, 2);

        // Powiadamiamy proces 2
        printf("[Process 1] Wysyłam SIGUSR1 do Process 2 (PID=%d)\n", p2PID);
        kill(p2PID, SIGUSR1);

        // Zatrzymujemy się
        printf("[Process 1] Wykonuję SIGSTOP.\n");
        kill(getpid(), SIGSTOP);
    }
}

// Handler w Procesie 2 (SIGUSR1)
// - odczyt sygnału z kolejki (mtype=2)
// - wstawienie go ponownie (mtype=3) dla procesu 3
// - powiadomienie procesu 3
// - SIGSTOP
void sigusr1_handler_p2(int signo) {
    printf("[Process 2] Otrzymałem SIGUSR1.\n");

    int sig_from_queue = receiveSignalFromQueue(2);
    if (sig_from_queue == SIGTSTP) {
        printf("[Process 2] W kolejce był sygnał SIGTSTP (%d). Przekazuję dalej.\n", sig_from_queue);

        // Odkładamy sygnał do kolejki z mtype=3
        sendSignalToQueue(sig_from_queue, 3);

        // Powiadamiamy proces 3
        printf("[Process 2] Wysyłam SIGUSR1 do Process 3 (PID=%d)\n", p3PID);
        kill(p3PID, SIGUSR1);

        // Zatrzymujemy się
        printf("[Process 2] Wykonuję SIGSTOP.\n");
        kill(getpid(), SIGSTOP);
    }
}

// Handler w Procesie 3 (SIGUSR1)
// - odczyt sygnału z kolejki (mtype=3)
// - jeśli to SIGTSTP, zatrzymanie procesu
void sigusr1_handler_p3(int signo) {
    printf("[Process 3] Otrzymałem SIGUSR1.\n");

    int sig_from_queue = receiveSignalFromQueue(3);
    if (sig_from_queue == SIGTSTP) {
        printf("[Process 3] W kolejce był sygnał SIGTSTP (%d). Zatrzymuję się.\n", sig_from_queue);
        kill(getpid(), SIGSTOP);
    }
}

// Handler w Procesie 3 (SIGTSTP) - gdy użytkownik wciśnie np. Ctrl+Z
// Proces 3 wysyła wtedy SIGUSR2 do procesu głównego
void sigtstp_handler_p3(int signo) {
    printf("[Process 3] Otrzymałem SIGTSTP od użytkownika. Informuję Main.\n");
    kill(mainPID, SIGUSR2);
}

// ---------------------------------
// Funkcja main
// ---------------------------------

int main() {
    printf("[Main] Starting main process (PID=%d)\n", getpid());
    mainPID = getpid();  // zapamiętujemy PID procesu głównego

    // Tworzymy kolejkę komunikatów
    key_t msgKey = ftok("msgqueue", 65);
    msgid = msgget(msgKey, 0666 | IPC_CREAT);
    if (msgid == -1) {
        perror("[Main] msgget failed");
        exit(1);
    }

    // Ustawiamy handler dla SIGUSR2 (odbierany od procesu 3)
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
        // Proces potomny
        close(pipe_fd[0]);   // niepotrzebne do czytania
        process1(pipe_fd[1]);
    }
    p1PID = pid1;

    // Uruchamiamy proces 2
    pid_t pid2 = fork();
    if (pid2 == 0) {
        close(pipe_fd[1]);   // niepotrzebne do pisania
        process2(pipe_fd[0]);
    }
    p2PID = pid2;

    // Uruchamiamy proces 3
    pid_t pid3 = fork();
    if (pid3 == 0) {
        process3();
    }
    p3PID = pid3;

    // Proces macierzysty
    close(pipe_fd[0]);
    close(pipe_fd[1]);

    // Czekamy na zakończenie procesów potomnych
    waitpid(pid1, NULL, 0);
    printf("[Main] Process 1 finished\n");
    waitpid(pid2, NULL, 0);
    printf("[Main] Process 2 finished\n");
    waitpid(pid3, NULL, 0);
    printf("[Main] Process 3 finished\n");

    // Usunięcie semaforów
    sem_unlink("/sem2");
    sem_unlink("/sem3");

    // Usunięcie pamięci współdzielonej
    shmctl(shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT), IPC_RMID, NULL);

    // Usunięcie kolejki komunikatów
    msgctl(msgid, IPC_RMID, NULL);

    printf("[Main] Cleaned up resources and exiting\n");

    return 0;
}
