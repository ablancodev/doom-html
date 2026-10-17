# Notas técnicas: DOOM 1993 → WebAssembly

Este documento explica cómo está hecho el port, qué problemas aparecieron y qué se aprende por el camino. Para la versión ligera, ver el [README](README.md).

## Índice

1. [La idea: no reescribir nada](#1-la-idea-no-reescribir-nada)
2. [Emscripten: de C a WebAssembly](#2-emscripten-de-c-a-webassembly)
3. [El bucle infinito en un navegador](#3-el-bucle-infinito-en-un-navegador)
4. [Vídeo](#4-vídeo)
5. [Entrada: teclado y ratón](#5-entrada-teclado-y-ratón)
6. [Sonido: el mezclador de id](#6-sonido-el-mezclador-de-id)
7. [Música: emulando una Sound Blaster](#7-música-emulando-una-sound-blaster)
8. [Archivos: WADs y partidas guardadas](#8-archivos-wads-y-partidas-guardadas)
9. [Cambios en el motor original](#9-cambios-en-el-motor-original)
10. [Cómo se verifica la fidelidad](#10-cómo-se-verifica-la-fidelidad)
11. [Bugs de 1997 encontrados por el camino](#11-bugs-de-1997-encontrados-por-el-camino)
12. [Lecciones aprendidas](#12-lecciones-aprendidas)

---

## 1. La idea: no reescribir nada

El código que liberó id Software (`linuxdoom-1.10`, 1997) está muy bien separado en dos mundos:

```
┌───────────────────────────────────────────────────────────┐
│  Motor portable (C puro)                                  │
│  r_*  render BSP     p_*  física, IA, mapas               │
│  g_*  lógica de juego  m_*  menús   s_sound  sonido lógico │
│  w_wad  lectura de WAD   z_zone  memoria   d_*  bucle      │
└──────────────────────────┬────────────────────────────────┘
                           │  interfaz i_*.h
┌──────────────────────────┴────────────────────────────────┐
│  Capa de plataforma (lo único que toca el sistema)         │
│  i_video  i_sound  i_system  i_net  i_main                 │
└───────────────────────────────────────────────────────────┘
```

Los archivos `i_*.c` originales hablaban con X11, con `/dev/dsp` y con sockets UDP de Linux. La estrategia fue:

- **Motor** (`src/engine/`): copiado tal cual del repo de id, con 5 cambios mínimos ([sección 9](#9-cambios-en-el-motor-original)).
- **Plataforma** (`src/web/`): reescrita desde cero para el navegador, conservando el código original donde tenía sentido. Por ejemplo, el mezclador de sonido es el de id.
- **Página** (`public/app.js`): el pegamento con las APIs del navegador (canvas, Web Audio, teclado, IndexedDB).

Toda la lógica del juego sigue en C. JavaScript solo mueve píxeles, muestras de audio, pulsaciones de teclas y archivos.

## 2. Emscripten: de C a WebAssembly

[Emscripten](https://emscripten.org/) es un compilador basado en LLVM/Clang que, en lugar de generar código máquina para x86 o ARM, genera **WebAssembly** (un bytecode que los navegadores ejecutan a velocidad casi nativa). También genera el JavaScript de soporte: una libc completa, un sistema de archivos virtual en memoria y el código para cargar el `.wasm`.

```sh
emcc -O2 -std=gnu89 ... src/engine/*.c src/web/*.c src/opl/opl3.c -o public/doom.js
```

El resultado son **370 KB de `.wasm`** y 75 KB de JavaScript, más pequeño que muchas imágenes de cabecera.

### Compilar C de 1993 con un compilador de 2026

Sorprendentemente, el motor compiló casi a la primera. Lo que hubo que tocar:

| Problema | Solución |
|----------|----------|
| C antiguo: funciones sin tipo de retorno (*implicit int*) y funciones usadas sin declarar | `-std=gnu89` y desactivar esos errores (`-Wno-implicit-int`, `-Wno-implicit-function-declaration`) |
| `strupr()` ya existe en la libc de Emscripten con otra firma | Renombrar la de id a `W_strupr` |
| `em_js.h` define `true` y `false` como macros, y rompe el `typedef enum {false, true} boolean` de DOOM | Incluir `<emscripten.h>` **después** de las cabeceras de DOOM |

### Una ventaja inesperada: 32 bits

El código de 1997 asume en varios sitios que **un puntero cabe en un `int`**. Por ejemplo, la tabla de configuración guarda punteros a cadenas en campos `int`. En los ports a 64 bits esto es una fuente de crashes y hay que arreglarlo con cuidado.

WebAssembly (`wasm32`) tiene punteros de 32 bits, así que **el código original funciona sin tocarlo**. DOOM se siente como en casa.

## 3. El bucle infinito en un navegador

Este es el problema central de cualquier port de un juego antiguo a la web. DOOM se estructura así:

```c
void D_DoomLoop (void)
{
    while (1)
    {
        I_StartFrame ();
        TryRunTics ();      // espera activa hasta que toca el siguiente tic
        S_UpdateSounds (...);
        D_Display ();       // dibuja (y a veces hace la transición "melt" con su propio bucle)
        ...
    }
}
```

Un navegador **no permite bloquear el hilo principal**. Mientras tu código se ejecuta no se pinta nada, no llegan eventos de teclado y no suena el audio. Un `while(1)` congela la pestaña.

Había dos opciones:

1. **Reestructurar el bucle** en una función que el navegador llame en cada fotograma (`emscripten_set_main_loop`). Es el enfoque clásico, pero obliga a reescribir `D_DoomLoop`, `TryRunTics` y el bucle del *melt*, que es justo el código que gobierna el ritmo del juego.
2. **Asyncify.** Emscripten reescribe el WebAssembly para que una función pueda **pausarse en mitad de su ejecución**: guarda la pila, devuelve el control al navegador y más tarde la restaura y continúa como si nada.

Elegí **Asyncify**, porque permite dejar el bucle original intacto. Solo hay que añadir una llamada en los dos sitios donde DOOM hace espera activa:

```c
// d_net.c, TryRunTics: esperar al siguiente tic
while (lowtic < gametic/ticdup + counts)
{
    I_Yield ();   // [WEB] → emscripten_sleep(1): el navegador respira
    NetUpdate ();
    ...
}
```

Y en `I_FinishUpdate` (cuando se ha terminado de dibujar un fotograma) se garantiza al menos una cesión por fotograma, por si el juego va atrasado y nunca llega a esperar.

### El coste de Asyncify y `ASYNCIFY_IGNORE_INDIRECT`

Asyncify tiene que instrumentar todas las funciones que *podrían* llegar a una pausa. Si hay llamadas mediante punteros a función, supone lo peor e instrumenta todo lo que hay encima. DOOM usa punteros a función en todas partes: el render (`colfunc`), los *thinkers* de los enemigos, los menús…

Con `-sASYNCIFY_IGNORE_INDIRECT=1` le decimos que las llamadas indirectas nunca pausan. Así solo se instrumenta la cadena real (`main → D_DoomMain → D_DoomLoop → TryRunTics/D_Display → I_Yield`), y el render y la física corren sin sobrecoste.

Esto obliga a que **ninguna** ruta indirecta llegue a una pausa. Hubo un caso: al salir del juego, `M_QuitResponse` (que el menú llama mediante un puntero a función) hace `I_WaitVBL(105)` para que suene el efecto de despedida antes de cerrar. La solución fue convertir esa espera en una **salida diferida**: `I_WaitVBL` apunta una hora límite, `I_Quit` se pospone y en el siguiente fotograma, pasado el tiempo, se sale de verdad.

## 4. Vídeo

DOOM dibuja en un búfer de **320×200 píxeles de 8 bits**: cada byte es un índice a una paleta de 256 colores. El flujo por fotograma es:

1. El motor dibuja en `screens[0]`, igual que en 1993.
2. `I_FinishUpdate` convierte cada índice a RGBA con la paleta actual, a la que ya se ha aplicado la **gamma** (`gammatable`, la de F11).
3. Se pasa ese búfer a JavaScript (sin copiar: es una vista directa sobre la memoria del WebAssembly), que lo pinta en un canvas con `putImageData`.

### La proporción correcta

320×200 es una proporción de 16:10, pero en DOS se mostraba en un **monitor 4:3**, así que los píxeles eran un 20 % más altos que anchos. Si lo muestras con píxeles cuadrados, todo parece achatado.

Para que se vea nítido y con la proporción correcta:

1. El fotograma se escala **×5 en horizontal y ×6 en vertical** (1600×1200) con vecino más cercano. Son enteros, así que todos los píxeles de DOOM ocupan exactamente el mismo bloque.
2. El navegador reduce ese canvas grande al tamaño de la ventana con suavizado.

El resultado es lo que en emulación se llama *sharp bilinear*: píxeles nítidos, sin el centelleo del escalado no entero y con la proporción de un CRT.

### Los cambios de paleta

El destello rojo al recibir daño, el amarillo al coger objetos y el verde del traje antirradiación no son filtros: DOOM **cambia la paleta entera** (`PLAYPAL` tiene 14). Como la conversión a RGBA se hace en cada fotograma con la paleta del momento, funcionan sin hacer nada especial.

## 5. Entrada: teclado y ratón

- **Teclado.** Se usa `event.code` (la tecla física), no `event.key`. Así la distribución es la de un teclado de EE. UU., igual que en DOS, y no depende de si tienes el teclado en español. Cada tecla se traduce a los códigos de `doomdef.h` (`KEY_RCTRL`, `KEY_UPARROW`…) y se envía al motor con `D_PostEvent`, la misma función que usaban los drivers originales.
- **Ratón.** Usa *Pointer Lock*. Los movimientos se acumulan y se envían **una vez por tic** en `I_StartTic`, como hacía la versión de X11 con su *pointer warping*. Los botones mantienen el orden de bits del driver de ratón de DOS: izquierdo = disparar, derecho = desplazamiento lateral, central = avanzar.
- **Ratón vertical.** En el DOOM original, mover el ratón hacia delante **hace andar al jugador**. Era así y es fiel, pero hoy resulta incómodo, así que hay una casilla (activada por defecto) que lo desactiva, como el `-novert` de los ports modernos.
- **El problema de Esc.** Con el puntero capturado, el navegador se queda la tecla Esc para liberarlo, y la página nunca la recibe. La solución: si se pierde la captura y el menú no está abierto, se simula una pulsación de Esc, así que el menú se abre como esperarías. Para saberlo se exporta `menuactive` desde C. En pantalla completa con Chrome se usa además la *Keyboard Lock API*.

## 6. Sonido: el mezclador de id

### Linux vs DOS

En DOS, DOOM usaba **DMX**, una librería comercial de sonido de Paul Radek. Por eso, al liberar el código, id no pudo incluirla. La versión de Linux tenía su propio mezclador por software, más sencillo y con bastantes cabos sueltos.

`src/web/i_sound.c` conserva **el mezclador de id**: 8 canales de muestras de 8 bits sin signo, convertidos con tablas de volumen precalculadas a 16 bits estéreo. Pero arregla lo que en Linux estaba a medias, para acercarse a DOS:

| En Linux | En este port | En DOS (DMX) |
|----------|--------------|--------------|
| `I_StopSound`, `I_UpdateSoundParams` vacías: los sonidos no se podían cortar y el **estéreo no seguía tu posición** al moverte | Funcionan con *handles* reales | Funcionaban |
| `I_SoundIsPlaying` devolvía `gametic < handle`, un apaño | Busca el canal de verdad | Real |
| El volumen llegaba en una escala de 0 a 15 sin multiplicar: sonaba muy bajo | ×8 (0 a 127) | ×8 |
| Panorama con curva cuadrática | Lineal | Lineal |
| Todo a 11 025 Hz | Cada efecto a la frecuencia de su cabecera | Igual |
| Incluía los 16 bytes de relleno de cada muestra (clics) | Se saltan | Se saltaban |

### Modelo *pull*

En Linux, el juego mezclaba 512 muestras por fotograma y las escribía en `/dev/dsp`, una escritura bloqueante que de paso frenaba el juego. En el navegador funciona al revés: **Web Audio pide muestras cuando las necesita** y llama a `I_WebMixAudio(buffer, n)`, que mezcla exactamente `n` muestras a la frecuencia de salida (44,1 o 48 kHz) con remuestreo de vecino más cercano, como una tarjeta de la época.

Esa llamada puede llegar mientras el bucle del juego está «dormido» por Asyncify. Es seguro: Emscripten permite llamar a funciones exportadas que no pausan mientras otra está suspendida, y como todo ocurre en un único hilo no hay condiciones de carrera.

> Pendiente: se usa `ScriptProcessorNode`, que está marcado como obsoleto. La versión moderna es un `AudioWorklet`, que se ejecuta en otro hilo y necesita compartir memoria o pasar mensajes.

## 7. Música: emulando una Sound Blaster

La versión de Linux **no tenía música**. Era la parte más dependiente de DMX y simplemente no se incluyó. Para recuperarla hacen falta tres piezas:

1. **El chip.** Las AdLib y Sound Blaster usaban un **Yamaha YM3812 (OPL2)**, un sintetizador FM. Se emula con [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3), un emulador **de nivel de ciclo** construido a partir del análisis del silicio real. Sus 9 voces suenan exactamente como el chip.
2. **Los instrumentos.** El WAD trae un lump `GENMIDI` con los 175 instrumentos (128 melódicos y 47 de percusión), definidos como parámetros de registro OPL: ataque, sostenido, forma de onda, *feedback*…
3. **El driver.** Aquí sí hay **ingeniería inversa**. Cómo convertía DMX una nota en una frecuencia, cómo calculaba el volumen o qué voz robaba cuando se acababan las 9 son detalles que **solo existían en un binario cerrado**. El proyecto Chocolate Doom los reconstruyó (tablas `frequency_curve`, `volume_mapping_table`, reglas de asignación de voces), y `src/web/i_music.c` reimplementa esa lógica.

Una diferencia de diseño respecto a Chocolate Doom: allí la música se convierte primero de MUS a MIDI. Aquí **se lee el formato MUS directamente**, que es lo que hacía DMX, y se usan los números de canal MUS para las prioridades.

El secuenciador avanza dentro del propio callback de audio: los eventos MUS van a 140 Hz y se procesan justo en la muestra que les toca. Así la música queda perfectamente sincronizada con el audio y no depende de temporizadores de JavaScript.

## 8. Archivos: WADs y partidas guardadas

Emscripten ofrece un sistema de archivos POSIX virtual, así que el código de id sigue usando `open()`, `read()` y `fopen()` como siempre:

```
/wad/doom1.wad     ← descargado por fetch o leído de IndexedDB
/save/.doomrc      ← configuración (HOME=/save)
/save/doomsav0.dsg ← partidas guardadas (directorio actual)
```

- `/save` se monta con **IDBFS**, que vuelca el sistema de archivos en memoria a IndexedDB. Se sincroniza cada 5 segundos y al ocultar la pestaña (momento en el que también se llama a `M_SaveDefaults`, para no perder la configuración aunque no se salga por el menú).
- `linuxdoom` no tiene parámetro `-iwad`: busca el WAD **por nombre de archivo** (`doom2.wad`, `doomu.wad`, `doom.wad`, `doom1.wad`…) en `$DOOMWADDIR`. Por eso, cuando alguien carga su propio WAD, la página **lee el directorio de lumps** para saber qué es (¿tiene `MAP01`? Es DOOM II. ¿Tiene `E4M1`? Es Ultimate DOOM) y lo guarda con el nombre que el motor espera.
- Como `VERSION` es 109, las partidas guardadas tienen el formato de DOS v1.9.

## 9. Cambios en el motor original

Todos los cambios en `src/engine/` llevan el comentario `[WEB]`:

| Archivo | Cambio | Motivo |
|---------|--------|--------|
| `d_net.c` | `I_Yield()` en la espera del siguiente tic | Ceder el control al navegador ([sección 3](#3-el-bucle-infinito-en-un-navegador)) |
| `d_main.c` | `I_Yield()` en la espera del *melt* | Lo mismo |
| `doomdef.h` | `VERSION` 110 → 109 | Reproducir las demos de DOS v1.9 y usar su formato de partidas |
| `doomdef.h` | Sin `#define SNDSERV` | Mezclador integrado, sin el proceso externo `sndserver` de Linux |
| `m_misc.c` | `snd_channels` 3 → 8 | El valor que escribía SETUP.EXE en DOS |
| `w_wad.c` | `strupr` → `W_strupr` | Conflicto de nombre con la libc |
| `i_system.h` | Declaración de `I_Yield` | — |
| `g_game.c` | Traza `DEMOTRACE` (solo si se compila con ese flag) | Verificación ([sección 10](#10-cómo-se-verifica-la-fidelidad)) |

Ni la física, ni el render, ni la IA, ni el generador de números aleatorios se han tocado.

## 10. Cómo se verifica la fidelidad

«Se ve igual» no es una prueba. DOOM ofrece una mucho mejor: las **demos**.

Una demo es solo la lista de `ticcmd` (las teclas pulsadas en cada tic) grabada por un jugador. Al reproducirla, el motor vuelve a simular la partida desde cero. Como DOOM es completamente determinista, cualquier diferencia en la lógica, por pequeña que sea, hace que la partida se desvíe y el jugador acabe andando contra las paredes. Es lo que se llama una **desincronización**.

El método:

1. Añadir a `G_Ticker` una traza que, cada 35 tics (1 segundo), imprime mapa, posición, vida y bajas del jugador.
2. Compilar [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom) de forma nativa con la misma traza. Es el port que se toma como referencia de exactitud frente al `DOOM.EXE` v1.9.
3. Reproducir `DEMO1`, `DEMO2` y `DEMO3` del shareware en los dos con `-timedemo` y comparar las salidas línea a línea.

```sh
make clean && make EXTRA_CFLAGS=-DDEMOTRACE
# navegador:  public/?args=-timedemo%20demo1   → líneas TRACE en la consola
# referencia: chocolate-doom -iwad doom1.wad -timedemo demo1 -nodraw
```

Resultado: **idénticas las tres, hasta el último tic**.

```
TRACE 4970 E1M5 x=1312 y=1283 hp=2 kills=61
TRACE 5005 E1M5 x=1333 y=1287 hp=0 kills=61   ← ambos, al mismo tic
```

Para que esto saliera bien fue clave reproducir las demos de la v1.9. La versión de Linux se identificaba como 1.10 y **rechazaba** las demos de DOS con un *«Demo is from a different game version!»*.

## 11. Bugs de 1997 encontrados por el camino

Portar código antiguo obliga a leerlo con lupa. Algunos hallazgos en el código original de Linux:

- **Longitud de los sonidos enlazados.** En `I_InitSound`, para sonidos que reutilizan los datos de otro (como la ametralladora, que usa el sonido de la pistola), se hacía:
  ```c
  lengths[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
  ```
  La resta de punteros en C ya da el número de elementos, así que dividir entre `sizeof` vuelve a dividir y apunta a otro sonido. Corregido en el nuevo `i_sound.c`.
- **El volumen olvidado.** `S_Init(snd_SfxVolume /* *8 */, ...)`: el `*8` está comentado, así que el volumen llegaba al mezclador en una escala de 0 a 15 cuando esperaba de 0 a 127. En Linux los efectos sonaban a un 12 % de su volumen.
- **`I_SoundIsPlaying` imaginario.** Devolvía `gametic < handle`, sin mirar si el sonido seguía sonando.
- **Comprobación de versión de las demos**, comentada arriba.
- Del lado de DMX hay algún bug que **se conserva a propósito**, porque forma parte de cómo sonaba DOOM: por ejemplo, el último valor de la tabla de frecuencias es un desbordamiento de búfer del driver original.

## 12. Lecciones aprendidas

- **Una buena capa de abstracción dura décadas.** id separó la plataforma del motor en 1993 pensando en DOS, NeXTSTEP y Linux. 30 años después, esa misma frontera es la que permite llevarlo a un navegador cambiando 5 archivos.
- **Asyncify es magia práctica.** Convertir código síncrono de toda la vida en algo que convive con el bucle de eventos del navegador, sin reestructurarlo, es lo que hace viable portar software antiguo con fidelidad. Pero hay que ser consciente de lo que instrumenta.
- **El determinismo es el mejor test.** Si tu sistema es determinista, una grabación de entradas es una prueba de regresión perfecta. DOOM lo tenía en 1993.
- **«Fiel» hay que definirlo y medirlo.** ¿Fiel a Linux 1.10 o a DOS 1.9? Son distintos. Aquí se eligió DOS v1.9, que es lo que jugó todo el mundo, y se verificó contra una referencia externa.
- **WebAssembly de 32 bits le sienta bien al código antiguo.** Los supuestos de la época (`sizeof(int) == sizeof(void*)`) vuelven a ser ciertos.

## Ideas para seguir

- Controles táctiles para móvil.
- Cargar PWADs (mods) además del IWAD.
- **Multijugador**: el protocolo de red original (`d_net.c`) es *peer-to-peer* por paquetes. Encaja de forma natural con los *DataChannels* de WebRTC, sustituyendo `i_net.c`.
- Migrar el audio a `AudioWorklet`.
- Emular los desbordamientos de memoria de DOS (*spechits*, *intercepts*) que Chocolate Doom reproduce para algunas demos de PWADs.
