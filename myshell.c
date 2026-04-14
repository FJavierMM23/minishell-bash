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

#define PROMPT "msh> "
#define MAX_LINE 1024
#define RUNNING 0
#define STOPPED 1

typedef struct
{
    int job_id;
    pid_t pid;
    char linea[MAX_LINE];
    int estado;
} tjob;

static tjob *jobs = NULL;
static int num_jobs = 0;
static int next_id = 1;
static pid_t pgid_pipeline = 0;

/* Prototipos */
int aplicar_redireccion(const char *fichero, int flags, int destino);
void cerrar_pipes(int *fds, int npipes);
int crear_pipes(int *fds, int npipes);
pid_t lanzar_hijo(tline *linea, int indice, int *fds, int npipes);
void lanzar_hijos(tline *linea, int *fds, int npipes, pid_t *pids);
void wait_hijos(pid_t *pids, int n);
void ejecutar_pipeline(tline *linea, const char *texto);
void cmd_cd(tcommand *cmd);
void cmd_jobs(void);
void cmd_fg(tcommand *cmd);
void agregar_job(pid_t pid, const char *texto);
void eliminar_job(int pos);

int main(void)
{
    char comando[MAX_LINE];
    tline *linea_parser;

    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    while (1)
    {
        printf(PROMPT);
        fflush(stdout);

        if (fgets(comando, MAX_LINE, stdin) == NULL)
        {
            printf("\n");
            break;
        }

        linea_parser = tokenize(comando);

        if (linea_parser == NULL || linea_parser->ncommands == 0)
        {
            continue;
        }
        /* Comandos Internos */
        if (linea_parser->ncommands == 1)
        {
            if (strcmp(linea_parser->commands[0].argv[0], "cd") == 0)
            {
                cmd_cd(&linea_parser->commands[0]);
                continue;
            }
            if (strcmp(linea_parser->commands[0].argv[0], "jobs") == 0)
            {
                cmd_jobs();
                continue;
            }
            if (strcmp(linea_parser->commands[0].argv[0], "fg") == 0)
            {
                cmd_fg(&linea_parser->commands[0]);
                continue;
            }
            if (strcmp(linea_parser->commands[0].argv[0], "exit") == 0)
            {
                break;
            }
        }
        /* Resto de Comandos */
        comando[strcspn(comando, "\n")] = '\0';
        ejecutar_pipeline(linea_parser, comando);
    }

    free(jobs);
    return 0;
}

