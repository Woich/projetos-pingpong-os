#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include "pingpong.h"
#include "queue.h"
#include "diskdriver.h"
#include "harddisk.h"

#define STACKSIZE 32768

#define DEFAULT_PRIO 0
#define MIN_PRIO -20
#define MAX_PRIO 20
#define ALPHA_PRIO 1

#define RESET_TICKS 10
#define TICK_MICROSECONDS 1000

// Tasks
task_t taskMain; // Main
task_t taskDisp; // Dispatcher
task_t taskDiskMgr; // Gerenciador de disco

// Ponteiros para tasks
task_t* taskExec; // Task em execu��o
task_t* freeTask; // Task a ser liberada (exit)

// Filas
task_t* readyQueue; // Fila de tarefas prontas
task_t* sleepQueue; // Fila de tarefas dormindo
task_t* suspendedQueue; // Fila de tarefas suspensas (por tempo indeterminado)

/* ID da pr�xima task a ser criada */
long nextid;

/* Contagem de tasks de usu�rio criadas */
long countTasks;

/* Flag que indica se a preempcao por tempo esta ativa ou nao */
unsigned char preempcao;

/* Preemp��o por tempo */
void tickHandler();
short remainingTicks;
struct sigaction action;
struct itimerval timer;
unsigned int systemTime;

/* Fun��o a ser executada pela task do dispatcher*/
void bodyDispatcher(void* arg);

/* Fun��o a ser executada pelo gerenciador de disco */
void bodyDiskManager(void* arg);
disk_t disco;
struct sigaction diskAction;
void diskSignalHandler();

/* Fun��o que retorna a pr�xima task a ser executada. */
task_t* scheduler();

void pingpong_init() {
    /* Desativa o buffer de sa�da padr�o */
    setvbuf(stdout, 0, _IONBF, 0);

    readyQueue = NULL;
    sleepQueue = NULL;

    /* INICIA A TASK MAIN */
    /* Refer�ncia a si mesmo */
    taskMain.main = &taskMain;

    /* A task main esta pronta. */
    taskMain.estado = 'r';

    /* A task main tem id 0. */
    taskMain.tid = 0;

    /* Informa��es de tempo */
    taskMain.creationTime = systime();
    taskMain.lastExecutionTime = 0;
    taskMain.execTime = 0;
    taskMain.procTime = 0;
    taskMain.activations = 0;
    preempcao = 1;

    taskMain.joinQueue = NULL;

    taskMain.awakeTime = 0;

    /* Coloca a tarefa na fila */
    queue_append((queue_t**)&readyQueue, (queue_t*)&taskMain);
    taskMain.queue = &readyQueue;

    /* O id da pr�xima task a ser criada � 1. */
    nextid = 1;
    
    /* A contagem de tasks de usu�rio inicia em 0. */
    countTasks = 0;

    /* A task que est� executando nesse momento � a main (que chamou pingpong_init). */
    taskExec = &taskMain;

    /* Nao ha nenhuma task para ser liberada. */
    freeTask = NULL;

    /* Preemp��o por tempo */
    remainingTicks = RESET_TICKS;
    action.sa_handler = tickHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGALRM, &action, 0) < 0) {
        perror("Erro em sigaction: ");
        exit(1);
    }
    timer.it_value.tv_usec = TICK_MICROSECONDS;
    timer.it_value.tv_sec = 0;
    timer.it_interval.tv_usec = TICK_MICROSECONDS;
    timer.it_interval.tv_sec = 0;
    if (setitimer(ITIMER_REAL, &timer, 0) < 0) {
        perror("Erro em setitimer: ");
        exit(1);
    }

    systemTime = 0;

    /* O contexto n�o precisa ser salvo agora, porque a primeira troca de contexto far� isso. */

    /* INICIA A TASK DISPATCHER */
    task_create(&taskDisp, &bodyDispatcher, NULL);
    queue_remove((queue_t**)&readyQueue, (queue_t*)&taskDisp);

    /* INICIA A TASK DISK MANAGER */
    task_create(&taskDiskMgr, &bodyDiskManager, NULL);
    --countTasks; // O disk manager n�o � uma task de usu�rio.
    diskAction.sa_handler = diskSignalHandler;
    sigemptyset(&diskAction.sa_mask);
    diskAction.sa_flags = 0;
    if (sigaction(SIGUSR1, &diskAction, 0) < 0) {
        perror("Erro em sigaction: ");
        exit(1);
    }

    /* Ativa o dispatcher */
    task_yield();
}

