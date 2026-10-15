# A01 - Simulacao do Sistema PIX com Threads

Este projeto implementa, em um unico programa C, as tres etapas progressivas da
avaliacao:

1. demonstracao de uma condicao de corrida em uma base de contas compartilhada;
2. protecao da secao critica e escalonamento FCFS;
3. visualizacao do estado da fila por meio do sinal `SIGUSR1`.

O programa utiliza um modelo N:1: **N remetentes diferentes enviam R$ 10 para
uma unica conta destinataria**. As transferencias sao processadas por uma thread
pool pertencente ao sistema PIX.

## Requisitos

- macOS, Linux ou WSL;
- compilador C com suporte a C11;
- biblioteca POSIX Threads (`pthread`).

## Compilacao

Execute o comando a partir da raiz do repositorio:

```bash
cc -std=c11 -Wall -Wextra -Wpedantic -pthread ava01/pix.c -o ava01/pix
```

Se o comando terminar sem mensagens, o programa foi compilado sem erros ou
avisos.

## Uso

```text
./ava01/pix <questao1|questao2|questao3> <N> <tamanho da pool>
```

Exemplos:

```bash
./ava01/pix questao1 8 3
./ava01/pix questao2 8 3
./ava01/pix questao3 15 3
```

## Visao geral da solucao

| Recurso | Questao 1 | Questao 2 | Questao 3 |
|---|---:|---:|---:|
| Thread pool | Sim | Sim | Sim |
| Modelo N:1 | Sim | Sim | Sim |
| Base de contas compartilhada | Sim | Sim | Sim |
| Protecao dos saldos | Nao | Sim | Sim |
| Fila FCFS explicita | Nao | Sim | Sim |
| Debug por `SIGUSR1` | Nao | Nao | Sim |

As tres etapas estao no mesmo arquivo. O primeiro argumento seleciona quais
recursos serao ativados, permitindo comparar as etapas com os mesmos valores de
N e de tamanho da pool.

# Roteiro da gravacao

O roteiro abaixo foi organizado para uma gravacao de aproximadamente quatro a
cinco minutos.

## 1. Introducao - aproximadamente 30 segundos

Mostrar o arquivo `ava01/pix.c` e dizer:

> Este programa simula o processamento de transferencias PIX. Usamos o modelo
> N:1, no qual N remetentes enviam dinheiro para uma unica conta destinataria.
> As requisicoes sao processadas por uma thread pool do sistema PIX. Cada modo
> do programa representa uma etapa progressiva da avaliacao.

Explicar que cada remetente comeca com R$ 100 e envia R$ 10. A conta
destinataria comeca com saldo zero. Portanto, para N requisicoes, o saldo final
esperado e `N * R$ 10`.

## 2. Questao 1: condicao de corrida - aproximadamente 1 minuto

### Conceitos que precisam ser explicados

- **Recurso compartilhado:** a base de contas, especialmente o saldo da conta
  destinataria.
- **Secao critica:** o trecho que le, calcula e grava os saldos.
- **Condicao de corrida:** duas ou mais workers podem ler o mesmo saldo antigo e
  depois sobrescrever as atualizacoes umas das outras.
- **Thread pool:** conjunto fixo de threads que processa as requisicoes PIX.

No codigo, mostrar a funcao `processar_pix`. A secao critica realiza
conceitualmente estas operacoes:

```c
int saldo_destino_lido = destino->saldo;
destino->saldo = saldo_destino_lido + requisicao.valor;
```

A pausa de 200 ms entre a leitura e a escrita simula o tempo de acesso a base e
torna a corrida mais facil de observar. Ela nao cria a condicao de corrida; ela
somente aumenta a janela em que as threads podem se sobrepor.

### Demonstracao empirica

Primeiro, executar com somente uma requisicao:

```bash
./ava01/pix questao1 1 2
```

Apontar que o saldo esperado e o obtido sao R$ 10. Nao existe outra requisicao
para disputar o recurso.

