# Próximo experimento de performance — 2026-09-20

Status: plano baseado no LOG e na leitura do código de `8758cb6`.
Nenhuma otimização foi implementada ou medida nesta análise.

## Decisão

Há uma chance concreta de reduzir o custo de sprites sem alterar os pixels:
adiar trabalho de projeção até saber que o objeto será desenhado e, numa
experiência separada, reduzir o custo do laço de rasterização por coluna.
Começar pela atribuição da projeção; não iniciar uma reescrita do renderer.

O objetivo de aceitação é economizar pelo menos um vblank por quadro em
duas vistas pesadas, com resultado idêntico e sem regressão sustentada nos
controles. Isso é um critério para investir, não uma previsão de resultado.
Não há evidência de que essas mudanças levem o jogo a 20–30 FPS.

## Evidência e limites

O histórico contém ganhos expressivos: PVS reduziu cast em até 42%; scalers
reduziram pack em até 44%; a rasterização de sprites por coluna reduziu seu
estágio em 41% no conjunto medido. Esses ganhos já estão incorporados ao jogo.
Eles não podem ser contados novamente como oportunidades.

As fases novas aumentaram a população: E1M6 comporta 405 objetos no hard,
177 deles monstros. A seleção visual continua limitada a 12 objetos de mundo,
além dos efeitos, mas a projeção percorre toda a lista ativa.

| Medição histórica | Quadro (vb) | Projeção (subticks) | Sprites (subticks) |
|---|---:|---:|---:|
| E1M6, horda (-1984,-1392), a9 | 24,71 | 4770 | 9321 |
| E1M6, início (32,1376), a201 | 18,7 | 6529 | 1511 |
| E1M7, (96,1232), a137 | 23,73 | 4412 | 5592 |

Fonte: LOG de 18–19/09 e CSVs em `out/sweep`. São pistas para escolher as
experiências, não baselines para uma implementação futura. Há inclusive uma
medição posterior local, `sweep-p2muls-x96y1232.csv`, com projeção 4010 nessa
vista de E1M7. O código atual já usa `billboard_muls_word` nos limites do
frustum; propor essa troca novamente seria repetir trabalho feito.

Como referência de escala: retirar 20% dos 9321 subticks de sprites de E1M6
equivaleria a cerca de 1,45 vb, ou aproximadamente 6% a mais de FPS naquele
quadro se o restante ficasse igual. Retirar também 25% da projeção daria cerca
de 2,38 vb no total: 24,71 → 22,33 vb, aproximadamente 2,43 → 2,69 FPS a 60 Hz.
São contas condicionais, não ganhos demonstrados. A taxa final deve ser medida
pela cadência, pois upload, DMA e espera por vblank afetam a conversão.

## Experiência 1 — projetar completamente só os objetos selecionados

### Trabalho hoje desperdiçado

Em `src/billboard/billboard.c`, `billboard_measure_object` calcula geometria,
limites horizontais e verticais, atlas, meia largura e altura projetada.
Somente depois, `src/billboard/billboard_projection.c`:

1. rejeita o objeto fora da tela;
2. consulta a profundidade das paredes para descartar um sprite oculto;
3. verifica se ele cabe entre os 12 objetos mais próximos;
4. pode substituir depois um objeto cuja medida completa já foi calculada.

Já existe uma árvore de profundidade: sua raiz contém a maior profundidade
de parede da tela. Já existe também a seleção dos 12 com cache do mais distante.
Não é necessário introduzir uma estrutura espacial para antecipar esses testes.

### Primeiro, medir a oportunidade

Adicionar uma instrumentação opcional específica, desligada no release:

- Contar objetos ativos, rejeitados por profundidade, fora do frustum,
  ocultos pelas paredes, recusados pelo limite de 12, substituídos e selecionados.
- Contar quantos poderiam sair imediatamente após calcular `forward` por
  estarem atrás da profundidade máxima da tela ou além do corte da seleção.
- Separar custo de transformação, geometria/projeção, consulta de oclusão,
  seleção/ordenação e efeitos. Medir overhead dos temporizadores; preferir
  amostragem e blocos grandes a um temporizador por operação.
- Registrar custos absolutos e quantidade de trabalho, não apenas percentuais.