int task_create(task_t* task, void(*start_func)(void*), void* arg) {
    char* stack;

    /* Coloca refer�ncia para task main. */
    task->main = &taskMain;

    /* Inicializa o contexto. */
    getcontext(&(task->context));

    /* Aloca a pilha. */
    stack = malloc(STACKSIZE);
    if (stack == NULL) {
        perror("Erro na cria��o da pilha: ");
        return -1;
    }

    /* Seta a pilha do contexto. */
    task->context.uc_stack.ss_sp = stack;
    task->context.uc_stack.ss_size = STACKSIZE;
    task->context.uc_stack.ss_flags = 0;

    /* N�o liga o contexto a outro. */
    task->context.uc_link = NULL;

    /* Cria o contexto com a fun��o. */
    makecontext(&(task->context), (void(*)(void))start_func, 1, arg);

    /* Seta o id da task. */
    task->tid = nextid;
    nextid++;
    
    countTasks++;

    /* Informa��es da fila. */
    queue_append((queue_t**)&readyQueue, (queue_t*)task);
    task->queue = &readyQueue;
    task->estado = 'r';
    task->prio = DEFAULT_PRIO;
    task->dynPrio = task->prio;

    /* Informa��es de tempo */
    task->creationTime = systime();
    task->lastExecutionTime = 0;
    task->execTime = 0;
    task->procTime = 0;
    task->activations = 0;

    task->joinQueue = NULL;

    task->awakeTime = 0;

    return (task->tid);
}

void task_exit(int exitCode) {
    freeTask = taskExec;
    freeTask->estado = 'x';
    freeTask->exitCode = exitCode;

    /* Acorda todas as tarefas na fila de join. */
    while (freeTask->joinQueue != NULL) {
        task_resume(freeTask->joinQueue);
    }

    freeTask->procTime += systime() - freeTask->lastExecutionTime;
    freeTask->execTime = systime() - freeTask->creationTime;
    printf("Task %d exit: execution time %d ms, processor time %d ms, %d activations\n", freeTask->tid, freeTask->execTime, freeTask->procTime, freeTask->activations);
    
    countTasks--;

    if (taskExec == &taskDisp) {
        task_switch(&taskMain);
    }
    else {
        task_switch(&taskDisp);
    }
}

int task_switch(task_t *task) {
    task_t* prevTask;

    prevTask = taskExec;
    taskExec = task;

    prevTask->procTime += systime() - prevTask->lastExecutionTime;

    task->activations++;
    task->lastExecutionTime = systime();

    if (swapcontext(&(prevTask->context), &(task->context)) < 0) {
        perror("Erro na troca de contexto: ");
        taskExec = prevTask;
        return -1;
    }

    return 0;
}

int task_id() {
    return (taskExec->tid);
}

void task_suspend(task_t *task, task_t **queue) {
    /* Se task for nulo, considera a tarefa corrente. */
    if (task == NULL) {
        task = taskExec;
    }

    /* Se queue for nulo, n�o retira a tarefa da fila atual. */
    if (queue != NULL) {
        if (task->queue != NULL) {
            queue_remove((queue_t**)(task->queue), (queue_t*)task);
        }
        queue_append((queue_t**)queue, (queue_t*)task);
        task->queue = queue;
    }

    task->estado = 's';
}

