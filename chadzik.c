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

void cleanup() {
    printf("[PARENT] Cleaning up resources\n");
    msgctl(msgq_id, IPC_RMID, NULL);
    shmctl(shm_id, IPC_RMID, NULL);
    sem_close(sem);
    sem_unlink("/pcsem");
}

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

void p1_signal_handler(int sig) {
    printf("[P1, PID=%d] Received notification from parent\n", getpid());
    msgbuf msg;
    msgrcv(msgq_id, &msg, sizeof(int), 2, 0);
    printf("[P1, PID=%d] Forwarding signal %d to P2\n", getpid(), msg.signal);
    kill(p2, SIGUSR2);
}

void p2_signal_handler(int sig) {
    printf("[P2, PID=%d] Received notification from P1\n", getpid());
}

void p1_signals() {
    signal(SIGUSR1, p1_signal_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCONT, SIG_IGN);
}

void p2_signals() {
    signal(SIGUSR2, p2_signal_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCONT, SIG_IGN);
}

/* ZMIENIONE: Ignorowanie EOF w pętli (opcjonalny usleep, aby nie obciążać CPU). */
void run_p1() {
    printf("[P1, PID=%d] Started\n", getpid());
    p1_signals();
    close(pipe_fd[0]);
    
    char buffer[MAX_LINE];

    while (1) {
        if (fgets(buffer, MAX_LINE, stdin) == NULL) {
            // Sprawdzenie, czy przyczyną jest EOF
            if (feof(stdin)) {
                // Czyścimy status EOF
                clearerr(stdin);
                // Opcjonalnie mała przerwa, aby nie obciążać CPU ciągłym loopem
                usleep(100000); 
                continue;
            } else {
                // Inny błąd czytania
                perror("[P1] Error reading stdin");
                break;
            }
        }
        
        if (!paused) {
            printf("[P1, PID=%d] Sending line: %s", getpid(), buffer);
            write(pipe_fd[1], buffer, strlen(buffer));
        }
    }

    close(pipe_fd[1]);
    printf("[P1, PID=%d] Exiting\n", getpid());
    exit(EXIT_SUCCESS);
}

void run_p2() {
    printf("[P2, PID=%d] Started\n", getpid());
    p2_signals();
    close(pipe_fd[1]);
    
    char buffer[MAX_LINE];
    int bytes_read;
    int *shm_ptr = (int*)shmat(shm_id, NULL, 0);
    
    while((bytes_read = read(pipe_fd[0], buffer, MAX_LINE)) > 0) {
        if (!paused) {
            buffer[bytes_read] = '\0';
            printf("[P2, PID=%d] Received %d bytes\n", getpid(), bytes_read);
            *shm_ptr = strlen(buffer) - 1;
            sem_post(sem);
            printf("[P2, PID=%d] Posted to semaphore\n", getpid());
        }
    }
    close(pipe_fd[0]);
    shmdt(shm_ptr);
    printf("[P2, PID=%d] Exiting\n", getpid());
    exit(EXIT_SUCCESS);
}

void run_p3() {
    printf("[P3, PID=%d] Started\n", getpid());
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGTSTP, sig_handler);
    signal(SIGCONT, sig_handler);
    
    int *shm_ptr = (int*)shmat(shm_id, NULL, 0);
    
    while(1) {
        if (!paused) {
            sem_wait(sem);
            printf("[P3, PID=%d] Received count: %d\n", getpid(), *shm_ptr);
        }
    }
    shmdt(shm_ptr);
    exit(EXIT_SUCCESS);
}

int main() {
    printf("[PARENT] Main process started (PID=%d)\n", getpid());

    // --- Wybór źródła danych (klawiatura vs plik) ---
    int choice;
    char filename[256];

    printf("Wybierz źródło wczytywania danych:\n");
    printf("1 - Klawiatura\n");
    printf("2 - Plik\n");
    printf("Twój wybór: ");
    scanf("%d", &choice);

    if (choice == 2) {
        printf("Podaj nazwę pliku: ");
        scanf("%s", filename);
        FILE *fp = fopen(filename, "r");
        if (!fp) {
            perror("Nie można otworzyć pliku");
            exit(EXIT_FAILURE);
        }
        // Przekierowanie stdin na plik
        dup2(fileno(fp), fileno(stdin));
        fclose(fp);
    }
    // --- Koniec dodanej sekcji ---

    pipe(pipe_fd);
    msgq_id = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
    
    key_t shm_key = ftok("/tmp", 'S');
    shm_id = shmget(shm_key, SHM_SIZE, 0666 | IPC_CREAT);
    
    sem = sem_open("/pcsem", O_CREAT, 0666, 0);
    atexit(cleanup);

    p1 = fork();
    if (p1 == 0) run_p1();
    
    p2 = fork();
    if (p2 == 0) run_p2();
    
    p3 = fork();
    if (p3 == 0) run_p3();

    printf("[PARENT] Child PIDs: P1=%d, P2=%d, P3=%d\n", p1, p2, p3);
    
    signal(SIGUSR1, parent_sigusr1_handler);
    printf("[PARENT] Waiting for children...\n");

    int running = 3;
    while (running > 0) {
        int status;
        pid_t pid = waitpid(-1, &status, WUNTRACED | WCONTINUED);
        
        if (pid == -1) continue;
        
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