Parar esta experiência se a maior parte do custo estiver na transformação
obrigatória de objetos que não podem sair cedo, ou se o teto de trabalho
evitável não justificar perseguir aproximadamente um vblank nas vistas alvo.
Não ampliar automaticamente o escopo para PVS de sprites, caches ou grids.

### Protótipo condicionado à medição

Manter a implementação atual como referência, selecionável por flag.

1. Calcular profundidade uma vez. Após a rejeição já existente, descartar
   `forward >= profundidade_maxima_da_tela`: nenhum intervalo da tela pode
   tornar esse objeto visível segundo o teste estrito atual.
2. Quando já houver 12 candidatos comprovadamente visíveis, rejeitar uma
   profundidade estritamente maior que a do mais distante antes de calcular
   geometria. Preservar o desempate atual por índice; inicialmente deixar
   profundidades iguais seguirem pelo caminho existente.
3. Para os sobreviventes, calcular os limites horizontais exatos e executar
   a mesma consulta de oclusão por intervalo.
4. Selecionar os mesmos 12, na mesma ordem. Finalizar os limites verticais e
   os campos restantes somente para os selecionados. Medir se guardar dados
   intermediários ou recomputar a geometria para até 12 objetos custa menos.

Não duplicar a transformação para implementar os testes. Não alterar o
contrato de `billboard_measure_object` usado pela IA em `billboard_enemy.c`.
O caminho especial pertence à projeção da cena; efeitos e desempates mantêm
seu comportamento. O teste contra paredes usa a árvore existente, não a
profundidade do centro do sprite, que esconderia objetos atrás de pilares.

### Prova

Criar um diferencial que execute a seleção/projeção antiga e a nova sobre o
mesmo estado e compare quantidade, IDs selecionados, ordem e todos os campos
de saída relevantes, sem comparar padding não inicializado. Incluir portas,
janelas, sprites parcialmente visíveis, explosões, cadáveres, efeitos,
empates e os três tamanhos de viewport. Confirmar que a verificação não altera
o estado ao executar a referência.

Controles negativos: remover um objeto visível e inverter um desempate.
Ambos precisam ser detectados. Exigir contadores de caminhos exercitados:
zero divergências sem rejeições antecipadas não prova o protótipo.
Manter `tools/test-billboard-projection.py` e as verificações de combate/IA.

## Experiência 2 — laço de pixels dos sprites

Esta oportunidade independe do resultado da experiência 1.

`src/renderer/renderer_billboard_draw.c::draw_sprite_column_rows` ainda faz
duas leituras de texel, dois remapeamentos e a composição de cada byte em C.
O LOG de 18/09 registra cerca de 170 ciclos no laço gerado e estima
140 para bytes opacos / 90 para transparentes em assembly. A economia de
15–20% no estágio é uma hipótese antiga ainda não implementada, não uma
medição atual. Conferir o código de máquina do build atual antes de investir.

Prototipar apenas o laço por coluna, preservando os cortes de porta/janela,
o DDA, a transparência e a ordem dos sprites. Evitar uma chamada por pixel e
medir o custo das chamadas por coluna/trecho. Manter uma alternativa C por flag.

A marcação de overlay deve continuar ocorrendo antes da primeira escrita
no tile: ela guarda a imagem de fundo que será restaurada no próximo quadro.
Chamadas auxiliares, salvamento de registradores e essa marcação entram no
custo real; uma conta só do miolo não demonstra ganho do estágio.

Usar `BILLBOARD_RASTER_VERIFY=1` contra o rasterizador de referência e provar
que os controles de corrupção de texel e omissão dos cortes falham também
quando o caminho novo está ativo. Exercitar todas as faixas verificadas pelo
harness e medir explicitamente a cobertura do novo caminho. Verificar também
restauração do fundo em quadros sem reconstrução, registradores e canários.
`npm run asm-diff` cobre paredes/overlays, não substitui esse diferencial de
sprites; executá-lo também se houver alteração em `renderer_hotpath.s` ou
nos consumidores/descritores de paredes.

## Matriz e método de decisão

Usar `tools/perf-sweep.ps1`, nível explícito e quatro direções por posição:

| Papel | Nível | Posição | Ângulos iniciais |
|---|---|---|---|
| Sprites próximos | E1M6 (5) | -1984,-1392 | 9,73,137,201 |
| Projeção cara | E1M6 (5) | 32,1376 | 9,73,137,201 |
| Cena mista | E1M7 (6) | 96,1232 | 9,73,137,201 |
| Regressão em cena simples | E1M1 (0) | -285,3295 | 9,73,137,201 |
| Portas e sprites | E1M2 (1) | -590,-2196 | 0,64,128,192 |
| Sprites grandes | E1M3 (2) | -1952,2448 | 0,64,128,192 |

Triagem em duas vistas pesadas e uma simples; ampliar a matriz apenas se a
experiência passar. Fixar dificuldade e viewport. Rodar no mínimo três pares
A/B, construídos na mesma sessão, com flags independentes para cada mudança.

`PERF_FIXED_POSE` não fixa os inimigos. Para atribuir custo com precisão,
acrescentar um modo de teste opt-in que capture um estado representativo e
repita a renderização desse estado imutável, incluindo atores, animações,
efeitos, portas e flashes. A preparação fica fora da janela medida. Publicar
uma assinatura dos inputs e contadores de trabalho; os dois builds têm que
coincidir. Esse modo ainda não existe: não confundir com `DEBUG_E2E_GOD`.

Depois validar cadência e jogo em movimento com simulação normal. Separar
builds de verificação dos de performance: sem `RENDERER_ASM_DIFF`,
`BILLBOARD_RASTER_VERIFY` ou diferencial de projeção durante a cronometragem.
Usar o probe de cadência release, pois `DEBUG_PERF` também executa diagnóstico
de subsectors por objeto e altera o caminho de upload.

Aceitar somente se:

- os diferenciais passarem, os controles negativos falharem e houver cobertura;
- houver economia reproduzível de pelo menos 1 vb em duas vistas pesadas;
- controles simples não regredirem mais de 2% de forma sustentada, descontado
  o ruído medido com baseline repetido/perturbação sem efeito;
- RAM livre continuar em pelo menos 20480 bytes e os limites de ROM passarem;
- as rotas E2E pertinentes completarem e o boot pelo frontend funcionar;
- capturas equivalentes preservarem pixels e a execução em movimento confirmar
  o benefício. Diferenças de fase do uploader não são, sozinhas, erro visual.

Se cada mudança passar, medir também sua combinação: não somar percentuais de
estágios diferentes para anunciar FPS. Publicar tempo total, FPS, pior quadro,
variação entre repetições e custo de memória, além dos subticks por estágio.

## O que ficou fora desta rodada

DMA parcial, stride 4, quantização de descritores, culling pelo subsector do
centro, pre-cull por distância e flags genéricas do GCC já têm resultados
negativos no LOG. O PVS e os scalers de paredes já foram implantados.

Pular a base escondida pelas molduras de janelas continua sendo uma ideia
plausível, mas seu teto precisa ser medido novamente: as janelas agora são
limitadas a 64 unidades e o pack usa scalers. O perfil recente das fases
pesadas dá prioridade a sprites/projeção. Não iniciar essa terceira frente
com os números anteriores às mudanças como justificativa.

Registrar em LOG.md os resultados das experiências, inclusive os abandonos.
Este plano não adiciona uma regra permanente a AGENTS.md.

## Complemento — refatoração do BSP e do cast

A leitura específica do BSP mostrou uma terceira experiência justificável.
Em E1M7 (96,1232), a137, cast custa historicamente 8580 subticks: cerca de
6,7 dos 23,73 vb do quadro. Há 97 segmentos testados, 13 desenhados e 26
chamadas de projeção de caixas. Esses números tornam o cast relevante nessa
vista, mas não dizem em qual rejeição os outros 84 segmentos saíram.

### Pré-requisito: reparar a atribuição de custo do PVS

`CADENCE_DRAWSEG_SPLIT` mede o total de `draw_seg` dentro de `bsp_visit_leaf`.
O caminho principal, `bsp_run_vis_program`, chama `bsp_draw_seg` e
`bsp_draw_seg_facing` diretamente, fora desse temporizador. Analogamente,
`CADENCE_TRAVERSE_SPLIT` envolve a projeção das caixas em
`bsp_render_boxed_child`, mas não a projeção dos GROUPs do programa.