void task_resume(task_t *task) {
    /* Remove a task de sua fila atual e coloca-a na fila de tasks prontas. */
    if (task->queue != NULL) {
        queue_remove((queue_t**)(task->queue), (queue_t*)task);
    }

    queue_append((queue_t**)&readyQueue, (queue_t*)task);
    task->queue = &readyQueue;
    task->estado = 'r';
}

void task_yield() {
    if (taskExec->estado != 's') {
        /* Recoloca a task no final da fila de prontas */
        queue_append((queue_t**)&readyQueue, (queue_t*)taskExec);
        taskExec->queue = &readyQueue;
        taskExec->estado = 'r';
    }

    /* Volta o controle para o dispatcher. */
    task_switch(&taskDisp);
}

void task_setprio(task_t* task, int prio) {
    if (task == NULL) {
        task = taskExec;
    }
    if (prio <= MAX_PRIO && prio >= MIN_PRIO) {
        task->prio = prio;
        task->dynPrio = prio;
    }
}

int task_getprio(task_t* task) {
    if (task == NULL) {
        task = taskExec;
    }
    return task->prio;
}

int task_join(task_t* task) {
    if (task == NULL) {
        return -1;
    }
    if (task->estado == 'x') {
        return task->exitCode;
    }

    /* Se a tarefa existir e n�o tiver terminado */
    preempcao = 0; // Impede preemp��o
    task_suspend(NULL, &(task->joinQueue));
    preempcao = 1; // Retoma preemp��o
    
    task_yield();
    return task->exitCode;
}

void task_sleep(int t) {
    if(t > 0) {
        taskExec->awakeTime = systime() + t*1000; // systime() � em milissegundos.

        preempcao = 0; // Impede preemp��o
        task_suspend(NULL, &sleepQueue);
        preempcao = 1; // Retoma preemp��o
        
        task_yield(); // Volta para o dispatcher.
    }
}

void bodyDispatcher(void* arg) {
    task_t* iterator;
    task_t* awake;
    unsigned int time;

    while (countTasks > 0) {
        if(readyQueue != NULL) {
            task_t* next = scheduler();

            if (next != NULL) {
                /* Coloca a tarefa em execu��o */
                /* Reseta as ticks */
                remainingTicks = RESET_TICKS;
                queue_remove((queue_t**)&readyQueue, (queue_t*)next);
                next->queue = NULL;
                next->estado = 'e';
                task_switch(next);

                /* Libera a memoria da task, caso ela tenha dado exit. */
                if (freeTask != NULL) {
                    free(freeTask->context.uc_stack.ss_sp);
                    freeTask = NULL;
                }
            }
        }

        /* Percorre a fila de tasks dormindo e acorda as tasks que devem ser acordadas. */
        if (sleepQueue != NULL) {
            iterator = sleepQueue;
            time = systime();
            do {
                if(iterator->awakeTime <= time) {
                    awake = iterator;
                    iterator = iterator->next;
                    task_resume(awake);
                }
                else {
                    iterator = iterator->next;
                }
            } while (iterator != sleepQueue && sleepQueue != NULL);
        }
    }
    task_exit(0);
}

task_t* scheduler() {
    task_t* iterator;
    task_t* nextTask;
    int minDynPrio;
    int minPrio;

    iterator = readyQueue;
    nextTask = NULL;
    minDynPrio = MAX_PRIO + 1;
    minPrio = MAX_PRIO + 1;

    /* Se a fila estiver vazia, retorna NULL. */
    if (iterator == NULL) {
        return NULL;
    }

    /* Busca a tarefa com menor dynPrio para executar. */
    do {
        if (iterator->dynPrio < minDynPrio) {
            nextTask = iterator;
            minDynPrio = iterator->dynPrio;
            minPrio = iterator->prio;
        }
        else if (iterator->dynPrio == minDynPrio) { /* Desempate */
            if (iterator->prio < minPrio) {
                nextTask = iterator;
                minDynPrio = iterator->dynPrio;
                minPrio = iterator->prio;
            }
        }

        iterator = iterator->next;
    } while (iterator != readyQueue);

    /* Retira a tarefa da fila e reseta sua prioridade dinamica. */
    nextTask->dynPrio = nextTask->prio;
    nextTask->dynPrio += ALPHA_PRIO; /* Para n�o precisar verificar se cada outra task � a nextTask ou n�o. */

    /* Atualiza a dynprio das outras tarefas. */
    iterator = readyQueue;
    if (iterator != NULL) {
        do {
            iterator->dynPrio -= ALPHA_PRIO;
            iterator = iterator->next;
        } while (iterator != readyQueue);
    }

    return nextTask;
}

