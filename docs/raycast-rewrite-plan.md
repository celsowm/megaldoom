# Plano de reescrita do raycaster SGDK

Este plano abandona a implementacao atual e trata o proximo renderer como uma reescrita limpa. O codigo bugado pode continuar no repositorio ate a troca final, mas nao deve ser usado como base de copia para o novo motor.

Objetivo: construir um raycaster para Mega Drive/Genesis usando SGDK moderno, com foco em estabilidade, previsibilidade e custo baixo por frame. A prioridade e validar cada bloco visualmente em ROM real de teste antes de juntar tudo.

## Regras da reescrita

- Nao reaproveitar `src/main.c`, renderer BMP atual, DDA atual, tabelas atuais ou estruturas atuais como base de implementacao.
- Nao mirar uma imitacao do Doom NeoGeo. A referencia tecnica passa a ser: raycaster proprio, adequado ao VDP do Mega Drive.
- Evitar o `BMP` software engine como renderer final. A nova rota deve renderizar para tiles/planos do VDP e usar DMA de forma controlada.
- Sem `float` em runtime. Toda camera, trigonometria, distancia e projecao devem usar fixed-point.
- Zero warnings no build antes de qualquer validacao humana.
- Cada etapa deve produzir uma ROM testavel e uma captura esperada simples.
- Testar no minimo em BlastEm e ares antes de marcar uma etapa como aceita.
- Se uma etapa falhar visualmente, nao avancar para a proxima.

## Pasta externa nao versionada

Criar uma pasta local nao versionada chamada `.extenals/` na raiz do projeto, conforme solicitado. Ela deve guardar repositorios e ferramentas auxiliares que nao entram no Git deste projeto.

Estrutura planejada:

```text
.extenals/
  freedoom/
  tools/
  cache/
```

Primeira tarefa dessa etapa:

- Adicionar `.extenals/` ao `.gitignore`.
- Criar script de sincronizacao, por exemplo `tools/sync-externals.ps1`.
- Clonar `https://github.com/freedoom/freedoom` dentro de `.extenals/freedoom`.
- Registrar commit, licenca e data usada em `docs/external-assets.md`.
- Nunca depender do estado flutuante de `master` sem registrar o commit usado.

Uso esperado do Freedoom:

- Fonte de estudo e extracao de recursos livres: texturas, sprites, flats, sons e mapas.
- Nao usar Freedoom como engine. O repositorio fornece dados/conteudo; o motor continua sendo nosso.
- Converter recursos para formatos pequenos e proprios do Mega Drive antes de entrar em `res/`.

## Etapa 0: congelar o estado quebrado

Entrega:

- Criar um ponto de referencia no Git antes da reescrita, sem apagar o codigo atual.
- Documentar que `src/main.c` atual esta fora da nova base.

Validacao humana:

- Confirmar que vamos prosseguir com reescrita limpa e nao com mais patches no renderer atual.

## Etapa 1: esqueleto SGDK limpo

Entrega:

- Novo ponto de entrada minimo.
- Inicializacao VDP, CRAM, joypad e loop principal.
- Tela preta, contador de frame opcional e cor de fundo fixa.
- Nenhum raycast ainda.

Criterio de aceite:

- Compila sem warnings.
- Roda por varios minutos sem flicker, lixo grafico ou corrupcao.
- BlastEm e ares mostram a mesma tela.

## Etapa 2: renderer de tiles dinamicos

Entrega:

- Renderer novo baseado em tiles/planos do VDP.
- Um buffer pequeno em RAM para montar tiles de viewport.
- Upload por DMA em uma area fixa de VRAM.
- Padrao visual deterministico: barras verticais, horizontais e xadrez.

Criterio de aceite:

- Padrao estavel por 60 segundos.
- Sem ruido periodico a cada segundo.
- Sem uso do renderer BMP antigo.

## Etapa 3: janela 3D sem raycast

Entrega:

- Viewport 3D com ceu, chao e barras de parede sinteticas.
- Controle de largura de coluna: comecar com 4 px por coluna, depois testar 2 px.
- HUD separado em plano/tilemap diferente ou completamente desligado.

Criterio de aceite:

- A tela permanece limpa.
- O custo de frame fica previsivel.
- O tamanho da viewport e aprovado antes de colocar DDA.

## Etapa 4: matematica fixed-point isolada

Entrega:

- Novo modulo de fixed-point.
- Tabelas de seno/cosseno geradas de forma reproduzivel.
- Tabelas de direcao dos raios e correcao de fish-eye precomputadas.
- Teste visual simples: girar um vetor/bussola sem raycast.

