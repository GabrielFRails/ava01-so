#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SALDO_INICIAL 100
#define VALOR_PIX 10

/*
 * Tempo-base da simulacao em milissegundos. Pode ser alterado aqui ou durante
 * a compilacao com -DTEMPO_OPERACAO_MS=800, por exemplo.
 */
#ifndef TEMPO_OPERACAO_MS
#define TEMPO_OPERACAO_MS 400
#endif

typedef enum {
    QUESTAO_1,
    QUESTAO_2,
    QUESTAO_3
} Modo;

typedef struct {
    int id;
    int saldo;
} Conta;

typedef struct {
    int id;
    int remetente;
    int destinatario;
    int valor;
} RequisicaoPix;

typedef struct {
    RequisicaoPix *itens;
    int inicio;
    int fim;
    int quantidade;
    int capacidade;
} FilaFcfs;

static Conta *contas;
static RequisicaoPix *requisicoes;
static int quantidade_requisicoes;
static int proxima_requisicao;
static int ordem_fcfs;
static int tamanho_pool;
static int conta_destinatario;
static int requisicoes_concluidas;
static int *requisicoes_ativas;
static Modo modo;
static FilaFcfs fila;
static sigset_t conjunto_debug;
static volatile sig_atomic_t encerrando_debug;

/* Protege a distribuicao das requisicoes entre as workers. */
static pthread_mutex_t mutex_distribuicao = PTHREAD_MUTEX_INITIALIZER;

/* Protege os saldos somente no modo questao2. */
static pthread_mutex_t mutex_base = PTHREAD_MUTEX_INITIALIZER;

/* Impede apenas que as mensagens se misturem no terminal. */
static pthread_mutex_t mutex_saida = PTHREAD_MUTEX_INITIALIZER;

static void dormir_ms(long milissegundos) {
    struct timespec tempo = {
        .tv_sec = milissegundos / 1000,
        .tv_nsec = (milissegundos % 1000) * 1000000L,
    };

    while (nanosleep(&tempo, &tempo) == -1 && errno == EINTR) {
    }
}

static void fila_inicializar(int capacidade) {
    fila.itens = calloc((size_t)capacidade, sizeof(*fila.itens));
    if (fila.itens == NULL) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    fila.inicio = 0;
    fila.fim = 0;
    fila.quantidade = 0;
    fila.capacidade = capacidade;
}

static void fila_inserir(RequisicaoPix requisicao) {
    fila.itens[fila.fim] = requisicao;
    fila.fim = (fila.fim + 1) % fila.capacidade;
    fila.quantidade++;
}

/*
 * Retira sempre o item mais antigo da fila. O mutex garante que duas workers
 * nao retirem a mesma requisicao.
 */
static bool fila_retirar(RequisicaoPix *requisicao, int worker) {
    pthread_mutex_lock(&mutex_distribuicao);

    if (fila.quantidade == 0) {
        pthread_mutex_unlock(&mutex_distribuicao);
        return false;
    }

    *requisicao = fila.itens[fila.inicio];
    fila.inicio = (fila.inicio + 1) % fila.capacidade;
    fila.quantidade--;
    ordem_fcfs++;

    if (modo == QUESTAO_3) {
        requisicoes_ativas[worker - 1] = requisicao->id;
    }

    /* A mensagem e impressa ainda dentro do escalonador para provar a ordem. */
    pthread_mutex_lock(&mutex_saida);
    printf("[FCFS %02d] worker %d retirou PIX %d; restam %d na fila\n",
           ordem_fcfs, worker, requisicao->id, fila.quantidade);
    pthread_mutex_unlock(&mutex_saida);

    pthread_mutex_unlock(&mutex_distribuicao);
    return true;
}

