#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <fcntl.h>

#include <signal.h>
#include <sys/msg.h>
#include <sys/types.h>

/* =====================================================================
   Stałe i definicje
   ===================================================================== */
#define SHM_SIZE 1024
#define MSGQ_KEY 0x12345

/* ---------------------------------------------------
   Struktura wiadomości do kolejki komunikatów (SysV)
   --------------------------------------------------- */
struct msgbuf {
    long mtype;          // typ komunikatu
    char mtext[128];     // treść komunikatu (np. numer sygnału)
};

/* =====================================================================
   Zmienne globalne na potrzeby sygnałów i kolejki
   ===================================================================== */
pid_t g_pid1 = 0;
pid_t g_pid2 = 0;
pid_t g_pid3 = 0;
int   g_msqid = -1;   // id kolejki komunikatów

/* =====================================================================
   Handlery sygnałów
   ===================================================================== */

/* -----------------------------
   Handler sygnału w procesie 3
   ----------------------------- */
void signal_handler_p3(int sig)
{
    // Proces 3 po odebraniu sygnału (np. SIGUSR1) chce poinformować rodzica
    printf("[Process 3] Odebrano sygnał %d, wysyłam SIGUSR2 do rodzica.\n", sig);
    kill(getppid(), SIGUSR2);  // proces 3 -> rodzic
}

/* -----------------------------------------
   Handler sygnału w procesie macierzystym
   ----------------------------------------- */
void signal_handler_parent(int sig)
{
    // Rodzic otrzymuje SIGUSR2 od P3, zapisuje info do kolejki i powiadamia P1
    if (sig == SIGUSR2) {
        printf("[Parent] Odebrano SIGUSR2 od procesu 3. Zapisuję do kolejki.\n");

        struct msgbuf msg;
        msg.mtype = 1;  // typ 1 (np. info od P3)
        snprintf(msg.mtext, sizeof(msg.mtext), "Powiadomienie od P3 (SIGUSR1->SIGUSR2)");

        if (msgsnd(g_msqid, &msg, strlen(msg.mtext)+1, 0) == -1) {
            perror("[Parent] msgsnd");
        }

        // Powiadamiamy proces 1 sygnałem SIGUSR1
        if (g_pid1 > 0) {
            printf("[Parent] Wysyłam SIGUSR1 do procesu 1.\n");
            kill(g_pid1, SIGUSR1);
        }
    }
}

/* -----------------------------
   Handler sygnału w procesie 1
   ----------------------------- */
void signal_handler_p1(int sig)
{
    // P1 po otrzymaniu SIGUSR1 odczytuje kolejkę i powiadamia P2
    printf("[Process 1] Odebrano sygnał %d, odczytuję kolejkę.\n", sig);

    struct msgbuf msg;
    // Odczytujemy komunikat typu 1
    if (msgrcv(g_msqid, &msg, sizeof(msg.mtext), 1, IPC_NOWAIT) != -1) {
        printf("[Process 1] Odczytano z kolejki: %s\n", msg.mtext);
        // Powiadamiamy proces 2
        if (g_pid2 > 0) {
            printf("[Process 1] Wysyłam SIGUSR1 do procesu 2.\n");
            kill(g_pid2, SIGUSR1);
        }
    } else {
        perror("[Process 1] msgrcv");
    }
}

/* -----------------------------
   Handler sygnału w procesie 2
   ----------------------------- */
void signal_handler_p2(int sig)
{
    // P2 po otrzymaniu SIGUSR1 może ponownie odczytać kolejkę i na koniec powiadomić P3
    printf("[Process 2] Odebrano sygnał %d, odczytuję kolejkę.\n", sig);

    struct msgbuf msg;
    // Również odczytujemy typ 1 (o ile coś tam jest)
    if (msgrcv(g_msqid, &msg, sizeof(msg.mtext), 1, IPC_NOWAIT) != -1) {
        printf("[Process 2] Odczytano z kolejki: %s\n", msg.mtext);
    } else {
        perror("[Process 2] msgrcv");
    }

    // Powiadamiamy proces 3, że także coś może zrobić
    if (g_pid3 > 0) {
        printf("[Process 2] Wysyłam SIGUSR1 do procesu 3.\n");
        kill(g_pid3, SIGUSR1);
    }
}

