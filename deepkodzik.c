#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <fcntl.h>
#include <semaphore.h>
#include <errno.h>

#define SHM_SIZE 1024
#define MAX_LINE 1024

typedef struct {
    long mtype;
    int signal;
} msgbuf;

static int pipe_fd[2];
static int msgq_id;
static int shm_id;
static sem_t *sem;
static pid_t p1, p2, p3;

// Struktury dla flag pauzy
struct process_data {
    volatile sig_atomic_t paused;
};

struct process_data p1_data = {0}, p2_data = {0};

// --- Funkcja sprzątająca ---
void cleanup() {
    printf("\n[PARENT] Cleaning up resources\n");
    msgctl(msgq_id, IPC_RMID, NULL);
    shmctl(shm_id, IPC_RMID, NULL);
    sem_close(sem);
    sem_unlink("/pcsem");
}

// --- Obsługa sygnałów w procesie rodzica ---
void parent_sigusr1_handler(int sig) {
    msgbuf msg;
    msgrcv(msgq_id, &msg, sizeof(int), 1, 0);
    printf("[PARENT] Received signal %d from P3\n", msg.signal);

    if (msg.signal == SIGTSTP || msg.signal == SIGCONT) {
        // Wysyłanie komend do P1 i P2
        msgbuf cmd_p1 = {.mtype = 3, .signal = msg.signal};
        msgbuf cmd_p2 = {.mtype = 4, .signal = msg.signal};
        msgsnd(msgq_id, &cmd_p1, sizeof(int), 0);
        msgsnd(msgq_id, &cmd_p2, sizeof(int), 0);
        
        // Powiadomienie procesów
        kill(p1, SIGUSR1);
        kill(p2, SIGUSR1);
    } else {
        msg.mtype = 2;
        msgsnd(msgq_id, &msg, sizeof(int), 0);
        kill(p1, SIGUSR1);
    }
}

// --- Obsługa sygnałów w procesie P3 ---
void sig_handler(int sig) {
    printf("\n[P3] Received signal %d\n", sig);
    msgbuf msg = {.mtype = 1, .signal = sig};
    msgsnd(msgq_id, &msg, sizeof(int), 0);
    kill(getppid(), SIGUSR1);

    if (sig == SIGTSTP || sig == SIGCONT) {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

// --- Obsługa sygnałów w procesie P1 ---
void p1_signal_handler(int sig) {
    msgbuf msg;
    if (msgrcv(msgq_id, &msg, sizeof(int), 3, IPC_NOWAIT) != -1) {
        printf("[P1] Received %s command\n", 
              (msg.signal == SIGTSTP) ? "PAUSE" : "RESUME");
        p1_data.paused = (msg.signal == SIGTSTP);
    } else {
        msgrcv(msgq_id, &msg, sizeof(int), 2, 0);
        printf("[P1] Forwarding signal %d to P2\n", msg.signal);
        kill(p2, SIGUSR2);
    }
}

// --- Obsługa sygnałów w procesie P2 ---
void p2_signal_handler(int sig) {
    msgbuf msg;
    if (msgrcv(msgq_id, &msg, sizeof(int), 4, IPC_NOWAIT) != -1) {
        printf("[P2] Received %s command\n", 
              (msg.signal == SIGTSTP) ? "PAUSE" : "RESUME");
        p2_data.paused = (msg.signal == SIGTSTP);
    }
}

// --- Rejestracja sygnałów w P1 ---
void p1_signals() {
    struct sigaction sa;
    sa.sa_handler = p1_signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
}

// --- Rejestracja sygnałów w P2 ---
void p2_signals() {
    struct sigaction sa;
    sa.sa_handler = p2_signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
}

// --- Proces P1 z menu ---
void run_p1() {
    printf("[P1] Started (PID: %d)\n", getpid());
    p1_signals();
    close(pipe_fd[0]);

    while(1) {
        if (!p1_data.paused) {
            printf("\n=== MENU (P1) ===\n");
            printf("1. Wczytaj z klawiatury\n");
            printf("2. Wczytaj z pliku\n");
            printf("Wybor: ");
            fflush(stdout);

            char choice[10];
            fgets(choice, sizeof(choice), stdin);
            int c = atoi(choice);

            switch(c) {
                case 1: {
                    printf("Wprowadz dane (pusta linia konczy):\n");
                    char buffer[MAX_LINE];
                    while(fgets(buffer, MAX_LINE, stdin) && strcmp(buffer, "\n") != 0) {
                        write(pipe_fd[1], buffer, strlen(buffer));
                    }
                    break;
                }
                case 2: {
                    printf("Podaj nazwe pliku: ");
                    char filename[256];
                    fgets(filename, sizeof(filename), stdin);
                    filename[strcspn(filename, "\n")] = 0;
                    
                    FILE *fp = fopen(filename, "r");
                    if(fp) {
                        char buffer[MAX_LINE];
                        while(fgets(buffer, MAX_LINE, fp)) {
                            write(pipe_fd[1], buffer, strlen(buffer));
                        }
                        fclose(fp);
                    }
                    break;
                }
                default: {
                    printf("Niepoprawny wybor!\n");
                    break;
                }
            }
        } else {
            pause();
        }
    }
}

// --- Proces P2 ---
void run_p2() {
    printf("[P2] Started (PID: %d)\n", getpid());
    p2_signals();
    close(pipe_fd[1]);

    char buffer[MAX_LINE];
    int *shm_ptr = (int*)shmat(shm_id, NULL, 0);

    while(1) {
        if (!p2_data.paused) {
            int bytes_read;
            while((bytes_read = read(pipe_fd[0], buffer, MAX_LINE)) > 0) {
                buffer[bytes_read] = 0;
                *shm_ptr = strlen(buffer) - 1;
                sem_post(sem);
                printf("[P2] Processed %d bytes\n", bytes_read);
            }
            
            if (bytes_read == -1 && errno != EINTR) {
                break;
            }
        } else {
            printf("[P2] PAUSED\n");
            pause();
        }
    }
    
    shmdt(shm_ptr);
    exit(EXIT_SUCCESS);
}

// --- Proces P3 ---
void run_p3() {
    printf("[P3] Started (PID: %d)\n", getpid());
    signal(SIGTSTP, sig_handler);
    signal(SIGCONT, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int *shm_ptr = (int*)shmat(shm_id, NULL, 0);
    while(1) {
        sem_wait(sem);
        printf("[P3] Licznik: %d\n", *shm_ptr);
    }
}

// --- Funkcja main ---
int main() {
    printf("[PARENT] Starting (PID: %d)\n", getpid());
    atexit(cleanup);

    // Inicjalizacja zasobów
    pipe(pipe_fd);
    msgq_id = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
    shm_id = shmget(IPC_PRIVATE, SHM_SIZE, 0666 | IPC_CREAT);
    sem = sem_open("/pcsem", O_CREAT, 0666, 0);

    // Uruchamianie procesów
    p1 = fork();
    if(p1 == 0) run_p1();
    
    p2 = fork();
    if(p2 == 0) run_p2();
    
    p3 = fork();
    if(p3 == 0) run_p3();

    // Konfiguracja handlera w rodzicu
    signal(SIGUSR1, parent_sigusr1_handler);
    printf("[PARENT] Children PIDs: P1=%d, P2=%d, P3=%d\n", p1, p2, p3);

    // Pętla oczekiwania
    int running = 3;
    while(running > 0) {
        pid_t pid = waitpid(-1, NULL, WNOHANG);
        if(pid > 0) running--;
        usleep(100000);
    }

    return EXIT_SUCCESS;
}