void tickHandler() {
    systemTime++;

    if (taskExec != &taskDisp) {
        remainingTicks--;

        if (preempcao && remainingTicks <= 0) {
            task_yield();
        }
    }
}

unsigned int systime() {
    return systemTime * TICK_MICROSECONDS / 1000;
}

int sem_create(semaphore_t* s, int value) {
    if (s == NULL) {
        return -1;
    }
    
    preempcao = 0; // Impede preemp��o
    s->queue = NULL;
    s->value = value;
    s->active = 1;

    preempcao = 1; // Retoma preemp��o
    if(remainingTicks <= 0) {
        task_yield();
    }

    return 0;
}

int sem_down(semaphore_t* s) {
    if (s == NULL || !(s->active)) {
        return -1;
    }

    preempcao = 0; // Impede preemp��o
    s->value--;
    if (s->value < 0) {
        // Caso n�o existam mais vagas no sem�foro, suspende a tarefa.
        task_suspend(taskExec, &(s->queue));

        preempcao = 1; // Retoma preemp��o
        task_yield();

        // Se a tarefa foi acordada devido a um sem_destroy, retorna -1.
        if (!(s->active)) {
            return -1;
        }
        
        return 0;
    }
    
    preempcao = 1; // Retoma preemp��o
    if(remainingTicks <= 0) {
        task_yield();
    }
    return 0;
}

int sem_up(semaphore_t* s) {
    if (s == NULL || !(s->active)) {
        return -1;
    }
    
    preempcao = 0; // Impede preemp��o
    s->value++;
    if (s->value <= 0) {
        task_resume(s->queue);
    }
    preempcao = 1; // Retoma preemp��o
    
    if(remainingTicks <= 0) {
        task_yield();
    }
    return 0;
}

int sem_destroy(semaphore_t* s) {
    if (s == NULL || !(s->active)) {
        return -1;
    }
    
    preempcao = 0; // Impede preemp��o
    s->active = 0;
    while (s->queue != NULL) {
        task_resume(s->queue);
    }

    preempcao = 1; // Retoma preemp��o
    if(remainingTicks <= 0) {
        task_yield();
    }
    return 0;
}

int mutex_create(mutex_t* m) {
    if (m == NULL) {
        return -1;
    }

    preempcao = 0; // Impede preemp��o
    m->queue = NULL;
    m->value = 1;
    m->active = 1;
    preempcao = 1; // Retoma preemp��o

    if (remainingTicks <= 0) {
        task_yield();
    }

    return 0;
}

int mutex_lock(mutex_t* m) {
    if (m == NULL || !(m->active)) {
        return -1;
    }

    preempcao = 0; // Impede preemp��o

    if (m->value == 0) { // Se j� estiver travado, suspende a task
        task_suspend(taskExec, &(m->queue));

        preempcao = 1; // Retoma preemp��o
        task_yield();

        // Se a tarefa foi acordada devido a um mutex_destroy, retorna -1.
        if (!(m->active)) {
            return -1;
        }

        return 0;
    }

    m->value = 0; // Se n�o estiver travado, trava e obt�m o mutex.

    preempcao = 1; // Retoma preemp��o
    if (remainingTicks <= 0) {
        task_yield();
    }
    return 0;
}