static bool obter_requisicao(RequisicaoPix *requisicao, int worker) {
    if (modo != QUESTAO_1) {
        return fila_retirar(requisicao, worker);
    }

    /* Questao 1: distribuicao simples, sem o escalonador FCFS explicito. */
    pthread_mutex_lock(&mutex_distribuicao);
    if (proxima_requisicao >= quantidade_requisicoes) {
        pthread_mutex_unlock(&mutex_distribuicao);
        return false;
    }

    *requisicao = requisicoes[proxima_requisicao];
    proxima_requisicao++;
    pthread_mutex_unlock(&mutex_distribuicao);

    pthread_mutex_lock(&mutex_saida);
    printf("[worker %d] iniciou o PIX %d\n", worker, requisicao->id);
    pthread_mutex_unlock(&mutex_saida);
    return true;
}

static int processar_pix(RequisicaoPix requisicao, int worker) {
    Conta *origem = &contas[requisicao.remetente];
    Conta *destino = &contas[requisicao.destinatario];

    /* A validacao utiliza metade do tempo-base. */
    dormir_ms(TEMPO_OPERACAO_MS / 2);

    if (modo != QUESTAO_1) {
        pthread_mutex_lock(&mutex_base);
    }

    /* INICIO DA SECAO CRITICA: leitura, calculo e escrita dos saldos. */
    int saldo_origem_lido = origem->saldo;
    int saldo_destino_lido = destino->saldo;
    dormir_ms(TEMPO_OPERACAO_MS);

    if (saldo_origem_lido >= requisicao.valor) {
        origem->saldo = saldo_origem_lido - requisicao.valor;
        destino->saldo = saldo_destino_lido + requisicao.valor;
    }
    /* FIM DA SECAO CRITICA. */

    /* Mantem saldo e estado de debugging consistentes no mesmo instante. */
    if (modo == QUESTAO_3) {
        pthread_mutex_lock(&mutex_distribuicao);
        requisicoes_ativas[worker - 1] = 0;
        requisicoes_concluidas++;
        pthread_mutex_unlock(&mutex_distribuicao);
    }

    if (modo != QUESTAO_1) {
        pthread_mutex_unlock(&mutex_base);
    }

    return saldo_destino_lido;
}

static void *executar_worker(void *argumento) {
    int worker = *(int *)argumento;
    RequisicaoPix requisicao;

    while (obter_requisicao(&requisicao, worker)) {
        int saldo_destino_lido = processar_pix(requisicao, worker);

        pthread_mutex_lock(&mutex_saida);
        printf("[worker %d] concluiu PIX %d (leu saldo do destino = R$ %d)\n",
               worker, requisicao.id, saldo_destino_lido);
        pthread_mutex_unlock(&mutex_saida);
    }

    return NULL;
}

static void imprimir_estado_debug(void) {
    pthread_mutex_lock(&mutex_base);
    pthread_mutex_lock(&mutex_distribuicao);
    int saldo_atual = contas[conta_destinatario].saldo;
    pthread_mutex_lock(&mutex_saida);

    printf("\n========== DEBUG RECEBIDO POR SIGUSR1 ==========\n");
    printf("Concluidas: %d/%d\n", requisicoes_concluidas,
           quantidade_requisicoes);
    printf("Workers em execucao: ");

    bool alguma_ativa = false;
    for (int i = 0; i < tamanho_pool; i++) {
        if (requisicoes_ativas[i] != 0) {
            printf("[worker %d -> PIX %d] ", i + 1, requisicoes_ativas[i]);
            alguma_ativa = true;
        }
    }
    if (!alguma_ativa) {
        printf("nenhuma");
    }

    printf("\nFila FCFS (%d aguardando): ", fila.quantidade);
    if (fila.quantidade == 0) {
        printf("vazia");
    } else {
        for (int i = 0; i < fila.quantidade; i++) {
            int posicao = (fila.inicio + i) % fila.capacidade;
            printf("PIX %d%s", fila.itens[posicao].id,
                   i + 1 == fila.quantidade ? "" : " -> ");
        }
    }

    printf("\nSaldo atual do destinatario: R$ %d\n", saldo_atual);
    printf("================================================\n\n");
    fflush(stdout);

    pthread_mutex_unlock(&mutex_saida);
    pthread_mutex_unlock(&mutex_distribuicao);
    pthread_mutex_unlock(&mutex_base);
}

