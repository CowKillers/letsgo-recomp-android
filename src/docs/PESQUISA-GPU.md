# Pesquisa: onde a GPU gasta o quadro do mundo

_17-18/10/2026. Eevee 1.0.0, RTX 3080, rota da Rota 1 (`build/audit-save-20261017`, mundo parado, ~600 draws por quadro),
resolução interna 1920x1080. **Resolvido**: a GPU lia os vértices pelo PCIe. Correção e números abaixo; o que ainda está
aberto fica no fim._

## Resumo

- O jogo desenhava o mundo com ~9-10 ms de GPU por quadro, a maior parte em **4 draws** (instanciados, vegetação) que
  quase não geram imagem.
- A causa não era o shader nem os pixels: os vértices e os índices eram lidos **direto da memória do jogo importada na GPU**
  (memória do sistema), pelo PCIe, a cada invocação de vertex shader. Cada instância tem 8 streams de 16 bytes, relidos para
  cada um dos ~71 mil vértices do draw: ~9 MB por draw, ~0,8 ms a ~11 GB/s.
- Em placas discretas os vértices e os índices agora vão para a VRAM (staging em memória do sistema + cópia no começo de cada
  submissão). Faixas que o próprio jogo escreve por compute continuam sendo lidas no lugar. GPU: **10,2 -> 1,2 ms por quadro
  (-88%)**; fps sem limite no mundo **~75 -> ~90 (+19% na mediana)**, porque a CPU paga ~1,8 ms pelas cópias.
- Em placas integradas (memória do sistema é a da GPU) o comportamento antigo foi mantido.

## Aviso sobre o método