int mutex_unlock(mutex_t* m) {
    if (m == NULL || !(m->active)) {
        return -1;
    }

    preempcao = 0; // Impede preemp��o

    if (m->queue != NULL) { // Se alguma task estiver esperando na fila, mant�m o mutex travado (para a pr�xima task) e acorda a primeira task da fila.
        task_resume(m->queue);
    }
    else { // Se n�o tiver nenhuma task esperando, libera o mutex.
        m->value = 1;
    }

    preempcao = 1; // Retoma preemp��o
    if (remainingTicks <= 0) {
        task_yield();
    }
    return 0;
}

int mutex_destroy(mutex_t* m) {
    if (m == NULL || !(m->active)) {
        return -1;
    }

    preempcao = 0; // Impede preemp��o
    m->active = 0;
    while (m->queue != NULL) {
        task_resume(m->queue);
    }

    preempcao = 1; // Retoma preemp��o
    if (remainingTicks <= 0) {
        task_yield();
    }
    return 0;
}

int barrier_create(barrier_t* b, int N) {
    if (b == NULL || N <= 0) {
        return -1;
    }
    
    preempcao = 0; // Impede preemp��o
    b->maxTasks = N;
    b->countTasks = 0;
    b->active = 1;
    
    preempcao = 1; // Retoma preemp��o
    if(remainingTicks <= 0) {
        task_yield();
    }
    return 0;
}

int barrier_join(barrier_t* b) {
    if (b == NULL || !(b->active)) {
        return -1;
    }
    
    preempcao = 0; // Impede preemp��o
    b->countTasks++;

    if (b->countTasks == b->maxTasks) {
        while (b->queue != NULL) {
            task_resume(b->queue);
        }
        b->countTasks = 0;
        preempcao = 1; // Retoma preemp��o
        if(remainingTicks <= 0) {
            task_yield();
        }
        return 0;
    }

    task_suspend(taskExec, &(b->queue));
    preempcao = 1; // Retoma preemp��o
    task_yield();
    
    if(!(b->active)) {
        return -1;
    }
    return 0;
}

int barrier_destroy(barrier_t* b) {
    if (b == NULL || !(b->active)) {
        return -1;
    }
    
    preempcao = 0; // Impede preemp��o
    b->active = 0;
    while (b->queue != NULL) {
        task_resume(b->queue);
    }

    preempcao = 1; // Retoma preemp��o
    if(remainingTicks <= 0) {
        task_yield();
    }
    return 0;
}

int mqueue_create(mqueue_t* queue, int max, int size) {
    if(queue == NULL) {
        return -1;
    }
    
    preempcao = 0; // Impede preemp��o
    
    queue->content = malloc(max * size);
    queue->messageSize = size;
    queue->maxMessages = max;
    queue->countMessages = 0;
    
    sem_create(&(queue->sBuffer), 1);
    sem_create(&(queue->sItem), 0);
    sem_create(&(queue->sVaga), max);
    
    queue->active = 1;
    
    preempcao = 1; // Retoma preemp��o
    if(remainingTicks <= 0) {
        task_yield();
    }
    return 0;
}

int mqueue_send(mqueue_t* queue, void* msg) {
    if (queue == NULL || !(queue->active)) {
        return -1;
    }
    
    if (sem_down(&(queue->sVaga)) == -1) return -1;
    if (sem_down(&(queue->sBuffer)) == -1) return -1;
    
    memcpy(queue->content + queue->countMessages * queue->messageSize, msg, queue->messageSize);
    ++(queue->countMessages);
        
    sem_up(&(queue->sBuffer));
    sem_up(&(queue->sItem));
    
    return 0;
}

int mqueue_recv(mqueue_t* queue, void* msg) {
    if (queue == NULL || !(queue->active)) {
        return -1;
    }
    
    if (sem_down(&(queue->sItem)) == -1) return -1;
    if (sem_down(&(queue->sBuffer)) == -1) return -1;
    
    --(queue->countMessages);
    memcpy(msg, queue->content, queue->messageSize);
    memmove(queue->content, queue->content + queue->messageSize, queue->countMessages * queue->messageSize);
    
    sem_up(&(queue->sBuffer));
    sem_up(&(queue->sVaga));
    
    return 0;
}