Em seguida, executar:

```bash
./ava01/pix questao1 2 2
```

Apontar que as duas workers podem ler saldo zero e ambas gravar R$ 10. O saldo
esperado e R$ 20, mas o saldo obtido tende a ser R$ 10.

Usar um exemplo maior:

```bash
./ava01/pix questao1 8 3
```

O valor exato incorreto pode variar entre execucoes, pois essa variacao faz
parte da condicao de corrida. O ponto importante e mostrar que o saldo obtido e
menor que os R$ 80 esperados.

Para provar que o problema depende de concorrencia, executar:

```bash
./ava01/pix questao1 8 1
```

Com uma unica worker nao existe execucao concorrente da secao critica, e o saldo
final deve ser R$ 80.

Conclusao a ser falada:

> Nesta simulacao, a corrida torna-se evidente a partir de N igual a 2 quando a
> pool possui pelo menos duas workers. Com pool de tamanho 1, as requisicoes sao
> processadas sequencialmente e o problema nao aparece.

### Por que existem mutexes na questao 1?

O programa utiliza `mutex_distribuicao` para impedir que duas workers recebam a
mesma requisicao e `mutex_saida` para que os textos nao sejam misturados no
terminal. Esses mutexes **nao protegem os saldos**. A base de contas permanece
deliberadamente desprotegida no modo `questao1`.

## 3. Questao 2: protecao e FCFS - aproximadamente 1 minuto

Executar com os mesmos valores usados anteriormente:

```bash
./ava01/pix questao2 8 3
```

Destacar no cabecalho:

```text
Protecao dos saldos: SIM (mutex)
Escalonador FCFS explicito: SIM
```

### Diferenca em relacao a questao 1

A questao 2 acrescenta dois elementos:

1. `mutex_base`, que permite somente uma worker por vez na secao critica dos
   saldos;
2. uma fila circular FCFS, que guarda as requisicoes que ainda nao conseguiram
   uma worker.

O mecanismo minimo de protecao e:

```c
pthread_mutex_lock(&mutex_base);

/* leitura, calculo e escrita dos saldos */

pthread_mutex_unlock(&mutex_base);
```

As validacoes das requisicoes ainda podem ocorrer concorrentemente. Apenas o
acesso que precisa ser atomico fica dentro do mutex.

### Como provar o FCFS

Com N igual a 8 e pool igual a 3, somente tres requisicoes podem estar em
atendimento inicialmente. As demais aguardam na fila. Mostrar as linhas:

```text
[FCFS 01] worker 1 retirou PIX 1; restam 7 na fila
[FCFS 02] worker 2 retirou PIX 2; restam 6 na fila
[FCFS 03] worker 3 retirou PIX 3; restam 5 na fila
...
[FCFS 08] worker ... retirou PIX 8; restam 0 na fila
```

Os identificadores retirados aparecem na ordem `1, 2, 3, ..., 8`, comprovando o
First Come, First Served: a primeira requisicao que entra e a primeira que inicia
o atendimento.

A ordem das mensagens de conclusao pode variar porque existem varias workers.
Isso nao viola o FCFS: o escalonador garante a ordem de retirada da fila, nao que
todas as transferencias terminem na mesma ordem.

Ao final, apontar:

```text
Saldo esperado do destinatario: R$ 80
Saldo obtido do destinatario:   R$ 80
Resultado: CORRETO
```

## 4. Questao 3: debugging por sinais - aproximadamente 1 minuto

A questao 3 preserva o mutex e o FCFS da questao 2 e acrescenta o debugging por
sinal. Usar mais requisicoes para haver tempo suficiente para a demonstracao:

```bash
./ava01/pix questao3 15 3
```

Durante a execucao, pressionar somente a tecla:

```text
d
```

Nao e necessario pressionar Enter. O programa deve exibir um snapshot como:

