/*
 * myshell.c - MiniShell
 * Sistemas Operativos - Curso 2025-26
 * Grado en Ingeniería de Computadores
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include "parser.h"

#define PROMPT   "msh> "
#define MAX_LINE 1024

typedef struct {
    int   numero;
    pid_t pid;
    char  linea[MAX_LINE];
} tjob;

static tjob *jobs     = NULL;
static int   njobs    = 0;
static int   next_num = 1;

/* Prototipos */
int   aplicar_redireccion(const char *fichero, int flags, int destino);
void  cerrar_pipes(int *fds, int npipes);
int   crear_pipes(int *fds, int npipes);
pid_t lanzar_hijo(tline *linea, int indice, int *fds, int npipes);
void  lanzar_hijos(tline *linea, int *fds, int npipes, pid_t *pids);
void  wait_hijos(pid_t *pids, int n);
void  ejecutar_pipeline(tline *linea, const char *texto);
void  cmd_cd(tcommand *cmd);
void  cmd_jobs(void);
void  cmd_fg(tcommand *cmd);
void  agregar_job(pid_t pid, const char *texto);
void  eliminar_job(int pos);

int main(void)
{
    char   linea[MAX_LINE];
    tline *tl;

    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    while (1) {
        printf(PROMPT);
        fflush(stdout);

        if (fgets(linea, MAX_LINE, stdin) == NULL) {
            printf("\n");
            break;
        }

        tl = tokenize(linea);

        if (tl == NULL || tl->ncommands == 0) {
            continue;
        }
        /* Comandos Internos */
        if (tl->ncommands == 1) {
            if (strcmp(tl->commands[0].argv[0], "cd") == 0) {
                cmd_cd(&tl->commands[0]);
                continue;
            }
            if (strcmp(tl->commands[0].argv[0], "jobs") == 0) {
                cmd_jobs();
                continue;
            }
            if (strcmp(tl->commands[0].argv[0], "fg") == 0) {
                cmd_fg(&tl->commands[0]);
                continue;
            }
            if (strcmp(tl->commands[0].argv[0], "exit") == 0) {
                break;
            }
        }
        /* Resto de Comandos */
        linea[strcspn(linea, "\n")] = '\0';
        ejecutar_pipeline(tl, linea);
    }

    free(jobs);
    return 0;
}

