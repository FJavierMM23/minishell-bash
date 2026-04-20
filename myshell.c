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

/*
 * tjob: representa un proceso o pipeline en background.
 * Almacena el id de job, el pgid del grupo de procesos, un array con
 * todos los pids individuales del pipeline, el texto del comando y
 * el estado actual (RUNNING o STOPPED).
 */
typedef struct
{
    int job_id;
    pid_t pgid;           /* group id = pid del primer proceso del pipeline */
    pid_t *pids;          /* array con todos los pids del pipeline */
    int npids;            /* numero de procesos del pipeline */
    char linea[MAX_LINE]; /* texto del comando para mostrarlo en jobs/fg */
    int estado;
} tjob;

/* Lista global de jobs en background */
static tjob *jobs = NULL;
static int num_jobs = 0;
static int next_id = 1;

/*
 * pgid_pipeline: group id del pipeline que se está lanzando en este momento.
 * Se asigna al pid del primer proceso (indice 0) y los demás procesos del
 * pipeline se unen a ese mismo grupo. Se usa tanto en el hijo (setpgid)
 * como en el padre (tcsetpgrp) y en agregar_job.
 */
static pid_t pgid_pipeline = 0;

/* Prototipos */
int aplicar_redireccion(const char *fichero, int flags, int fd_destino);
void cerrar_pipes(int *fds, int npipes);
int crear_pipes(int *fds, int npipes);
pid_t lanzar_hijo(tline *linea, int indice, int *fds, int npipes);
void lanzar_hijos(tline *linea, int *fds, int npipes, pid_t *pids);
void wait_hijos(pid_t *pids, int n);
void ejecutar_pipeline(tline *linea, const char *comando);
void cmd_cd(tcommand *cmd);
void cmd_jobs(void);
void cmd_fg(tcommand *cmd);
void agregar_job(pid_t pgid, pid_t *pids, int npids, const char *texto);
void eliminar_job(int id);

/*
 * main: bucle principal del minishell.
 * Ignora las señales de teclado para que el shell no muera con Ctrl+C,
 * Ctrl+\ ni Ctrl+Z. Muestra el prompt, lee líneas, detecta comandos
 * internos y delega el resto a ejecutar_pipeline.
 */
int main(void)
{
    char comando[MAX_LINE];
    tline *linea_parser;

    /* El shell ignora estas señales permanentemente.
     * SIGTTOU y SIGTTIN son necesarias para que tcsetpgrp no bloquee al shell.
     * Los hijos restaurarán SIG_DFL antes de execvp. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    while (1)
    {
        printf(PROMPT);
        fflush(stdout);

        /* EOF (Ctrl+D): salir limpiamente */
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

        /* Comandos internos: solo disponibles sin pipes */
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

        /* Eliminar el \n que deja fgets antes de guardar el texto en jobs */
        comando[strcspn(comando, "\n")] = '\0';
        ejecutar_pipeline(linea_parser, comando);
    }

    free(jobs);
    return 0;
}

/*
 * aplicar_redireccion: abre un fichero y redirige el descriptor fd_destino
 * hacia él mediante dup2. Se llama desde el proceso hijo antes de execvp.
 * Retorna 0 si todo fue bien, -1 si hubo error.
 */
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

/*
 * cerrar_pipes: cierra todos los extremos del array plano de pipes.
 * fds[i*2] es el extremo de lectura y fds[i*2+1] el de escritura de la pipe i.
 * Es imprescindible cerrarlos en el hijo para que los lectores reciban EOF
 * cuando todos los escritores terminen. No hace nada si fds es NULL.
 */
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

/*
 * crear_pipes: crea npipes pipes y guarda sus file descriptors en el array
 * plano fds (2 fds por pipe). Si falla alguna, cierra las ya creadas.
 * Retorna 0 si todo fue bien, -1 si hubo error.
 */