int mqueue_destroy(mqueue_t* queue) {
    if (queue == NULL || !(queue->active)) {
        return -1;
    }
    
    queue->active = 0;
    free(queue->content);
    sem_destroy(&(queue->sBuffer));
    sem_destroy(&(queue->sItem));
    sem_destroy(&(queue->sVaga));
    
    return 0;
}

int mqueue_msgs(mqueue_t* queue) {
    if (queue == NULL || !(queue->active)) {
        return -1;
    }

    return queue->countMessages;
}

int diskdriver_init(int* numBlocks, int* blockSize) {
    int qtdBlocos;
    int tamBloco;

    if (disk_cmd(DISK_CMD_INIT, 0, NULL) < 0) {
        return -1;
    }
    qtdBlocos = disk_cmd(DISK_CMD_DISKSIZE, 0, NULL);
    tamBloco = disk_cmd(DISK_CMD_BLOCKSIZE, 0, NULL);
    if (qtdBlocos < 0 || tamBloco < 0) {
        return -1;
    }

    *numBlocks = qtdBlocos;
    *blockSize = tamBloco;

    disco.numBlocks = qtdBlocos;
    disco.blockSize = tamBloco;
    disco.diskQueue = NULL;
    disco.requestQueue = NULL;
    disco.livre = 1;
    disco.sinal = 0;
    
    sem_create(&(disco.semaforo), 1);

    return 0;
}

int disk_block_read(int block, void* buffer) {
    diskrequest_t* request;

    if (sem_down(&(disco.semaforo)) < 0) {
        return -1;
    }

    request = malloc(sizeof(diskrequest_t));
    request->task = taskExec;
    request->operation = DISK_REQUEST_READ;
    request->block = block;
    request->buffer = buffer;
    request->next = NULL;
    request->prev = NULL;

    queue_append((queue_t**)&(disco.requestQueue), (queue_t*)request);

    if (taskDiskMgr.estado == 's') {
        task_resume(&taskDiskMgr);
    }

    if (sem_up(&(disco.semaforo))) {
        return -1;
    }

    task_suspend(taskExec, &(disco.diskQueue));
    task_yield();

    return 0;
}

int disk_block_write(int block, void* buffer) {
    diskrequest_t* request;

    if (sem_down(&(disco.semaforo)) < 0) {
        return -1;
    }

    request = malloc(sizeof(diskrequest_t));
    request->task = taskExec;
    request->operation = DISK_REQUEST_WRITE;
    request->block = block;
    request->buffer = buffer;
    request->next = NULL;
    request->prev = NULL;

    queue_append((queue_t**)&(disco.requestQueue), (queue_t*)request);

    if (taskDiskMgr.estado == 's') {
        task_resume(&taskDiskMgr);
    }

    if (sem_up(&(disco.semaforo))) {
        return -1;
    }

    task_suspend(taskExec, &(disco.diskQueue));
    task_yield();

    return 0;
}

void bodyDiskManager(void* arg) {
    diskrequest_t* request;

    while (1) {
        sem_down(&(disco.semaforo));
        
        if (disco.sinal) {
            disco.sinal = 0;
            task_resume(disco.diskQueue);
            disco.livre = 1;
        }

        if (disco.livre && disco.requestQueue != NULL) {
            request = (diskrequest_t*) queue_remove((queue_t**)&(disco.requestQueue), (queue_t*)disco.requestQueue);
            if (request->operation == DISK_REQUEST_READ) {
                disk_cmd(DISK_CMD_READ, request->block, request->buffer);
                disco.livre = 0;
            }
            else if (request->operation == DISK_REQUEST_WRITE) {
                disk_cmd(DISK_CMD_WRITE, request->block, request->buffer);
                disco.livre = 0;
            }
            free(request);
        }

        sem_up(&(disco.semaforo));
        
        task_yield();
    }
}

void diskSignalHandler() {
#ifdef DEBUG
    printf("Sinal de disco recebido.\n");
#endif
    disco.sinal = 1;
}