O relatório existente `out/sweep/drawseg-e1m7split-x96y1232a137.json`
confirma a lacuna: 97 segmentos testados, `drawseg total = 0`,
`sample loop = 4248` e custo fixo calculado negativo. Não interpretar isso
como custo de segmento nulo ou como prova de travessia dominante.

Instrumentar os dois caminhos sem contagem dupla: programa, GROUP/BRANCH,
transformações, clipping/projeção, preparação de interpolação e amostras.
Contar também quantos frames entram no fallback e quanto ele custa.
Provar que o temporizador de segmentos é exercitado num frame atendido
inteiramente pelo programa; registrar contagens e overhead da instrumentação.

### Primeira refatoração: decidir visibilidade antes da interpolação

Em `bsp_render_columns.c::draw_seg`, após projetar/ordenar os endpoints,
o código calcula `invzL/R`, `uzL/R`, `inv_span` e metadados de textura antes
de limitar o intervalo à tela e procurar a primeira amostra aberta.

Mover o clipping horizontal, o cálculo de `first_sample/last_sample` e
`bsp_find_next_open(first_sample)` para antes dessa preparação. Retornar
imediatamente se não houver amostra aberta no intervalo, e reutilizar a
amostra encontrada para iniciar o laço. Manter as mesmas fórmulas e a mesma
ordem de desenho. Confirmar no código compilado quais operações hoje ainda
acontecem antes da rejeição e quanto a mudança efetivamente remove.

Uma segunda antecipação, medida separadamente: no caminho de overlay,
após obter `depth_col`, testar se já existe overlay mais próximo antes de
calcular U, altura e escala de textura para uma amostra que será descartada.
Não confundir essa possibilidade com eliminar o desenho do fundo das janelas.

Contadores de saída precisam distinguir: porta/trigger aberto, backface,
atrás do near plane, fora da tela, intervalo sem amostras, intervalo já fechado
e overlay preterido. Se quase todos os rejeitados já saírem antes da preparação,
abandonar a hipótese de ganho grande por essa refatoração.

**Verificação adicional obrigatória:** o `BSP_VIS_ORACLE` existente usa o mesmo
`draw_seg` nos dois lados. Portanto, ele sozinho não verifica uma mudança
nesse núcleo compartilhado. Preservar a implementação anterior como referência
em build de teste, comparar os campos relevantes de `RayColumn` no mesmo
estado, verificar descoberta do automapa e aplicar um controle negativo só
ao caminho novo. Executar também o oracle do PVS, com seu controle negativo,
e exigir conclusão das rotas.

### Reformulação maior, somente se a atribuição justificar

O programa atual é por folha e cobre todas as direções. Uma extensão possível
é especializá-lo por folha e faixa de ângulos para remover offline segmentos
que nunca podem entrar no campo de visão daquela combinação. Isso usa a direção
para escolher uma lista conservadora; não arredonda o ângulo usado para desenhar.

Antes de alterar o runtime, fazer um estudo offline de quatro ou oito faixas:
quantos SEG/GROUP realmente executados seriam evitados e quantos bytes seriam
necessários, com deduplicação das listas. Provar a exclusão sobre toda a célula,
todos os ângulos da faixa e os viewports suportados, respeitando trigonometria,
near clip e arredondamento do renderer. Amostrar apenas o centro da folha ou
o centro da faixa não é uma prova. Na dúvida, manter o segmento.

Preservar BRANCH, ordem frente-trás, portas/janelas e o fallback atual.
Medir o tamanho por pack: a folga residente não aumenta automaticamente a
janela de 1,5 MB. O oracle independente da travessia é apropriado para verificar
essa alteração do bake. A aprovação depende do ganho líquido, incluindo seleção
de lista, fallback e crescimento de ROM. Trata-se de pesquisa, não ganho previsto.

Rebalancear a árvore original ou fundir seus segmentos fica fora da primeira
tentativa: a travessia principal já foi substituída pelo programa PVS, e mudar
os cortes também pode mudar arredondamento da textura, IDs e consumidores de
geometria. Remover recursão por si só não fornece um teto de ganho demonstrado.

Prioridade para BSP: corrigir medição → antecipar rejeições → só então avaliar
especialização direcional. A meta de 1 vb exige cerca de 15% de redução do cast
na vista pesada de E1M7; isso representaria aproximadamente 4–5% de FPS total,
mantidos os outros custos. Nenhuma dessas economias foi medida nesta análise.