int crear_pipes(int *fds, int npipes)
{
    int i;

    for (i = 0; i < npipes; i++)
    {
        if (pipe(&fds[i * 2]) < 0)
        {
            fprintf(stderr, "pipe: %s\n", strerror(errno));
            /* Cerrar las pipes ya creadas antes de salir */
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

/*
 * lanzar_hijo: hace fork y configura el proceso hijo en la posición 'indice'
 * dentro del pipeline. El hijo se mete en el grupo de procesos del pipeline,
 * restaura las señales a SIG_DFL, conecta sus pipes con dup2, cierra los
 * extremos que no usa y aplica las redirecciones de fichero si las hay.
 * El padre confirma el setpgid para evitar una race condition con tcsetpgrp.
 * Retorna el pid del hijo (en el padre) o -1 si hubo error.
 */
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
        /* Proceso hijo: unirse al grupo del pipeline.
         * El primer proceso crea el grupo; los demás se unen a él.
         * Se hace tanto en el hijo como en el padre (líneas siguientes)
         * para evitar la race condition entre fork y tcsetpgrp. */
        if (indice == 0)
        {
            setpgid(0, 0);
        }
        else
        {
            setpgid(0, pgid_pipeline);
        }
    }
    else
    {
        /* Proceso padre: confirmar el grupo del hijo para evitar race condition */
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

    /* A partir de aquí solo ejecuta el proceso hijo */

    /* Restaurar señales: el hijo debe responder a Ctrl+C, Ctrl+\ y Ctrl+Z */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);

    /* Conectar stdin al extremo de lectura de la pipe anterior */
    if (indice > 0)
    {
        ret = dup2(fds[(indice - 1) * 2], STDIN_FILENO);
        if (ret < 0)
        {
            fprintf(stderr, "dup2 stdin: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    /* Conectar stdout al extremo de escritura de la pipe siguiente */
    if (indice < npipes)
    {
        ret = dup2(fds[indice * 2 + 1], STDOUT_FILENO);
        if (ret < 0)
        {
            fprintf(stderr, "dup2 stdout: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    /* Cerrar todos los extremos heredados para que los lectores reciban EOF */
    cerrar_pipes(fds, npipes);

    /* Redirección de fichero de entrada (solo el primer mandato del pipeline) */
    if (indice == 0 && linea->redirect_input != NULL)
    {
        if (aplicar_redireccion(linea->redirect_input, O_RDONLY, STDIN_FILENO) < 0)
        {
            exit(EXIT_FAILURE);
        }
    }

    /* Redirecciones de salida y error (solo el último mandato del pipeline) */
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

    /* Si execvp retorna, el mandato no se encontró */
    fprintf(stderr, "%s: No se encuentra el mandato\n", cmd->argv[0]);
    exit(EXIT_FAILURE);
}

/*
 * lanzar_hijos: lanza todos los mandatos del pipeline en orden y guarda
 * sus pids en el array pids.
 */
void lanzar_hijos(tline *linea, int *fds, int npipes, pid_t *pids)
{
    int i;

    for (i = 0; i < linea->ncommands; i++)
    {
        pids[i] = lanzar_hijo(linea, i, fds, npipes);
    }
}

/*
 * wait_hijos: espera a que terminen todos los procesos del array pids.
 * Usa WUNTRACED para detectar también si alguno se detiene con Ctrl+Z.
 * Ignora los pids con valor -1 (hijos que no se pudieron lanzar).
 */
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

/*
 * ejecutar_pipeline: coordina la ejecución de todos los mandatos de la línea.
 * Crea las pipes necesarias, lanza los procesos hijo y, según el modo:
 *   - Foreground: cede el terminal al grupo de procesos con tcsetpgrp,
 *     espera a que terminen y recupera el terminal. Si alguno se para
 *     con Ctrl+Z, lo registra como job STOPPED.
 *   - Background: registra el job y vuelve al prompt sin esperar.
 */
void ejecutar_pipeline(tline *linea, const char *comando)
{
    int num_comandos;
    int num_pipes;
    int *array_pipes;
    pid_t *pids_hijos;
    int status;
    pid_t ret;

    num_comandos = linea->ncommands;
    num_pipes = num_comandos - 1;

    pids_hijos = malloc(num_comandos * sizeof(pid_t));
    if (pids_hijos == NULL)
    {
        fprintf(stderr, "malloc pids_hijos: %s\n", strerror(errno));
        return;
    }

    /* Caso simple: un solo mandato sin pipes */
    if (num_pipes == 0)
    {
        pids_hijos[0] = lanzar_hijo(linea, 0, NULL, 0);
        if (pids_hijos[0] > 0)
        {
            if (linea->background)
            {
                agregar_job(pids_hijos[0], pids_hijos, 1, comando);
                printf("[%d] %d\n", jobs[num_jobs - 1].job_id, pids_hijos[0]);
            }
            else
            {
                /* Ceder el terminal al hijo para que Ctrl+C le llegue a él */
                tcsetpgrp(STDIN_FILENO, pids_hijos[0]);
                wait_hijos(pids_hijos, 1);
                tcsetpgrp(STDIN_FILENO, getpgrp());
            }
        }
        free(pids_hijos);
        return;
    }

    /* Caso con pipes: N mandatos conectados */
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

    /* El padre cierra todos los extremos para que los hijos reciban EOF */
    cerrar_pipes(array_pipes, num_pipes);

    if (linea->background)
    {
        /* Registrar el job con el pgid y todos los pids del pipeline */
        agregar_job(pgid_pipeline, pids_hijos, num_comandos, comando);
        printf("[%d] %d\n", jobs[num_jobs - 1].job_id, pgid_pipeline);
    }
    else
    {
        /* Ceder el terminal al grupo del pipeline */
        tcsetpgrp(STDIN_FILENO, pgid_pipeline);
        wait_hijos(pids_hijos, num_comandos);

        /* Comprobar si el pipeline se detuvo con Ctrl+Z */
        ret = waitpid(-pgid_pipeline, &status, WNOHANG | WUNTRACED);
        if (ret > 0 && WIFSTOPPED(status))
        {
            agregar_job(pgid_pipeline, pids_hijos, num_comandos, comando);
            jobs[num_jobs - 1].estado = STOPPED;
            printf("\n[%d]+ Stopped\t%s\n", jobs[num_jobs - 1].job_id, comando);
        }

        tcsetpgrp(STDIN_FILENO, getpgrp());
    }

    free(array_pipes);
    free(pids_hijos);
}

/*
 * cmd_cd: comando interno cd. Sin argumentos va al directorio HOME.
 * Imprime la ruta absoluta del nuevo directorio tras el cambio.
 * Se ejecuta en el proceso padre (sin fork) para que el cambio persista.
 */
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

/*
 * cmd_jobs: comando interno jobs. Recorre la lista de jobs en background
 * y comprueba el estado de cada proceso individual con waitpid(WNOHANG).
 * Marca con -1 los pids que han terminado. Cuando todos los pids de un
 * job están marcados, elimina el job de la lista. El resto se muestra
 * con su estado actual (Running o Stopped).
 */
void cmd_jobs(void)
{
    int i;
    int j;
    int status;
    int ret;
    int todos_terminados;

    i = 0;
    while (i < num_jobs)
    {
        todos_terminados = 1;

        /* Comprobar el estado de cada proceso del pipeline individualmente */
        for (j = 0; j < jobs[i].npids; j++)
        {
            if (jobs[i].pids[j] <= 0)
            {
                /* Proceso ya marcado como terminado en una llamada anterior */
                continue;
            }

            ret = waitpid(jobs[i].pids[j], &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (ret > 0)
            {
                if (WIFEXITED(status) || WIFSIGNALED(status))
                {
                    jobs[i].pids[j] = -1; /* marcar como terminado */
                }
                else if (WIFSTOPPED(status))
                {
                    jobs[i].estado = STOPPED;
                    todos_terminados = 0;
                }
                else if (WIFCONTINUED(status))
                {
                    jobs[i].estado = RUNNING;
                    todos_terminados = 0;
                }
            }
            else
            {
                if (ret < 0 && errno != ECHILD)
                {
                    fprintf(stderr, "waitpid: %s\n", strerror(errno));
                }
                if (ret == 0)
                {
                    todos_terminados = 0; /* proceso sigue vivo */
                }
            }
        }

        if (todos_terminados)
        {
            eliminar_job(i); /* no incrementar i: el siguiente ocupa la misma posición */
        }
        else
        {
            printf("[%d]+ %s\t%s\n", jobs[i].job_id,
                   (jobs[i].estado == RUNNING) ? "Running" : "Stopped",
                   jobs[i].linea);
            i++;
        }
    }
}

/*
 * cmd_fg: comando interno fg. Trae al foreground el job indicado por número,
 * o el último si no se especifica ninguno. Cede el terminal al grupo de
 * procesos con tcsetpgrp y les envía SIGCONT para reanudarlos. Espera a
 * que terminen o se paren de nuevo. Si se paran (Ctrl+Z), el job queda
 * en la lista como STOPPED; si terminan, se elimina de la lista.
 */
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
        /* Validar el argumento con strtol para detectar entradas no numéricas */
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

    /* Ceder el terminal al grupo del job: Ctrl+C llegará al hijo (SIG_DFL),
     * no al shell (SIG_IGN). Enviar SIGCONT a todo el grupo para reanudarlo. */
    tcsetpgrp(STDIN_FILENO, jobs[id_job_encontrado].pgid);
    if (kill(-jobs[id_job_encontrado].pgid, SIGCONT) < 0)
    {
        fprintf(stderr, "kill (SIGCONT): %s\n", strerror(errno));
    }

    jobs[id_job_encontrado].estado = RUNNING;

    /* Esperar a cualquier proceso del grupo (WUNTRACED detecta Ctrl+Z) */
    ret = waitpid(-jobs[id_job_encontrado].pgid, &status, WUNTRACED);
    if (ret < 0)
    {
        fprintf(stderr, "waitpid: %s\n", strerror(errno));
    }

    /* Recuperar el control del terminal para el shell */
    tcsetpgrp(STDIN_FILENO, getpgrp());

    if (WIFSTOPPED(status))
    {
        /* El proceso se pausó con Ctrl+Z: queda en la lista como STOPPED */
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

/*
 * agregar_job: añade un nuevo job al array dinámico global.
 * Copia el array de pids para que el job tenga su propia copia independiente
 * (el array original pids_hijos se libera al terminar ejecutar_pipeline).
 * Si realloc falla, el array anterior sigue siendo válido (sin fuga).
 */
void agregar_job(pid_t pgid, pid_t *pids, int npids, const char *texto)
{
    tjob *tmp;
    pid_t *copia_pids;
    int i;

    tmp = realloc(jobs, (num_jobs + 1) * sizeof(tjob));
    if (tmp == NULL)
    {
        fprintf(stderr, "realloc jobs: %s\n", strerror(errno));
        return;
    }

    copia_pids = malloc(npids * sizeof(pid_t));
    if (copia_pids == NULL)
    {
        fprintf(stderr, "malloc pids job: %s\n", strerror(errno));
        return;
    }

    for (i = 0; i < npids; i++)
    {
        copia_pids[i] = pids[i];
    }

    jobs = tmp;
    jobs[num_jobs].job_id = next_id++;
    jobs[num_jobs].pgid = pgid;
    jobs[num_jobs].pids = copia_pids;
    jobs[num_jobs].npids = npids;
    jobs[num_jobs].estado = RUNNING;
    strncpy(jobs[num_jobs].linea, texto, MAX_LINE - 1);
    jobs[num_jobs].linea[MAX_LINE - 1] = '\0';
    num_jobs++;
}

/*
 * eliminar_job: elimina el job en la posición id de la lista global.
 * Libera el array de pids del job antes de desplazar los siguientes
 * para mantener el array compacto sin huecos.
 */
void eliminar_job(int id)
{
    int i;

    free(jobs[id].pids);

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