Criterio de aceite:

- Movimento angular suave.
- Sem overflow conhecido nos limites de angulo.
- Build limpo.

## Etapa 5: DDA untextured

Entrega:

- Novo mapa minimo, por exemplo 8x8.
- Raycast DDA so com paredes solidas.
- Render de paredes com cor plana por lado atingido.
- Sem textura, sem sprites, sem portas, sem HUD.

Criterio de aceite:

- Cena correta parado.
- Rotacao correta.
- Movimento ainda pode estar desligado.
- Nenhum ruido visual em BlastEm ou ares.

## Etapa 6: movimento e colisao

Entrega:

- Player com posicao fixed-point.
- Rotacao, frente/tras e strafe.
- Colisao simples por raio/capsula pequena contra grid.

Criterio de aceite:

- Jogador nao atravessa parede.
- Movimento nao causa corrupcao visual.
- Rotacao continua estavel.

## Etapa 7: textura de parede minima

Entrega:

- Texturas 16x16 ou 32x32 em indices de paleta.
- Calculo de `tex_x` e amostragem vertical por tabela.
- Shading simples por lado/distancia usando paleta, nao multiplicacao cara por pixel.

Criterio de aceite:

- Textura nao "rasga" ao virar.
- Coordenadas de textura ficam estaveis em cantos.
- Sem listras aleatorias.

## Etapa 8: pipeline de assets

Entrega:

- Script para importar assets de `.extenals/freedoom`.
- Conversao para formatos intermediarios pequenos: PNG indexado, JSON de metadados ou binarios proprios.
- Geracao final para `res/` via SGDK resource compiler ou binarios carregaveis.

Criterio de aceite:

- Um asset do Freedoom convertido aparece corretamente na ROM.
- O asset gerado em `res/` e versionado; o clone em `.extenals/` nao.
- Licenca e origem documentadas.

## Etapa 9: portas e interacao

Entrega:

- Tipo de celula de porta simples.
- Estado fechado/abrindo/aberto/fechando.
- Raycast reconhece segmento de porta sem quebrar parede normal.

Criterio de aceite:

- Abrir e fechar porta nao altera estabilidade do renderer.
- Porta nao cria buraco incorreto no mapa.

## Etapa 10: sprites e arma

Entrega:

- Primeiro sprite como hardware sprite do VDP, nao desenhado dentro do raycast.
- Ordenacao simples por profundidade.
- Arma/HUD tambem separado do buffer 3D.

Criterio de aceite:

- Sprite oculta atras de parede conforme esperado.
- Nao ha corrupcao ao mover camera.

## Etapa 11: otimizacao dirigida por medicao

Entrega:

- Contadores simples de tempo/frame.
- Comparar 64 colunas de 4 px, 80 colunas de 2 px e viewport reduzida.
- Medir custo de DMA e custo de DDA separadamente.

Criterio de aceite:

- Escolher configuracao final baseada em estabilidade e frame time, nao em chute.
- Manter 60 fps como alvo; aceitar reducao de detalhe antes de aceitar instabilidade.

## Etapa 12: substituicao do prototipo antigo

Entrega:

- Trocar o build principal para a nova base.
- Remover ou arquivar codigo antigo em uma etapa separada.
- Atualizar README com os controles e estado real.

Criterio de aceite:

- Build limpo.
- ROM principal abre direto no novo renderer.
- O bug antigo nao aparece em teste prolongado no BlastEm e no ares.

## Fluxo de validacao por etapa

Para cada etapa:

1. Eu implemento apenas a etapa atual.
2. Eu rodo build local e elimino warnings.
3. Eu te passo o comando exato para rodar.
4. Voce valida visualmente e responde `ok` ou descreve o defeito.
5. So depois disso a proxima etapa comeca.

## Comandos previstos

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\tools\build-windows.ps1
.\tools\run-windows.ps1 -Emulator BlastEm
.\tools\run-windows.ps1 -Emulator ares
```

Depois da etapa da pasta externa:

```powershell
.\tools\sync-externals.ps1
```

## Decisoes pendentes para validar antes de codar

- Manter o nome solicitado `.extenals/` ou corrigir para `.externals/`.
- Viewport inicial: 64 colunas de 4 px ou 80 colunas de 2 px.
- Resolver se o primeiro renderer dinamico usa BG_A inteiro ou viewport em uma area fixa do plano.
- Definir se a primeira meta visual e parede plana ou textura 16x16.
