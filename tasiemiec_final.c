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
static volatile sig_atomic_t paused = 0;

// --- Funkcja sprzątająca ---
void cleanup() {
    printf("[PARENT] Cleaning up resources\n");
    msgctl(msgq_id, IPC_RMID, NULL);
    shmctl(shm_id, IPC_RMID, NULL);
    sem_close(sem);
    sem_unlink("/pcsem");
}

// --- Obsługa sygnałów w procesie rodzica ---
void parent_sigusr1_handler(int sig) {
    msgbuf msg;
    msgrcv(msgq_id, &msg, sizeof(int), 1, 0);
    const int received_signal = msg.signal;
    printf("[PARENT] Received signal %d from P3\n", received_signal);

    if (received_signal == SIGTSTP) {
        printf("[PARENT] Suspending all processes\n");
        kill(p1, SIGSTOP);
        kill(p2, SIGSTOP);
        paused = 1;
    }
    else if (received_signal == SIGCONT) {
        printf("[PARENT] Resuming all processes\n");
        kill(p1, SIGCONT);
        kill(p2, SIGCONT);
        paused = 0;
    }
    else {
        msg.mtype = 2;
        msgsnd(msgq_id, &msg, sizeof(int), 0);
        kill(p1, SIGUSR1);
    }
}