/* SIGUSR1 fica bloqueado nas demais threads e e recebido aqui por sigwait. */
static void *monitorar_sinal(void *argumento) {
    (void)argumento;

    while (true) {
        int sinal_recebido;
        int erro = sigwait(&conjunto_debug, &sinal_recebido);
        if (erro != 0) {
            continue;
        }

        if (encerrando_debug) {
            break;
        }

        imprimir_estado_debug();
    }

    return NULL;
}

/* A tecla d envia SIGUSR1 ao proprio processo. Em terminal, nao exige ENTER. */
static void *ler_tecla_debug(void *argumento) {
    (void)argumento;
    struct termios terminal_original;
    bool terminal_alterado = false;

    if (isatty(STDIN_FILENO) &&
        tcgetattr(STDIN_FILENO, &terminal_original) == 0) {
        struct termios terminal_direto = terminal_original;
        terminal_direto.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
        terminal_direto.c_cc[VMIN] = 0;
        terminal_direto.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal_direto) == 0) {
            terminal_alterado = true;
        }
    }

    while (!encerrando_debug) {
        fd_set descritores;
        FD_ZERO(&descritores);
        FD_SET(STDIN_FILENO, &descritores);
        struct timeval espera = {.tv_sec = 0, .tv_usec = 100000};

        int disponivel = select(STDIN_FILENO + 1, &descritores, NULL, NULL,
                                &espera);
        if (disponivel > 0 && FD_ISSET(STDIN_FILENO, &descritores)) {
            char tecla;
            ssize_t lidos = read(STDIN_FILENO, &tecla, 1);
            if (lidos == 1 && (tecla == 'd' || tecla == 'D')) {
                kill(getpid(), SIGUSR1);
            } else if (lidos == 0) {
                break;
            }
        }
    }

    if (terminal_alterado) {
        tcsetattr(STDIN_FILENO, TCSANOW, &terminal_original);
    }

    return NULL;
}

static int ler_inteiro_positivo(const char *texto, const char *nome) {
    char *fim;
    errno = 0;
    long valor = strtol(texto, &fim, 10);

    if (errno != 0 || *texto == '\0' || *fim != '\0' || valor <= 0 ||
        valor > 100000) {
        fprintf(stderr, "%s deve ser um inteiro entre 1 e 100000.\n", nome);
        exit(EXIT_FAILURE);
    }

    return (int)valor;
}