```text
========== DEBUG RECEBIDO POR SIGUSR1 ==========
Concluidas: 10/15
Workers em execucao: [worker 1 -> PIX 11] [worker 2 -> PIX 12]
Fila FCFS (2 aguardando): PIX 14 -> PIX 15
Saldo atual do destinatario: R$ 100
================================================
```

Os numeros exatos do snapshot dependem do instante em que a tecla for
pressionada.

### Caminho do sinal

```text
tecla d
   -> kill(getpid(), SIGUSR1)
   -> SIGUSR1 e entregue ao processo
   -> a thread de monitoramento recebe o sinal com sigwait()
   -> imprimir_estado_debug() mostra o snapshot
```

Fala sugerida:

> Uma thread monitora o teclado. Quando pressionamos d, ela envia SIGUSR1 ao
> proprio processo. Outra thread espera por esse sinal usando sigwait. Ao
> recebe-lo, ela imprime quantas requisicoes terminaram, o trabalho atual de
> cada worker, a fila FCFS e o saldo do destinatario. O processamento continua
> depois do diagnostico.

### Prova alternativa do sinal

Ao iniciar a questao 3, o programa tambem mostra seu PID e um comando como:

```text
Tambem e possivel usar: kill -USR1 12345
```

Esse comando pode ser executado em outro terminal enquanto o programa estiver
rodando:

```bash
kill -USR1 12345
```

Deve ser usado o numero realmente exibido pelo programa. O mesmo snapshot sera
apresentado, comprovando que o debugging utiliza um sinal do sistema operacional
e nao apenas uma chamada direta feita pelo teclado.

## 5. Encerramento - aproximadamente 20 segundos

Fala sugerida:

> Na primeira etapa demonstramos a condicao de corrida. Na segunda, protegemos a
> secao critica e adicionamos uma fila FCFS. Na terceira, mantivemos a solucao
> correta e adicionamos a inspecao do estado por SIGUSR1. Assim, o mesmo programa
> permite observar tanto o problema quanto sua correcao e seu monitoramento.

# Sequencia resumida para a demonstracao

Se o tempo do video estiver curto, usar esta sequencia:

```bash
cc -std=c11 -Wall -Wextra -Wpedantic -pthread ava01/pix.c -o ava01/pix

./ava01/pix questao1 8 3
./ava01/pix questao2 8 3
./ava01/pix questao3 15 3
```

Na terceira execucao, pressionar `d` enquanto ainda houver requisicoes na fila.

# Resultados esperados

| Execucao | O que deve ser observado |
|---|---|
| `questao1 1 2` | saldo correto, pois nao ha disputa |
| `questao1 2 2` | corrida evidente a partir de N igual a 2 |
| `questao1 8 3` | saldo normalmente menor que R$ 80 |
| `questao1 8 1` | saldo correto, pois ha apenas uma worker |
| `questao2 8 3` | retirada FCFS e saldo final de R$ 80 |
| `questao3 15 3` | snapshot por `SIGUSR1` e saldo final de R$ 150 |

# Possivel divisao entre tres integrantes

- **Integrante 1:** arquitetura, modelo N:1 e demonstracao da condicao de corrida;
- **Integrante 2:** mutex da base, fila circular e demonstracao do FCFS;
- **Integrante 3:** tecla `d`, `SIGUSR1`, `sigwait` e snapshot de debugging.

Todos podem participar da introducao ou do encerramento, se necessario.

# Observacoes importantes

- A ordem das workers pode mudar entre execucoes; isso e esperado em um programa
  concorrente.
- O valor incorreto da questao 1 tambem pode variar. A avaliacao deve comparar o
  saldo obtido com o saldo esperado.
- Nao remover as pausas de 100 ms e 200 ms antes da gravacao. Elas simulam o
  tempo de processamento e tornam os comportamentos faceis de visualizar.
- Para repetir a demonstracao da questao 3, execute novamente o programa. O PID
  muda a cada execucao.
