# Let's Go Eevee — Port Android

Port não-oficial de **Pokémon Let's Go, Eevee!** (Nintendo Switch) para Android, baseado no
[letsgo-recomp](https://github.com/raulzrrs/letsgo-recomp) (recompilação estática, por raulzrrs).
Testado no Galaxy S23 (Snapdragon 8 Gen 2, Adreno 740, Android 16).

> **Uso pessoal. Este repositório NÃO contém o jogo, nem código do jogo, nem chaves.**
> Para buildar você precisa do seu próprio dump legalmente obtido (ver [BUILD.md](BUILD.md)).
>
> 📺 Canal da autora no YouTube: https://youtube.com/@emuucheck

## O que este repositório contém

| Pasta/arquivo | Conteúdo |
|---|---|
| `src/` | Código-fonte completo: `runtime/` (NVN→Vulkan, HLE, SDL3), `tools/` (geração: `gen.py`, `lifter.py` e mais 22 scripts), `Makefile`, `third_party/` (imgui, smaa, vma, fontes), `docs/` (docs upstream) |
| `apk/` | App Android: launcher, importador in-app, overlay de controles, manifest |
| `importer/` | Importador de NSP no aparelho (lê o dump do usuário e extrai `exefs`/`romfs`) |
| scripts na raiz | Scripts de build (`build-runtime.sh`, `link-runtime.sh`, `build-apk.sh`, etc.) |
| `docs/` | Documentação do projeto: auditorias, relatórios de builds, HOWTO original |
| `screenshots/` | Capturas do jogo rodando no S23 (estado atual) |
| `BUILD.md` | **Tutorial completo de build**: o que você precisa, passo a passo |
| `ISSUES.md` | **Problemas conhecidos**: o que funciona, o que não funciona, e o que já foi investigado |
| `RELATORIO-ATUALIZADO.md` | Relatório detalhado do estado do projeto (v0090) |

## O que este repositório NÃO contém (de propósito)

- ❌ O código recompilado do jogo (`generated-aarch64/`) — derivado do binário da Nintendo
- ❌ O NSP, `prod.keys`, `title.keys` ou qualquer dump — use o seu próprio
- ❌ Assets do jogo (texturas, modelos, fontes, áudio)
- ❌ APKs prontos — eles embutem o jogo recompilado e não podem ser distribuídos

Sem o código gerado e o dump, o projeto **não compila sozinho**. O [BUILD.md](BUILD.md)
explica como gerar tudo a partir do seu próprio dump.

## Estado atual (v0090)

**Funciona:** boot, mundo 3D, mapa, batalhas, menus, save, touch e controle físico,
importador de NSP, resolução escalável (480p–1080p), ~22–30 fps.

**Não funciona:** **o texto é invisível** em menus, diálogos e HUD. É o problema principal
e está documentado em detalhes em [ISSUES.md](ISSUES.md) — incluindo tudo o que já foi
eliminado por prova (cull, depth, write_mask, scissor, matriz de projeção, atributo `a0`,
janela de cópia de memória) e o teste decisivo atual (v0090, VS_RANGE_DIAG).

### Screenshots — estado atual (Galaxy S23, v0090)

O mundo 3D, as batalhas e os menus renderizam — mas **todo texto dinâmico está vazio**.
As caixas de diálogo, menus e listas de itens abrem em branco. (As imagens abaixo são
capturas do jogo rodando no aparelho, apenas para demonstrar o estado do port.)

| | |
|---|---|
| ![Mundo e diálogo](screenshots/01-mundo-casa-dialogo.jpg) | ![Diálogo vazio](screenshots/02-dialogo-vazio.jpg) |
| Mundo renderizando (20 fps) com caixa de diálogo aberta | Caixa de diálogo **vazia** — o texto não aparece |
| ![Menu vazio](screenshots/03-menu-vazio.jpg) | ![Tela título](screenshots/04-tela-titulo.jpg) |
| Menu do jogo com barras **vazias** (sem texto) | Tela-título funciona (texto da logo é textura, não texto dinâmico) |
| ![Caminho da floresta](screenshots/05-caminho-floresta.jpg) | ![Batalha](screenshots/06-batalha.jpg) |
| Viridian Forest: caminho e vegetação renderizando | Batalha funcionando (30 fps) |
| ![Floresta e sombras](screenshots/07-floresta-sombras.jpg) | ![Loja com itens vazios](screenshots/08-loja-itens-vazios.jpg) |
| Sombras blocadas na floresta (15 fps — ver ISSUES.md §4) | Loja: prateleiras e lista de itens com textos **vazios** |
| ![Centro Pokémon](screenshots/09-centro-pokemon.jpg) | |
| Interior do Centro Pokémon renderizando (30 fps) | |

### Novas capturas — texto renderizando

Capturas novas: o texto aparece nas telas do jogo — seleção de idioma,
tela de equipe e intro do Prof. Carvalho.

| | |
|---|---|
| ![Telhado da cidade à noite](screenshots/10-telhado-cidade-noite.jpg) | ![Equipe e EXP](screenshots/11-equipe-exp.jpg) |
| Mundo à noite renderizando | Tela de equipe com nomes, níveis e "Your Pokémon got 129 Exp. Points!" |
| ![Seleção de idioma em português](screenshots/12-selecao-idioma-portugues.jpg) | ![Seleção de idioma em espanhol](screenshots/13-selecao-idioma-espanhol.jpg) |
| Seleção de idioma em português, com texto | Seleção de idioma em espanhol, com texto |
| ![Intro do Prof. Carvalho em francês](screenshots/14-intro-professor-frances.jpg) | |
| Intro do Prof. Carvalho em francês, com texto (teste do contribuidor ptichalouf, Galaxy Z Fold7) | |

### Vídeos

- [Pokémon let's go Eevee android Port Giroscópio test](https://youtu.be/Wngih_skzco) — teste de giroscópio, com suporte a multi-idioma
- [Pokémon let's go Eevee Android Port (PROBLEMAS RESOLVIDOS)](https://youtu.be/ZyWnYdSipb8) — textos e FPS/Turnip resolvidos com as PRs do ptichalouf

## Build rápido

```bash
# 1. Gere o código do jogo a partir do SEU dump (ver BUILD.md)
cd src && make gen ARCH=aarch64

# 2. Compile o runtime, o importador e linke
./build-runtime.sh    # espera SMOKE_OK
./build-importer.sh
./link-runtime.sh     # espera RUNTIME_LINK_OK

# 3. Monte e assine o APK
./build-apk.sh        # APK sai na raiz
```

Detalhes, pré-requisitos e armadilhas: **[BUILD.md](BUILD.md)**.

## Mods / Mods multiples

**Português.** Extraia os mods com um app como o 7-Zip e coloque cada pasta em
`<pasta do jogo>/mods/<nome>/romfs/...`. Vários mods podem ficar ativos ao mesmo
tempo (até 8). Quando dois mods fornecem o mesmo arquivo, o primeiro da lista
vence: `mod = Traducao, Randomizer` dá prioridade a `Traducao`.

No launcher Android, a prioridade segue a ordem em que as caixas são marcadas,
não a ordem alfabética das caixas. A ajuda mostra a lista **Ordem ativa**, na mesma
ordem gravada na linha `mod` de `larecomp.ini`. Desmarcar e remarcar um mod o move
para o fim dessa lista. Só arquivos RomFS são substituídos; patches de código
(`exefs`, `.ips`) não são aplicados.

**Français.** Extrayez les mods avec une application comme 7-Zip et placez chaque
dossier dans `<dossier du jeu>/mods/<nom>/romfs/...`. Plusieurs mods peuvent être
actifs en même temps (jusqu'à 8). Si deux mods fournissent le même fichier, le
premier de la liste l'emporte : `mod = Traduction, Randomizer` donne la priorité
à `Traduction`.

Dans le lanceur Android, la priorité suit l'ordre dans lequel les cases sont
cochées, pas leur ordre alphabétique. L'aide affiche la liste **Ordre actif**, dans
l'ordre exact enregistré sur la ligne `mod` de `larecomp.ini`. Décocher puis
recocher un mod le déplace à la fin de cette liste. Seuls les fichiers RomFS sont
remplacés ; les correctifs de code (`exefs`, `.ips`) ne sont pas appliqués.

## Créditos

- [raulzrrs / letsgo-recomp](https://github.com/raulzrrs/letsgo-recomp) — recompilação estática original
- [peiikai / LASRecomp](https://github.com/peiikai/LASRecomp) — base do runtime (ver `LICENSE-LASRecomp`)
- [Nycolazs / LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus) — referência de port Android
- [ptichalouf](https://github.com/ptichalouf) — 5 PRs que salvaram o projeto: idioma do jogo, link do runtime (DT_NEEDED), Turnip realmente ativo, tela inicial com o estado da instalação e o texto invisível (emissão 32 bits sem shaderInt64)