static void mostrar_uso(const char *programa) {
    fprintf(stderr,
            "Uso: %s <questao1|questao2|questao3> <N> <tamanho da pool>\n"
            "Exemplos:\n"
            "  %s questao1 8 3\n"
            "  %s questao2 8 3\n"
            "  %s questao3 15 3\n",
            programa, programa, programa, programa);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        mostrar_uso(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "questao1") == 0) {
        modo = QUESTAO_1;
    } else if (strcmp(argv[1], "questao2") == 0) {
        modo = QUESTAO_2;
    } else if (strcmp(argv[1], "questao3") == 0) {
        modo = QUESTAO_3;
    } else {
        mostrar_uso(argv[0]);
        return EXIT_FAILURE;
    }

    quantidade_requisicoes = ler_inteiro_positivo(argv[2], "N");
    tamanho_pool = ler_inteiro_positivo(argv[3], "Tamanho da pool");

    contas = calloc((size_t)quantidade_requisicoes + 1, sizeof(*contas));
    requisicoes = calloc((size_t)quantidade_requisicoes, sizeof(*requisicoes));
    pthread_t *threads = calloc((size_t)tamanho_pool, sizeof(*threads));
    int *ids_workers = calloc((size_t)tamanho_pool, sizeof(*ids_workers));
    requisicoes_ativas = calloc((size_t)tamanho_pool,
                                sizeof(*requisicoes_ativas));

    if (contas == NULL || requisicoes == NULL || threads == NULL ||
        ids_workers == NULL || requisicoes_ativas == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }

    if (modo != QUESTAO_1) {
        fila_inicializar(quantidade_requisicoes);
    }

    /* Modelo N:1: N remetentes diferentes enviam para um unico destinatario. */
    conta_destinatario = quantidade_requisicoes;
    contas[conta_destinatario].id = conta_destinatario;
    contas[conta_destinatario].saldo = 0;

    for (int i = 0; i < quantidade_requisicoes; i++) {
        contas[i].id = i;
        contas[i].saldo = SALDO_INICIAL;

        requisicoes[i].id = i + 1;
        requisicoes[i].remetente = i;
        requisicoes[i].destinatario = conta_destinatario;
        requisicoes[i].valor = VALOR_PIX;

        if (modo != QUESTAO_1) {
            fila_inserir(requisicoes[i]);
        }
    }

    const char *nome_modo = modo == QUESTAO_1 ? "QUESTAO 1"
                            : modo == QUESTAO_2 ? "QUESTAO 2"
                                               : "QUESTAO 3";
    printf("\nSistema PIX - %s\n", nome_modo);
    printf("Modelo N:1: %d remetentes -> conta destinataria %d\n",
           quantidade_requisicoes, conta_destinatario);
    printf("Thread pool: %d workers\n", tamanho_pool);
    printf("Protecao dos saldos: %s\n",
           modo == QUESTAO_1 ? "NAO" : "SIM (mutex)");
    printf("Escalonador FCFS explicito: %s\n\n",
           modo == QUESTAO_1 ? "NAO" : "SIM");

    pthread_t thread_sinal;
    pthread_t thread_teclado;
    if (modo == QUESTAO_3) {
        sigemptyset(&conjunto_debug);
        sigaddset(&conjunto_debug, SIGUSR1);
        if (pthread_sigmask(SIG_BLOCK, &conjunto_debug, NULL) != 0) {
            fprintf(stderr, "Nao foi possivel preparar SIGUSR1.\n");
            return EXIT_FAILURE;
        }

        printf("Debug por sinal: pressione d durante a execucao.\n");
        printf("Tambem e possivel usar: kill -USR1 %ld\n\n", (long)getpid());
        pthread_create(&thread_sinal, NULL, monitorar_sinal, NULL);
        pthread_create(&thread_teclado, NULL, ler_tecla_debug, NULL);
    }

    for (int i = 0; i < tamanho_pool; i++) {
        ids_workers[i] = i + 1;
        if (pthread_create(&threads[i], NULL, executar_worker,
                           &ids_workers[i]) != 0) {
            fprintf(stderr, "Erro ao criar a worker %d.\n", i + 1);
            return EXIT_FAILURE;
        }
    }

    for (int i = 0; i < tamanho_pool; i++) {
        pthread_join(threads[i], NULL);
    }

    if (modo == QUESTAO_3) {
        encerrando_debug = 1;
        kill(getpid(), SIGUSR1);
        pthread_join(thread_sinal, NULL);
        pthread_join(thread_teclado, NULL);
    }

    int saldo_esperado = quantidade_requisicoes * VALOR_PIX;
    int saldo_obtido = contas[conta_destinatario].saldo;

    printf("\n=============== RESULTADO ===============\n");
    printf("Saldo esperado do destinatario: R$ %d\n", saldo_esperado);
    printf("Saldo obtido do destinatario:   R$ %d\n", saldo_obtido);
    printf("Resultado: %s\n",
           saldo_obtido == saldo_esperado
               ? "CORRETO"
               : "INCORRETO - CONDICAO DE CORRIDA OBSERVADA");
    printf("=========================================\n");

    pthread_mutex_destroy(&mutex_distribuicao);
    pthread_mutex_destroy(&mutex_base);
    pthread_mutex_destroy(&mutex_saida);
    free(fila.itens);
    free(contas);
    free(requisicoes);
    free(requisicoes_ativas);
    free(threads);
    free(ids_workers);

    return EXIT_SUCCESS;
}