// --- Obsługa sygnałów w procesie P3 ---
void sig_handler(int sig) {
    printf("[P3, PID=%d] Received signal %d\n", getpid(), sig);
    msgbuf msg;
    msg.mtype = 1;
    msg.signal = sig;
    msgsnd(msgq_id, &msg, sizeof(int), 0);
    printf("[P3, PID=%d] Forwarding signal %d to parent\n", getpid(), sig);
    kill(getppid(), SIGUSR1);

    if (sig == SIGTSTP || sig == SIGCONT) {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

// --- Obsługa sygnałów w procesie P1 ---
void p1_signal_handler(int sig) {
    printf("[P1, PID=%d] Received notification from parent\n", getpid());
    msgbuf msg;
    msgrcv(msgq_id, &msg, sizeof(int), 2, 0);
    printf("[P1, PID=%d] Forwarding signal %d to P2\n", getpid(), msg.signal);
    kill(p2, SIGUSR2);
}

// --- Obsługa sygnałów w procesie P2 ---
void p2_signal_handler(int sig) {
    printf("[P2, PID=%d] Received notification from P1\n", getpid());
}

// --- Rejestracja sygnałów w P1 ---
void p1_signals() {
    signal(SIGUSR1, p1_signal_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCONT, SIG_IGN);
}

// --- Rejestracja sygnałów w P2 ---
void p2_signals() {
    signal(SIGUSR2, p2_signal_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCONT, SIG_IGN);
}

// --- Proces P1 z menu ---
void run_p1() {
    printf("[P1, PID=%d] Started\n", getpid());
    p1_signals();
    close(pipe_fd[0]);  // zamykamy koniec do odczytu, używamy tylko do zapisu

    while (1) {
        // MENU
        printf("\n=== MENU (P1) ===\n");
        printf("1. Wczytaj dane z klawiatury\n");
        printf("2. Wczytaj dane z pliku\n");
        printf("Wybor: ");

        char choice[10];
        if (fgets(choice, sizeof(choice), stdin) == NULL) {
            // Błąd odczytu lub EOF -> kończymy
            printf("[P1] Błąd odczytu wyboru lub koniec STDIN. Kończę.\n");
            break;
        }

        int c = atoi(choice);

        switch (c) {
            case 1: {
                // Wczytanie z klawiatury (do momentu pustej linii)
                printf("Podawaj linie tekstu (pusta linia konczy wczytywanie):\n");
                while (1) {
                    char buffer[MAX_LINE];
                    if (fgets(buffer, MAX_LINE, stdin) == NULL) {
                        // EOF lub błąd -> kończymy
                        break;
                    }
                    // Jeśli pusta linia -> przerwij wczytywanie
                    if (strcmp(buffer, "\n") == 0) {
                        break;
                    }
                    if (!paused) {
                        printf("[P1, PID=%d] Sending line: %s", getpid(), buffer);
                        write(pipe_fd[1], buffer, strlen(buffer));
                    }
                }
                break;
            }
            case 2: {
                // Wczytanie z pliku
                printf("Podaj nazwe pliku: ");
                char filename[256];
                if (fgets(filename, sizeof(filename), stdin) == NULL) {
                    printf("[P1] Błąd odczytu nazwy pliku.\n");
                    break;
                }
                // Usunięcie znaku nowej linii
                filename[strcspn(filename, "\n")] = '\0';

                FILE *fp = fopen(filename, "r");
                if (!fp) {
                    printf("[P1] Nie można otworzyć pliku: %s\n", filename);
                    break;
                }

                char buffer[MAX_LINE];
                while (fgets(buffer, MAX_LINE, fp) != NULL) {
                    if (!paused) {
                        printf("[P1, PID=%d] Sending line: %s", getpid(), buffer);
                        write(pipe_fd[1], buffer, strlen(buffer));
                    }
                }
                fclose(fp);
                break;
            }
            default: {
                printf("Niepoprawny wybor. Sprobuj ponownie.\n");
                break;
            }
        }
    }

    // Jeśli z jakiegoś powodu wyjdziemy poza while(1):
    close(pipe_fd[1]);
    printf("[P1, PID=%d] Exiting\n", getpid());
    exit(EXIT_SUCCESS);
}

// --- Proces P2 ---
void run_p2() {
    printf("[P2, PID=%d] Started\n", getpid());
    p2_signals();
    close(pipe_fd[1]); // zamykamy koniec do zapisu, używamy tylko do odczytu
    
    char buffer[MAX_LINE];
    int bytes_read;
    int *shm_ptr = (int*)shmat(shm_id, NULL, 0);
    
    while ((bytes_read = read(pipe_fd[0], buffer, MAX_LINE)) > 0) {
        if (!paused) {
            buffer[bytes_read] = '\0';
            printf("[P2, PID=%d] Received %d bytes\n", getpid(), bytes_read);
            *shm_ptr = (int)strlen(buffer) - 1;  // -1, aby nie liczyć znaku nowej linii
            sem_post(sem);
            printf("[P2, PID=%d] Posted to semaphore\n", getpid());
        }
    }
    close(pipe_fd[0]);
    shmdt(shm_ptr);
    printf("[P2, PID=%d] Exiting\n", getpid());
    exit(EXIT_SUCCESS);
}

// --- Proces P3 ---
void run_p3() {
    printf("[P3, PID=%d] Started\n", getpid());
    // Rejestrujemy obsługę sygnałów
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGTSTP, sig_handler);
    signal(SIGCONT, sig_handler);
    
    int *shm_ptr = (int*)shmat(shm_id, NULL, 0);
    
    while (1) {
        if (!paused) {
            sem_wait(sem);
            printf("[P3, PID=%d] Received count: %d\n", getpid(), *shm_ptr);
        }
    }
    shmdt(shm_ptr);
    exit(EXIT_SUCCESS);
}

// --- Funkcja main (proces rodzica) ---
int main() {
    printf("[PARENT] Main process started (PID=%d)\n", getpid());
    
    pipe(pipe_fd);
    msgq_id = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
    
    key_t shm_key = ftok("/tmp", 'S');
    shm_id = shmget(shm_key, SHM_SIZE, 0666 | IPC_CREAT);
    
    sem = sem_open("/pcsem", O_CREAT, 0666, 0);
    atexit(cleanup);

    // Uruchomienie procesów potomnych
    p1 = fork();
    if (p1 == 0) run_p1();
    
    p2 = fork();
    if (p2 == 0) run_p2();
    
    p3 = fork();
    if (p3 == 0) run_p3();

    printf("[PARENT] Child PIDs: P1=%d, P2=%d, P3=%d\n", p1, p2, p3);
    
    // Rejestracja obsługi sygnału od P3
    signal(SIGUSR1, parent_sigusr1_handler);
    printf("[PARENT] Waiting for children...\n");

    // Czekamy, aż potomki zakończą działanie
    int running = 3;
    while (running > 0) {
        int status;
        pid_t pid = waitpid(-1, &status, WUNTRACED | WCONTINUED);
        
        if (pid == -1) {
            continue;
        }
        if (WIFEXITED(status)) {
            printf("[PARENT] Child %d exited\n", pid);
            running--;
        }
        else if (WIFSTOPPED(status)) {
            printf("[PARENT] Child %d stopped\n", pid);
        }
        else if (WIFCONTINUED(status)) {
            printf("[PARENT] Child %d continued\n", pid);
        }
    }

    return EXIT_SUCCESS;
}
