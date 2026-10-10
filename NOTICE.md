# Avisos e créditos

O Falua é software livre sob a **GNU General Public License, versão 3 apenas** (`GPL-3.0-only`, texto em `LICENSE`).

## Código de terceiros incluído

| O que | Autor | Licença | Onde |
|---|---|---|---|
| Firmware JIANT (motores, vozes, efeitos, tratamento de MIDI) | juanjiant-bit, fork do Felucca | GPL-3.0-only | `third_party/jiant/firmware/` |
| Felucca, base do JIANT | Leo Kuroshita (@kurogedelic), Hügelton Instruments | GPL-3.0-only | idem |
| Motor PHASE (porte do oscilador do CrispyZebra) | Hügelton Instruments | GPL-3.0 | `eng_phase.c` |
| Motor FM6 (msfa do Dexed) | Google Inc. e Pascal Gauthier | Apache-2.0 | `fm6_core.c` |
| Demais componentes listados pelo firmware | ver `third_party/jiant/LICENSING.md` | ver `third_party/jiant/LICENSES/` | |
| JUCE 8 (baixado na compilação, não incluído aqui) | Raw Material Software | AGPLv3 (ou licença comercial) | |
| VST3 SDK 3.7.12 (dentro do JUCE 8.0.4) | Steinberg Media Technologies | GPLv3 ou licença proprietária da Steinberg; o Falua usa a GPLv3 | |

A versão exata do firmware usada está em `third_party/jiant/UPSTREAM_COMMIT.txt`. As tabelas em `third_party/jiant/generated/` foram geradas pelos scripts do próprio firmware (`tools/gen_tables.py` e `tools/gen_fm6_patches.py`).

### Sobre o JUCE e a AGPL

O JUCE 8 é oferecido gratuitamente sob a AGPLv3. A GPLv3 (seção 13) permite combinar código GPLv3 com código AGPLv3. O plugin distribuído continua livre, e o código-fonte completo deve acompanhar os binários. Confira os termos atuais do JUCE antes de publicar.

## Modificações

O Falua **não altera** nenhum arquivo do firmware. O que foi acrescentado:

- `core/jiant_core.c`: compila as fontes de som do firmware para desktop, do mesmo jeito que os testes do próprio firmware (`tests/hostsim.c`), e expõe uma API C (MIDI, parâmetros, presets, render em float).
- `plugin/`: o plugin JUCE.
- `tests/`, `tools/`, `.github/`: testes, script de atualização e compilação automática.

## Marcas

M-VAVE e FM-1 são marcas de seus respectivos donos. VST é marca registrada da Steinberg Media Technologies GmbH. O Falua não é afiliado nem endossado por eles, nem pelos autores do Felucca ou do JIANT.