/* =====================================================================
   Funkcje inicjalizujące sygnały i kolejkę
   ===================================================================== */

/* Wywoływane w procesie 1 */
void setup_signals_for_proc1(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler_p1;
    sigaction(SIGUSR1, &sa, NULL);
}

/* Wywoływane w procesie 2 */
void setup_signals_for_proc2(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler_p2;
    sigaction(SIGUSR1, &sa, NULL);
}

/* Wywoływane w procesie 3 */
void setup_signals_for_proc3(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler_p3;
    sigaction(SIGUSR1, &sa, NULL);
}

/* Wywoływane w procesie macierzystym (main) – tworzy kolejkę i ustawia handler */
void setup_signals_and_queue(pid_t pid1, pid_t pid2, pid_t pid3)
{
    // Zachowujemy PID‐y dzieci w zmiennych globalnych
    g_pid1 = pid1;
    g_pid2 = pid2;
    g_pid3 = pid3;

    // Tworzymy kolejkę (lub otwieramy, jeśli istnieje)
    g_msqid = msgget(MSGQ_KEY, 0666 | IPC_CREAT);
    if (g_msqid == -1) {
        perror("[Parent] msgget");
        exit(1);
    }

    // Ustawiamy handler w rodzicu (nasłuchujemy np. SIGUSR2 od P3)
    struct sigaction sa_parent;
    memset(&sa_parent, 0, sizeof(sa_parent));
    sa_parent.sa_handler = signal_handler_parent;
    sigaction(SIGUSR2, &sa_parent, NULL);
}

/* =====================================================================
   Oryginalne funkcje: process1, process2, process3
   (bez zmian w środku!)
   ===================================================================== */
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

/* =====================================================================
   Funkcja main – uruchamia procesy i włącza obsługę sygnałów/komunikatów
   ===================================================================== */
int main() {
    printf("[Main] Starting main process\n");
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("[Main] pipe failed");
        exit(1);
    }

    // Tworzymy proces 1
    pid_t pid1 = fork();
    if (pid1 == 0) {
        // Inicjalizacja sygnałów w procesie 1
        setup_signals_for_proc1();
        close(pipe_fd[0]);
        process1(pipe_fd[1]);
    }

    // Tworzymy proces 2
    pid_t pid2 = fork();
    if (pid2 == 0) {
        // Inicjalizacja sygnałów w procesie 2
        setup_signals_for_proc2();
        close(pipe_fd[1]);
        process2(pipe_fd[0]);
    }

    // Tworzymy proces 3
    pid_t pid3 = fork();
    if (pid3 == 0) {
        // Inicjalizacja sygnałów w procesie 3
        setup_signals_for_proc3();
        process3();
    }

    // W procesie macierzystym podłączamy mechanizm sygnałów i kolejki
    setup_signals_and_queue(pid1, pid2, pid3);

    close(pipe_fd[0]);
    close(pipe_fd[1]);

    // Czekamy na zakończenie potomków
    waitpid(pid1, NULL, 0);
    printf("[Main] Process 1 finished\n");
    waitpid(pid2, NULL, 0);
    printf("[Main] Process 2 finished\n");
    waitpid(pid3, NULL, 0);
    printf("[Main] Process 3 finished\n");

    // Sprzątamy semafory
    sem_unlink("/sem2");
    sem_unlink("/sem3");

    // Sprzątamy pamięć współdzieloną
    shmctl(shmget(ftok("shmfile", 65), SHM_SIZE, 0666 | IPC_CREAT), IPC_RMID, NULL);

    // Sprzątamy kolejkę komunikatów
    if (g_msqid != -1) {
        msgctl(g_msqid, IPC_RMID, NULL);
    }

    printf("[Main] Cleaned up resources and exiting\n");
    return 0;
}