**O tempo de GPU do mesmo quadro varia de ~10 a ~18 ms de uma execução para outra com o mesmo código.** Comparar duas
execuções separadas só serve para efeitos grandes (2x ou mais). Várias conclusões iniciais desta pesquisa ("pilhas em
vetor: -11%", "achatar a máquina de estados: -12%") eram ruído e foram desmentidas por testes intercalados. Para efeitos
menores, alterne as versões **dentro da mesma execução** (`LARECOMP_EXP_ALT=1`: versão alterada só nos quadros ímpares,
perfil de 70 quadros seguidos, comparar as medianas de pares e ímpares) ou intercale várias execuções A/B e use só quadros
de mundo (cabeçalho do `[gpuprof]` com 6 ms ou mais). A imagem também não é idêntica entre duas execuções do mesmo modo
(~34 dB de PSNR: os Pokémon andam), então "igual" só se mede contra esse ruído. Ferramentas: [DEPURACAO.md](DEPURACAO.md).

## Como se chegou à causa

1. **O quadro de GPU é uma passada só.** `LARECOMP_GPUPROF_AT`: passe principal HDR 1920x1080 com 316 draws = 84% do tempo;
   mapa de sombras 2048x2048 = 1,6 ms; todo o pós-processamento junto, menos de 1 ms.
2. **Um grupo de 4 draws custa ~4,9 ms** (um terço da GPU): mesmo par de shaders, 1.404 índices x 180 instâncias cada. Na
   mesma execução, quadros pares com e ímpares sem eles: mediana 14,58 contra 9,70 ms.
3. **O custo é de vértice, não de pixel.** Scissor de 1x1: -0,27 ms (nada). `LARECOMP_GPUPROF_STATS`: 252.720 vértices de
   entrada, ~71.176 invocações de vertex shader, 0 fragmentos e 0 primitivas no recorte em 5 de 6 amostras.
4. **Não é a matemática do shader.** Um vertex shader que só lê os 11 atributos e soma tudo custa o mesmo que o completo
   (0,80 contra 0,81 ms por draw); sem ler os atributos (retorno no início de `main`) cai para 0,09 ms. O driver
   (`LARECOMP_PIPE_STATS`, `VK_KHR_pipeline_executable_properties`) informa 48 registradores, 10 KB de código e nenhum
   spill: o shader compilado é pequeno e eficiente.
5. **O código tinha o motivo**: `guest_direct` liga o heap importado do jogo como buffer de vértices e de índices, sem cópia
   ("lido no lugar", como no Switch). O heap importado é memória do sistema. O desenho dos atributos desse programa: 1 stream
   por vértice (24 bytes) e 8 streams por instância (`stride 16`, um `vec4` cada).
6. **Teste decisivo**: `LARECOMP_VK_COPY_VERTICES=1` (cópias para o anel): o mesmo draw cai de 0,8 para 0,01 ms e o quadro de
   GPU de 9,2 para ~1 ms, com a mesma imagem.

## O que foi descartado

| Hipótese | Teste | Resultado |
|---|---|---|
| Filtro anisotrópico 16x | `LARECOMP_ANISO=1` | custo por índice igual (4,8 contra 4,5 ns), entre execuções |
| Preenchimento de pixels | scissor de 1x1, mesma execução | sem efeito |
| Leitura do anel de memória pelo PCIe | log do anel | o anel de constantes está em memória local da GPU; **os vértices é que não passavam por ele** |
| Pilhas `ssy/pbk/pcnt` em memória local | `uvec4` no lugar de `uint[16]`, A/B intercalado | mediana 12,39 contra 12,88 ms; alteração revertida |
| Máquina de estados `for + switch (pc)` | shader convertido à mão em blocos sem laço | 0,73 contra 0,83 ms por draw, dentro do ruído |
| `robustBufferAccess` | `LARECOMP_NO_ROBUST=1` | inconclusivo (deriva entre execuções) |
| ALU do shader | GLSL e estatísticas do driver | 427 instruções, sem laço, sem spill |

## A correção

`runtime/nvn/vulkan/exec.c`:

- **Dois anéis.** O anel 0 (como antes) tem as constantes e as fontes de upload de textura. O anel 1 tem os vértices e os
  índices dos draws: um chunk de staging em memória do sistema onde a CPU escreve, e um chunk em VRAM de onde a GPU lê; no
  começo de cada submissão (`ring_flush_gpu`) o que foi escrito desde a anterior é copiado pelo buffer de upload, que já roda
  antes do principal com uma barreira.
- **Faixas que a GPU escreve** (`gpuw`): este jogo usa compute para gerar dados de vértice (um compute escreve buffers de 16
  bytes que dois vertex programs leem como streams). Os storage buffers que um compute (ou um shader de vértice ou de
  fragmento com `STG`) tem ligados entram num conjunto de intervalos ordenados; vértices e índices dentro deles seguem sendo
  lidos no lugar, quando a GPU roda, e nunca copiados na hora do draw. Se um shader escreve em memória global **sem** nenhum
  storage buffer ligado, o runtime lê tudo no lugar dali em diante (aviso no log).
- **Política**: `LARECOMP_VERTEX_READ=direct|copy|auto` (padrão `auto`: cópia em placa discreta, no lugar em integrada).
- **Menos bytes**: cópia por endereço inicial (um draw que quer um prefixo menor reaproveita a cópia de outro) e, em streams
  por vértice, o buffer ligado inteiro (até 8 MB) na primeira vez: 37,3 MB por quadro viraram 16,2 MB. A varredura do maior
  índice usa AVX2.

## Resultado (Eevee, RTX 3080, mundo da Rota 1, execuções intercaladas)

| | direto | híbrido (padrão) |
|---|---:|---:|
| GPU por quadro | 10,2 ms | 1,2 ms |
| fps sem limite (mediana de 11-13 janelas) | 75,1 | 89,6 |
| CPU extra por quadro (cópias) | 0,04 ms | 1,8 ms |
| 60 fps pacado: média / P99 | 60,0 fps / 17,4 ms | 60,0 fps / 17,3-18,6 ms |

Com 2x de resolução interna (a GPU passa a pesar) o híbrido sustentou ~79,7 fps contra 70,6-76,0 no direto. O fps sem limite
sobe pouco numa 3080 porque a CPU é o limite; o ganho grande é para quem tem GPU mais fraca ou PCIe mais lento, onde esses
~9 ms de leitura pelo barramento viram o gargalo (a conta de 9 ms vale para esta máquina; não medi outra).

## O que foi tentado para baixar o custo de CPU e não ficou

- Staging em memória cacheada e varredura de índices vetorizada: sem efeito mensurável (o custo é o volume copiado).
- **Write watch** do Windows (`MEM_WRITE_WATCH`) para manter as malhas na VRAM entre quadros e só copiar o que o jogo
  escreveu: o jogo escreve só ~500-1.900 páginas por quadro (2-7 MB), mas `GetWriteWatch` sobre o heap inteiro custa ~2,3 ms
  por chamada, e por faixa ~2,3 us (~300 faixas por quadro: ~0,7 ms por submissão). Ganho pequeno para a complexidade e o
  risco; revertido.

## A cadeia entre as threads do jogo

- Só **3 threads trabalham** (ids 80, 81, 82, prioridade 44), cada uma a ~40% de CPU (total ~1,15 núcleo); as outras 7 ficam
  100% paradas numa condvar. Medido com `LARECOMP_THREAD_DUMP=16,0.25` e CPU por thread.
- Quando "rodam", estão em `nvnQueueSubmitCommands` ou `nvnQueuePresentTexture` (`LARECOMP_HLE_WHO` dá os nomes): o backend
  Vulkan executa a lista de comandos **dentro da chamada, na thread do jogo**.
- Com a GPU a ~1 ms o fps sem limite continua ~90 (ciclo de ~11 ms): **o limite agora é a cadeia de CPU entre as três
  threads**, que se alternam em vez de se sobrepor. É o próximo alvo.

## Próximos passos

1. **Cadeia de CPU entre as threads**: tirar `backend_execute` da thread do jogo (thread de renderização com fila) e achar
   por que as três se alternam. Teto estimado: o ciclo cai para o da thread mais longa.
2. **Cópias em paralelo** (workers) ou cache entre quadros com outra forma de detectar escrita: baixar os ~1,8 ms.
3. Medir em GPU mais fraca e em placa integrada (a política `auto` assume que integrada não ganha com cópias).
