# Falua

Sintetizador em plugin (VST3, AU e app standalone) que roda os motores de som do firmware **JIANT** para o M-VAVE FM-1, um fork do **Felucca** de Leo Kuroshita (Hügelton Instruments).

O código de som do firmware é compilado **sem modificações** para o computador. O Falua só acrescenta uma camada fina em volta dele: entrada de MIDI do DAW, parâmetros automatizáveis, presets e conversão de taxa de amostragem.

> Projeto não oficial. Não é afiliado à M-VAVE, ao FM-1, ao Felucca nem ao JIANT. "Falua" é um nome provisório.

## Estado (v0.1)

| | |
|---|---|
| Motor no plugin | **ANALOG** (15 presets de fábrica) |
| Motores já funcionando no núcleo | ANALOG, FM6, PHASE, LOFI, VOICE, WHEEL, NOISE, DRUM (todos os presets testados) |
| Formatos | VST3 (Windows, macOS, Linux), AU (macOS), Standalone |
| Parâmetros | 8 knobs do motor, envelope, LFO, modo de voz, glide, distorção, envios de chorus, delay e reverb, pan, nível, delay e reverb globais |
| MIDI | notas, velocity, pitch bend, sustain, mod wheel e os CCs do FM-1 (71 a 75, 74 brilho etc.) |
| Taxa de amostragem | qualquer uma (o núcleo roda a 44,1 kHz e o plugin converte) |

### Limitações conhecidas

- **Uma instância por vez.** O firmware guarda o estado em variáveis globais. A segunda instância aberta fica muda e avisa na janela. Resolver isso está no roteiro.
- Todos os canais MIDI tocam a parte 1.
- A interface é a genérica do JUCE (sliders). Uma interface própria vem depois.
- Sem sequenciador: quem sequencia é o DAW.

## Roteiro

1. Seletor de motor com os 8 motores (o núcleo já suporta).
2. Interface própria inspirada no FM-1.
3. Multitimbral: 6 partes nos canais MIDI 1 a 6, como no aparelho.
4. Várias instâncias no mesmo DAW.
5. Importar patches `.syx` do DX7 no motor FM6.

## Como compilar

Você precisa de **CMake 3.22+** e **Git**. O JUCE 8 é baixado sozinho na primeira configuração.

### Windows

1. Instale o **Visual Studio 2022** (Community serve) com a carga de trabalho *Desenvolvimento para desktop com C++* e o componente **C++ Clang Compiler for Windows**. O firmware é escrito em C do GCC, e por isso o núcleo precisa do clang.
2. No terminal, na pasta do projeto:

```
cmake -B build -G "Visual Studio 17 2022" -A x64 -T ClangCL
cmake --build build --config Release
```

3. O plugin fica em `build\Falua_artefacts\Release\VST3\Falua.vst3`. Copie a pasta inteira para `C:\Program Files\Common Files\VST3\` e peça para o DAW procurar plugins novos.

### macOS

```
cmake -B build -G Xcode
cmake --build build --config Release
```

VST3 em `~/Library/Audio/Plug-Ins/VST3/`, AU em `~/Library/Audio/Plug-Ins/Components/`.

### Linux

```
sudo apt install ninja-build libasound2-dev libx11-dev libxext-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxcomposite-dev libfreetype-dev libfontconfig1-dev
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Sem compilar nada: GitHub Actions

Ao enviar o projeto para um repositório no GitHub, o workflow em `.github/workflows/build.yml` compila Windows, macOS e Linux sozinho. Os plugins aparecem na aba **Actions**, em *Artifacts*. Ao criar uma tag `v0.1.0`, eles são publicados numa Release.

## Testes

- `core_test`: toca frases em todos os motores e presets usando só a API C, confere que há som, sem valores inválidos e sem clipping forte, e grava WAVs.
- `host_test` (`-DFALUA_HOST_TEST=ON`): abre o `Falua.vst3` compilado como um DAW faria, toca em 44,1, 48, 88,2 e 96 kHz com blocos de tamanhos variados, troca presets, salva e restaura o estado e confere que uma segunda instância fica muda.

## Estrutura

| Pasta | O que tem |
|---|---|
| `core/` | `jiant_core.c/.h`: a API C em volta do DSP do firmware |
| `plugin/` | o plugin JUCE (processador e editor) |
| `third_party/jiant/` | cópia das fontes do firmware e das tabelas geradas, com as licenças originais |
| `tests/` | `core_test.c` e `host_test.cpp` |
| `tools/update_jiant.sh` | atualiza `third_party/jiant` a partir do repositório do JIANT |

## Licença

GPL-3.0-only, a mesma do firmware. Se você distribuir binários, distribua também o código-fonte completo. Detalhes e créditos em [NOTICE.md](NOTICE.md).
