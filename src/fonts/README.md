# Fontes do sistema (opcional)

Parte dos textos do jogo usa a fonte embutida no sistema do Switch, que não faz parte dos arquivos do jogo. Sem ela,
o Let's Go Recomp usa a Yu Gothic do Windows (o mesmo estilo de gótico japonês da Morisawa UD Shin Go do Switch) e,
na falta dela, a Arial. A maior parte do jogo desenha os textos com as fontes do próprio romfs e não muda.

Para o visual exato, coloque aqui uma cópia tirada do **seu** console (arquivos de sistema *FontStandard*,
título 0100000000000811, e *FontNintendoExtension*, 0100000000000810):

- `nintendo_udsg-r_std_003.bfttf`: a fonte principal (um `.ttf` com o mesmo nome, ou escrito `nintendo_udsgr_std_003`,
  também serve)
- `nintendo_ext_003.bfttf`: os símbolos dos botões (opcional)

`LARECOMP_FONT=<arquivo>` passa por cima de tudo (.ttf / .otf / .ttc / .bfttf).

Essas fontes são propriedade da Nintendo: ficam fora do git (`.gitignore`) e nunca devem ser compartilhadas.