int aplicar_redireccion(const char *fichero, int flags, int fd_destino)
{
    int fd;
    int ret;

    fd = open(fichero, flags, 0644);
    if (fd < 0)
    {
        fprintf(stderr, "%s: Error. %s\n", fichero, strerror(errno));
        return -1;
    }

    ret = dup2(fd, fd_destino);
    close(fd);

    if (ret < 0)
    {
        fprintf(stderr, "dup2: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

void cerrar_pipes(int *fds, int npipes)
{
    int j;

    if (fds == NULL || npipes <= 0)
    {
        return;
    }

    for (j = 0; j < npipes * 2; j++)
    {
        close(fds[j]);
    }
}

int crear_pipes(int *fds, int npipes)
{
    int i;

    for (i = 0; i < npipes; i++)
    {
        if (pipe(&fds[i * 2]) < 0)
        {
            fprintf(stderr, "pipe: %s\n", strerror(errno));
            while (--i >= 0)
            {
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
    pid_t pid;
    tcommand *cmd;
    int ret;

    cmd = &linea->commands[indice];

    if (cmd->filename == NULL)
    {
        fprintf(stderr, "%s: No se encuentra el mandato\n", cmd->argv[0]);
        return -1;
    }

    pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        if (indice == 0)
        {
            setpgid(0, 0);
        }
        else
        {
            setpgid(0, pgid_pipeline); /* resto se unen al grupo */
        }
    }
    else
    {
        if (indice == 0)
        {
            pgid_pipeline = pid;
            setpgid(pid, pid);
        }
        else
        {
            setpgid(pid, pgid_pipeline);
        }
        return pid;
    }
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);

    if (indice > 0)
    {
        ret = dup2(fds[(indice - 1) * 2], STDIN_FILENO);
        if (ret < 0)
        {
            fprintf(stderr, "dup2 stdin: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (indice < npipes)
    {
        ret = dup2(fds[indice * 2 + 1], STDOUT_FILENO);
        if (ret < 0)
        {
            fprintf(stderr, "dup2 stdout: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    cerrar_pipes(fds, npipes);

    if (indice == 0 && linea->redirect_input != NULL)
    {
        if (aplicar_redireccion(linea->redirect_input,
                                O_RDONLY, STDIN_FILENO) < 0)
        {
            exit(EXIT_FAILURE);
        }
    }

    if (indice == linea->ncommands - 1)
    {
        if (linea->redirect_output != NULL)
        {
            if (aplicar_redireccion(linea->redirect_output,
                                    O_WRONLY | O_CREAT | O_TRUNC,
                                    STDOUT_FILENO) < 0)
            {
                exit(EXIT_FAILURE);
            }
        }
        if (linea->redirect_error != NULL)
        {
            if (aplicar_redireccion(linea->redirect_error,
                                    O_WRONLY | O_CREAT | O_TRUNC,
                                    STDERR_FILENO) < 0)
            {
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

    for (i = 0; i < linea->ncommands; i++)
    {
        pids[i] = lanzar_hijo(linea, i, fds, npipes);
    }
}

void wait_hijos(pid_t *pids, int n)
{
    int i;
    int status;
    int ret;

    for (i = 0; i < n; i++)
    {
        if (pids[i] > 0)
        {
            ret = waitpid(pids[i], &status, WUNTRACED);
            if (ret < 0)
            {
                fprintf(stderr, "waitpid: %s\n", strerror(errno));
            }
        }
    }
}

void ejecutar_pipeline(tline *linea, const char *comando)
{
    int num_comandos;
    int num_pipes;
    int *array_pipes;
    pid_t *pids_hijos;

    num_comandos = linea->ncommands;
    num_pipes = num_comandos - 1;

    pids_hijos = malloc(num_comandos * sizeof(pid_t));
    if (pids_hijos == NULL)
    {
        fprintf(stderr, "malloc pids_hijos: %s\n", strerror(errno));
        return;
    }

    if (num_pipes == 0)
    {
        pids_hijos[0] = lanzar_hijo(linea, 0, NULL, 0);
        if (pids_hijos[0] > 0)
        {
            if (linea->background)
            {
                agregar_job(pids_hijos[0], comando);
                printf("[%d] %d\n", jobs[num_jobs - 1].job_id, pids_hijos[0]);
            }
            else
            {
                tcsetpgrp(STDIN_FILENO, pids_hijos[0]);
                wait_hijos(pids_hijos, 1);
                tcsetpgrp(STDIN_FILENO, getpgrp());
            }
        }
        free(pids_hijos);
        return;
    }

    array_pipes = malloc(num_pipes * 2 * sizeof(int));
    if (array_pipes == NULL)
    {
        fprintf(stderr, "malloc array_pipes: %s\n", strerror(errno));
        free(pids_hijos);
        return;
    }

    if (crear_pipes(array_pipes, num_pipes) < 0)
    {
        free(array_pipes);
        free(pids_hijos);
        return;
    }

    lanzar_hijos(linea, array_pipes, num_pipes, pids_hijos);
    cerrar_pipes(array_pipes, num_pipes);

    if (linea->background)
    {
        agregar_job(pids_hijos[num_comandos - 1], comando);
        printf("[%d] %d\n", jobs[num_jobs - 1].job_id, pids_hijos[num_comandos - 1]);
    }
    else
    {
        tcsetpgrp(STDIN_FILENO, pgid_pipeline);
        wait_hijos(pids_hijos, num_comandos);
        tcsetpgrp(STDIN_FILENO, getpgrp());
    }

    free(array_pipes);
    free(pids_hijos);
}

void cmd_cd(tcommand *cmd)
{
    char *destino;
    char cwd[MAX_LINE];

    if (cmd->argc == 1)
    {
        destino = getenv("HOME");
        if (destino == NULL)
        {
            fprintf(stderr, "cd: No se encuentra la variable HOME\n");
            return;
        }
    }
    else
    {
        destino = cmd->argv[1];
    }

    if (chdir(destino) < 0)
    {
        fprintf(stderr, "%s: Error. %s\n", destino, strerror(errno));
        return;
    }

    if (getcwd(cwd, sizeof(cwd)) != NULL)
    {
        printf("%s\n", cwd);
    }
}

void cmd_jobs(void)
{
    int i;
    int status;
    int ret;

    i = 0;
    while (i < num_jobs)
    {
        ret = waitpid(jobs[i].pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (ret > 0)
        {
            if (WIFEXITED(status) || WIFSIGNALED(status))
            {
                eliminar_job(i);
                continue;
            }
            else if (WIFSTOPPED(status))
            {
                jobs[i].estado = STOPPED;
            }
            else if (WIFCONTINUED(status))
            {
                jobs[i].estado = RUNNING;
            }
            i++;
        }
        else
        {
            if (ret < 0)
            {
                fprintf(stderr, "waitpid: %s\n", strerror(errno));
            }
            i++;
        }
    }

    for (i = 0; i < num_jobs; i++)
    {
        char *estado = (jobs[i].estado == RUNNING) ? "Running" : "Stopped";
        printf("[%d]+ %s\t%s\n", jobs[i].job_id, estado, jobs[i].linea);
    }
}

void cmd_fg(tcommand *cmd)
{
    int id_job_encontrado;
    int i;
    int status;
    int ret;
    long id_solicitado;
    char *ptr_validacion;

    if (cmd->argc == 1)
    {
        if (num_jobs == 0)
        {
            fprintf(stderr, "fg: No hay procesos en background\n");
            return;
        }
        id_job_encontrado = num_jobs - 1;
    }
    else
    {
        id_solicitado = strtol(cmd->argv[1], &ptr_validacion, 10);
        if (*ptr_validacion != '\0' || ptr_validacion == cmd->argv[1] || id_solicitado <= 0)
        {
            fprintf(stderr, "fg: %s: argumento no válido\n", cmd->argv[1]);
            return;
        }

        id_job_encontrado = -1;
        for (i = 0; i < num_jobs; i++)
        {
            if (jobs[i].job_id == (int)id_solicitado)
            {
                id_job_encontrado = i;
                break;
            }
        }
        if (id_job_encontrado < 0)
        {
            fprintf(stderr, "fg: %ld: No existe ese job\n", id_solicitado);
            return;
        }
    }

    printf("%s\n", jobs[id_job_encontrado].linea);

    /* Ctrl+C llegará al hijo (SIG_DFL), no al shell (SIG_IGN) */
    tcsetpgrp(STDIN_FILENO, jobs[id_job_encontrado].pid);
    if (kill(-jobs[id_job_encontrado].pid, SIGCONT) < 0)
    {
        fprintf(stderr, "kill (SIGCONT): %s\n", strerror(errno));
    }

    jobs[id_job_encontrado].estado = RUNNING;
    ret = waitpid(-jobs[id_job_encontrado].pid, &status, WUNTRACED);
    if (ret < 0)
    {
        fprintf(stderr, "waitpid: %s\n", strerror(errno));
    }

    tcsetpgrp(STDIN_FILENO, getpgrp());

    if (WIFSTOPPED(status))
    {
        /* El proceso se pausó (Ctrl+Z): queda en la lista como STOPPED */
        jobs[id_job_encontrado].estado = STOPPED;
        printf("\n[%d]+ Stopped\t%s\n",
               jobs[id_job_encontrado].job_id,
               jobs[id_job_encontrado].linea);
    }
    else
    {
        eliminar_job(id_job_encontrado);
    }
}

void agregar_job(pid_t pid, const char *texto)
{
    tjob *tmp;

    tmp = realloc(jobs, (num_jobs + 1) * sizeof(tjob));
    if (tmp == NULL)
    {
        fprintf(stderr, "realloc jobs: %s\n", strerror(errno));
        return;
    }

    jobs = tmp;
    jobs[num_jobs].job_id = next_id++;
    jobs[num_jobs].pid = pid;
    strncpy(jobs[num_jobs].linea, texto, MAX_LINE - 1);
    jobs[num_jobs].linea[MAX_LINE - 1] = '\0';
    jobs[num_jobs].estado = RUNNING;
    num_jobs++;
}

void eliminar_job(int id)
{
    int i;

    for (i = id; i < num_jobs - 1; i++)
    {
        jobs[i] = jobs[i + 1];
    }

    num_jobs--;

    if (num_jobs == 0)
    {
        free(jobs);
        jobs = NULL;
    }
}