int aplicar_redireccion(const char *fichero, int flags, int destino)
{
    int fd;
    int ret;

    fd = open(fichero, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "%s: Error. %s\n", fichero, strerror(errno));
        return -1;
    }

    ret = dup2(fd, destino);
    close(fd);

    if (ret < 0) {
        fprintf(stderr, "dup2: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

void cerrar_pipes(int *fds, int npipes)
{
    int j;

    if (fds == NULL || npipes <= 0) {
        return;
    }

    for (j = 0; j < npipes * 2; j++) {
        close(fds[j]);
    }
}

int crear_pipes(int *fds, int npipes)
{
    int i;

    for (i = 0; i < npipes; i++) {
        if (pipe(&fds[i * 2]) < 0) {
            fprintf(stderr, "pipe: %s\n", strerror(errno));
            while (--i >= 0) {
                close(fds[i * 2]);
                close(fds[i * 2 + 1]);
            }
            return -1;
        }
    }

    return 0;
}

pid_t lanzar_hijo(tline *linea, int indice, int *fds, int npipes)
{
    pid_t     pid;
    tcommand *cmd;
    int       ret;

    cmd = &linea->commands[indice];

    if (cmd->filename == NULL) {
        fprintf(stderr, "%s: No se encuentra el mandato\n", cmd->argv[0]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        setpgid(0, 0); /* el hijo crea su propio grupo */
    } else {
        setpgid(pid, pid); /* el padre lo confirma para evitar race condition */
        return pid;
    }

    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);

    if (indice > 0) {
        ret = dup2(fds[(indice - 1) * 2], STDIN_FILENO);
        if (ret < 0) {
            fprintf(stderr, "dup2 stdin: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (indice < npipes) {
        ret = dup2(fds[indice * 2 + 1], STDOUT_FILENO);
        if (ret < 0) {
            fprintf(stderr, "dup2 stdout: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    cerrar_pipes(fds, npipes);

    if (indice == 0 && linea->redirect_input != NULL) {
        if (aplicar_redireccion(linea->redirect_input,
                                O_RDONLY, STDIN_FILENO) < 0) {
            exit(EXIT_FAILURE);
        }
    }

    if (indice == linea->ncommands - 1) {
        if (linea->redirect_output != NULL) {
            if (aplicar_redireccion(linea->redirect_output,
                                    O_WRONLY | O_CREAT | O_TRUNC,
                                    STDOUT_FILENO) < 0) {
                exit(EXIT_FAILURE);
            }
        }
        if (linea->redirect_error != NULL) {
            if (aplicar_redireccion(linea->redirect_error,
                                    O_WRONLY | O_CREAT | O_TRUNC,
                                    STDERR_FILENO) < 0) {
                exit(EXIT_FAILURE);
            }
        }
    }

    execvp(cmd->filename, cmd->argv);

    fprintf(stderr, "%s: No se encuentra el mandato\n", cmd->argv[0]);
    exit(EXIT_FAILURE);
}

void lanzar_hijos(tline *linea, int *fds, int npipes, pid_t *pids)
{
    int i;

    for (i = 0; i < linea->ncommands; i++) {
        pids[i] = lanzar_hijo(linea, i, fds, npipes);
    }
}

void wait_hijos(pid_t *pids, int n)
{
    int i;
    int status;
    int ret;

    for (i = 0; i < n; i++) {
        if (pids[i] > 0) {
            ret = waitpid(pids[i], &status, 0);
            if (ret < 0) {
                fprintf(stderr, "waitpid: %s\n", strerror(errno));
            }
        }
    }
}

void ejecutar_pipeline(tline *linea, const char *texto)
{
    int    n;
    int    npipes;
    int   *fds;
    pid_t *pids;

    n      = linea->ncommands;
    npipes = n - 1;

    pids = malloc(n * sizeof(pid_t));
    if (pids == NULL) {
        fprintf(stderr, "malloc pids: %s\n", strerror(errno));
        return;
    }

    if (npipes == 0) {
        pids[0] = lanzar_hijo(linea, 0, NULL, 0);
        if (pids[0] > 0) {
            if (linea->background) {
                agregar_job(pids[0], texto);
                printf("[%d] %d\n", jobs[njobs - 1].numero, pids[0]);
            } else {
                tcsetpgrp(STDIN_FILENO, pids[0]);
                wait_hijos(pids, 1);
                tcsetpgrp(STDIN_FILENO, getpgrp());
            }
        }
        free(pids);
        return;
    }

    fds = malloc(npipes * 2 * sizeof(int));
    if (fds == NULL) {
        fprintf(stderr, "malloc fds: %s\n", strerror(errno));
        free(pids);
        return;
    }

    if (crear_pipes(fds, npipes) < 0) {
        free(fds);
        free(pids);
        return;
    }

    lanzar_hijos(linea, fds, npipes, pids);
    cerrar_pipes(fds, npipes);

    if (linea->background) {
        agregar_job(pids[n - 1], texto);
        printf("[%d] %d\n", jobs[njobs - 1].numero, pids[n - 1]);
    } else {
        tcsetpgrp(STDIN_FILENO, pids[0]);
        wait_hijos(pids, n);
        tcsetpgrp(STDIN_FILENO, getpgrp());
    }

    free(fds);
    free(pids);
}

void cmd_cd(tcommand *cmd)
{
    char *destino;
    char  cwd[MAX_LINE];

    if (cmd->argc == 1) {
        destino = getenv("HOME");
        if (destino == NULL) {
            fprintf(stderr, "cd: No se encuentra la variable HOME\n");
            return;
        }
    } else {
        destino = cmd->argv[1];
    }

    if (chdir(destino) < 0) {
        fprintf(stderr, "%s: Error. %s\n", destino, strerror(errno));
        return;
    }

    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);
    }
}

void cmd_jobs(void)
{
    int i;
    int status;
    int ret;

    i = 0;
    while (i < njobs) {
        ret = waitpid(jobs[i].pid, &status, WNOHANG);
        if (ret > 0) {
            eliminar_job(i);
        } else {
            if (ret < 0) {
                fprintf(stderr, "waitpid: %s\n", strerror(errno));
            }
            i++;
        }
    }

    for (i = 0; i < njobs; i++) {
        printf("[%d]+ Running\t%s\n", jobs[i].numero, jobs[i].linea);
    }
}

void cmd_fg(tcommand *cmd)
{
    int   pos;
    int   i;
    int   status;
    int   ret;
    long  num;
    char *fin;

    if (cmd->argc == 1) {
        if (njobs == 0) {
            fprintf(stderr, "fg: No hay procesos en background\n");
            return;
        }
        pos = njobs - 1;
    } else {
        num = strtol(cmd->argv[1], &fin, 10);
        if (*fin != '\0' || fin == cmd->argv[1] || num <= 0) {
            fprintf(stderr, "fg: %s: argumento no válido\n", cmd->argv[1]);
            return;
        }

        pos = -1;
        for (i = 0; i < njobs; i++) {
            if (jobs[i].numero == (int)num) {
                pos = i;
                break;
            }
        }
        if (pos < 0) {
            fprintf(stderr, "fg: %ld: No existe ese job\n", num);
            return;
        }
    }

    printf("%s\n", jobs[pos].linea);

    /* Ctrl+C llegará al hijo (SIG_DFL), no al shell (SIG_IGN) */
    tcsetpgrp(STDIN_FILENO, jobs[pos].pid);
    kill(-jobs[pos].pid, SIGCONT);

    ret = waitpid(jobs[pos].pid, &status, 0);
    if (ret < 0) {
        fprintf(stderr, "waitpid: %s\n", strerror(errno));
    }

    tcsetpgrp(STDIN_FILENO, getpgrp());
    eliminar_job(pos);
}

void agregar_job(pid_t pid, const char *texto)
{
    tjob *tmp;

    tmp = realloc(jobs, (njobs + 1) * sizeof(tjob));
    if (tmp == NULL) {
        fprintf(stderr, "realloc jobs: %s\n", strerror(errno));
        return;
    }

    jobs               = tmp;
    jobs[njobs].numero = next_num++;
    jobs[njobs].pid    = pid;
    strncpy(jobs[njobs].linea, texto, MAX_LINE - 1);
    jobs[njobs].linea[MAX_LINE - 1] = '\0';
    njobs++;
}

void eliminar_job(int pos)
{
    int i;

    for (i = pos; i < njobs - 1; i++) {
        jobs[i] = jobs[i + 1];
    }
    njobs--;
}
