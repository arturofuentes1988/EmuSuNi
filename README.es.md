<p align="center">
  <img src="docs/banner.png" alt="EmuSuNi" width="860">
</p>

<p align="center">
  <b>Super Nintendo, NES, Nintendo 64, Game Boy, Game Boy Color y Game Boy Advance para PS4 con jailbreak.</b><br>
  Seis consolas en un lanzador al estilo de una consola: carátulas automáticas, imagen HD con IA,
  niveles de calidad, marcos, favoritos, buscador y ajustes para cada juego.
</p>

<p align="center">
  <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest"><img alt="Descargar" src="https://img.shields.io/github/v/release/arturofuentes1988/EmuSuNi?label=descargar&style=for-the-badge&color=7b4dff"></a>
  <img alt="PS4 con GoldHEN" src="https://img.shields.io/badge/PS4-GoldHEN-003791?style=for-the-badge&logo=playstation&logoColor=white">
  <img alt="Snes9x, FCEUmm, Mupen64Plus, mGBA" src="https://img.shields.io/badge/n%C3%BAcleos-Snes9x%20%C2%B7%20FCEUmm%20%C2%B7%20Mupen64Plus%20%C2%B7%20mGBA-e5484d?style=for-the-badge">
  <a href="LICENSE"><img alt="Licencia GPL-2.0+" src="https://img.shields.io/badge/licencia-GPL--2.0%2B-3db46d?style=for-the-badge"></a>
</p>

<p align="center"><a href="README.md">Read in English</a> · <a href="CHANGELOG.md">Historial de cambios</a> · <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest">Descargar</a></p>

---

## Capturas

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/game-snes.png" alt="Super Nintendo"></td>
    <td width="50%"><img src="docs/screenshots/game-n64.png" alt="Nintendo 64"></td>
  </tr>
  <tr>
    <td align="center"><b>Super Nintendo</b> — redibujado con IA (xBRZ + Anime4K)</td>
    <td align="center"><b>Nintendo 64</b> — a 4x, casi Full HD</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/game-gb.png" alt="Game Boy"></td>
    <td><img src="docs/screenshots/game-gba.png" alt="Game Boy Advance"></td>
  </tr>
  <tr>
    <td align="center"><b>Game Boy</b> — con el marco de la consola</td>
    <td align="center"><b>Game Boy Advance</b> — con su marco Índigo</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/game-gbc.png" alt="Game Boy Color"></td>
    <td><img src="docs/screenshots/game-nes.png" alt="NES"></td>
  </tr>
  <tr>
    <td align="center"><b>Game Boy Color</b> — con su marco Frambuesa</td>
    <td align="center"><b>NES</b> — con el marco de TV retro</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/library.png" alt="La Biblioteca"></td>
    <td><img src="docs/screenshots/xmb.png" alt="Estilo XMB"></td>
  </tr>
  <tr>
    <td align="center"><b>Biblioteca</b> — una pestaña por consola, carátulas descargadas solas</td>
    <td align="center"><b>Estilo XMB</b> — las columnas al estilo PSP, 12 temas de color</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/settings-quality.png" alt="Calidad"></td>
    <td><img src="docs/screenshots/11-ingame-settings.png" alt="Menú del juego"></td>
  </tr>
  <tr>
    <td align="center"><b>Calidad</b> — Baja, Media, Alta o Ultra de una vez</td>
    <td align="center"><b>Menú del juego</b> (L3 + R3) — estados guardados, ajustes, trucos</td>
  </tr>
</table>

<details>
<summary>Más capturas</summary>

| | |
|---|---|
| ![Super Game Boy](docs/screenshots/game-gb-sgb.png) | ![Mario Kart 64](docs/screenshots/game-n64-mariokart.png) |
| Game Boy como Super Game Boy: sus colores y su borde | Mario Kart 64 a 4x |
| ![SNES](docs/screenshots/tab-snes.png) | ![NES](docs/screenshots/tab-nes.png) |
| Pestaña Super Nintendo | Pestaña NES |
| ![N64](docs/screenshots/tab-n64.png) | ![GB](docs/screenshots/tab-gb.png) |
| Pestaña Nintendo 64 | Pestaña Game Boy |
| ![GBC](docs/screenshots/tab-gbc.png) | ![GBA](docs/screenshots/tab-gba.png) |
| Pestaña Game Boy Color | Pestaña Game Boy Advance |
| ![Favoritos](docs/screenshots/03-favorites.png) | ![Buscador](docs/screenshots/04-search.png) |
| Favoritos (L2 sobre un juego) | Buscador con teclado en pantalla |
| ![Opciones del juego](docs/screenshots/05-game-options.png) | ![Limpiar juegos](docs/screenshots/08-clean-up.png) |
| Opciones de un juego (Triángulo) | Limpiar juegos: repetidos y otras regiones |

</details>

## Características

**Seis consolas**
- **Super Nintendo** con **Snes9x 1.63**: SuperFX, SA-1, DSP y demás chips, modos de alta
  resolución, hasta **5 jugadores** con el Super Multitap.
- **NES** y Famicom Disk System con **FCEUmm**, con paletas, turbo y filtro NTSC.
- **Nintendo 64** con **Mupen64Plus-Next**, ParaLLEl-RDP (la GPU de la PS4 dibuja a 1x, 2x o 4x) y
  ParaLLEl-RSP, recompilador dinámico, Controller Pak y Rumble Pak.
- **Game Boy, Game Boy Color y Game Boy Advance** con **mGBA**: colores de Game Boy Color o Super
  Game Boy con sus marcos, corrección de color de la pantalla, estela LCD, sensor solar (Boktai) y de
  inclinación (con el stick derecho o moviendo el DualShock 4), vibración y el reloj de los cartuchos
  (Pokémon).
- Partidas guardadas solas, **8 ranuras de estado** con captura, *continuar donde lo dejaste*,
  **trucos** `.cht` de RetroArch y Reset.

**Imagen**
- **Calidad** en un solo ajuste: Baja, Media, Alta o **Ultra** (lo máximo de cada consola).
- **Escalado por GPU**: Pixel Art HD (xBRZ), **IA (xBRZ + red neuronal Anime4K)**, HD suave (AMD
  FSR) y Nitidez HD (AMD RCAS), a la resolución de la TV.
- **Resolución** 2x–6x con xBRZ, HQx o Píxeles nítidos; relación de aspecto, escala entera, zoom,
  scanlines (en las Game Boy, la rejilla de su pantalla), filtro NTSC, V-Sync.
- **Marcos**: tres por consola más una TV retro (las Game Boy con la forma de cada portátil), o los
  tuyos.

**Un lanzador como el de una consola**
- Dos estilos de menú: **Biblioteca** (carátulas en fila, una pestaña por consola con L1 / R1) y
  **XMB** (las columnas de la PSP), 12 temas de color o tus fondos, sonidos de 8 bits.
- **Carátulas automáticas** desde el servidor de libretro (nombres GoodSNES y No-Intro).
- Favoritos, buscador, orden (A–Z, recientes, región, consola), juego predeterminado, explorador de
  carpetas, *Limpiar juegos* y **13 idiomas**.
- **Cada ajuste para todos los juegos, para una consola o para un juego**, con una breve
  explicación de cada opción, y **botones configurables**.

## Requisitos

- Una PS4 con **[GoldHEN](https://github.com/GoldHEN/GoldHEN)** (cualquier modelo).
- Tus propios juegos (también en `.zip`):

  | Consola | Archivos |
  |---|---|
  | Super Nintendo | `.sfc` `.smc` `.swc` `.fig` `.bs` |
  | NES | `.nes` `.fds` `.unf` |
  | Nintendo 64 | `.z64` `.n64` `.v64` |
  | Game Boy | `.gb` `.sgb` |
  | Game Boy Color | `.gbc` (y los `.gb` que pongas en su carpeta, como Pokémon Amarillo) |
  | Game Boy Advance | `.gba` |
- Internet en la PS4 para las carátulas (opcional).

## Instalación

1. Descarga **`EmuSuNi-vXX.YY.pkg`** desde la [última versión](https://github.com/arturofuentes1988/EmuSuNi/releases/latest).
2. Cópialo a un pendrive (FAT32 o exFAT), por ejemplo en una carpeta `PKG`.
3. En la PS4, instálalo con el **Package Installer** de GoldHEN (o *Ajustes → Debug Settings →
   Game → Package Installer*).
4. Abre **EmuSuNi** desde la pantalla de inicio.

Para actualizar, instala el paquete nuevo encima del anterior: tus ajustes, partidas y carátulas
se quedan en `/data/EmuSuNi`.

## Dónde poner los juegos

Lo más cómodo es un **pendrive** con una carpeta por consola:

```
Pendrive
└── Roms
    ├── snes    (Super Nintendo)
    ├── nes
    ├── n64
    ├── gb
    ├── gbc
    └── gba
```

- **N64, GB, GBC y GBA** buscan sus juegos en `Roms/n64`, `Roms/gb`, `Roms/gbc` y `Roms/gba` del
  pendrive sin configurar nada.
- **Super Nintendo y NES** empiezan en `/data/EmuSuNi/games` y `/data/EmuSuNi/nes` (en la PS4, por
  FTP).
- **Cualquier carpeta sirve**: en **Ajustes → Carpetas de juegos** eliges la de cada consola, en el
  pendrive o en la PS4. También se buscan las subcarpetas.
- Después de copiar juegos, pulsa **Cuadrado** en la biblioteca para buscarlos de nuevo.

## Carátulas

- **Se descargan solas** (la PS4 necesita internet) desde el servidor de libretro; el avance se ve
  bajo *Tus juegos*. *Ajustes → Descargar carátulas* las busca de nuevo.
- **Las tuyas también sirven**: un PNG con el nombre del archivo del juego o con su título en
  `/data/EmuSuNi/covers` (Super Nintendo) o en su subcarpeta: `covers/nes`, `covers/n64`,
  `covers/gb`, `covers/gbc`, `covers/gba`.
- Si a un juego le falta, **Triángulo → Buscar carátula** muestra las más parecidas para elegir.

## Controles

**L3 + R3** (los dos sticks presionados) abre el menú del juego en todas las consolas. Todos los
botones se pueden cambiar en **Ajustes → Controles** (para una consola o para un juego).

| DualShock 4 | Super Nintendo | NES | Nintendo 64 | Game Boy / Color | Game Boy Advance |
|---|---|---|---|---|---|
| Cruz | B | B | A | B | B |
| Círculo | A | A | C abajo | A | A |
| Cuadrado | Y | Turbo B | B | Turbo B | Turbo B |
| Triángulo | X | Turbo A | C arriba | Turbo A | Turbo A |
| L1 / R1 | L / R | Cara del disco (FDS) | L / R | — | L / R |
| L2 | — | — | Z | — | — |
| R2 | Avance rápido | Avance rápido | Z | Avance rápido | Avance rápido |
| Options | Start | Start | Start | Start | Start |
| Panel táctil | Select | Select | Avance rápido | Select | Select |
| Stick izquierdo | Cruceta | Cruceta | Stick | Cruceta | Cruceta |
| Stick derecho | — | — | Botones C | Sensor de inclinación | Sensor de inclinación |

| Menú (estilo Biblioteca) | |
|---|---|
| Cruceta izquierda / derecha | Juego anterior / siguiente (arriba / abajo: de 10 en 10) |
| L1 / R1 | Pestañas: Todos, cada consola, Favoritos, Ajustes |
| Cruz | Jugar |
| Triángulo | Opciones del juego: favorito, ajustes, trucos, controles, buscar carátula, renombrar, información, juego predeterminado, borrar |
| L2 / R2 | Favorito / buscar |
| Cuadrado | Buscar los juegos de nuevo |
| Options | Ordenar |
| Círculo | Volver |

Mantén **L1** al abrir EmuSuNi para ir al menú en vez del juego predeterminado.

## Ajustes

Cada ajuste puede aplicarse a **todos los juegos**, a **una consola** o a **un juego** (*Aplicar a*,
arriba en los ajustes de un juego). Debajo de cada lista, un texto breve explica el ajuste y su
opción actual.

| Categoría | Ajustes |
|---|---|
| **Video** | **Calidad** (Baja, Media, Alta, Ultra) · Resolución · Resolución N64 · Relación de aspecto · Tamaño de imagen · Zoom · **Marco** de cada consola · Recortar overscan · Límite de fotogramas · V-Sync · Contador de FPS |
| **Gráficos** | Filtro de escalado · **Escalado por GPU** (IA, Pixel Art HD, HD suave, Nitidez HD) · Filtrado · Scanlines · Filtro NTSC · Antialiasing, tramado, 2D, texturas y efectos de la N64 · **Colores de Game Boy** · Colores de pantalla · Estela LCD · Mezcla de alta resolución · Parpadeo de sprites · Transparencias |
| **Rendimiento** | Reloj de la CPU · Reloj del SuperFX · Sincronía, núcleos, momento de imagen, ritmo de fotogramas y ritmo de CPU de la N64 · Velocidad · Avance rápido · Salto de fotogramas · Gráficos de rendimiento |
| **Emulación** | Región · CPU, RSP y Controller Pak de la N64 · Paleta de NES · Botones turbo · Logo de inicio (BIOS) · Luz solar · Sensor de inclinación · Direcciones opuestas · Memoria aleatoria y más |
| **Sistema** | Idioma · Volumen · Interpolación de audio · Filtro de sonido · Búfer de audio · Multitap · Continuar donde lo dejaste |

## Archivos en la PS4

Todo está en `/data/EmuSuNi` (accesible por FTP con GoldHEN):

| Ruta | Contenido |
|---|---|
| `games/`, `nes/` | Las carpetas de juegos predeterminadas de Super Nintendo y NES |
| `covers/` | Carátulas (subcarpetas `nes`, `n64`, `gb`, `gbc`, `gba`) |
| `saves/`, `states/` | Partidas de batería (y el reloj de los cartuchos, `.rtc`) y estados guardados |
| `cheats/` | Trucos: `<nombre del archivo>.cht` (formato RetroArch) |
| `system/` | BIOS opcionales: `disksys.rom` (Famicom Disk System), `gb_bios.bin`, `gbc_bios.bin`, `sgb_bios.bin`, `gba_bios.bin` |
| `bezels/` | Tus marcos: PNG de 1920×1080 transparente donde va el juego, en `bezels/<consola>/` |
| `themes/` | Fondos (PNG, 1920×1080) y sonidos que reemplazan a los incluidos |
| `settings.ini`, `controls.ini`, `xmb.ini`, `favorites.ini` | Tus ajustes |
| `emusuni.log` | El registro, para informar de errores |

## Compilar

Se compila en **Ubuntu 24.04** (WSL2 sirve) con clang 21, el OpenOrbis de PacBrew y el runtime de
PS4 de DolphinPS4 (libc++ 21 y el controlador Vulkan RADV de Mesa).

```sh
# una vez: paquetes, toolchain, libc++, Mesa, los núcleos y Dear ImGui (como root, ~45 minutos)
sudo -i bash /ruta/a/EmuSuNi/scripts/setup.sh

# luego
EMUSUNI_VERSION=00.19 bash scripts/build.sh   # compila ~/emusuni-build/emusuni_eboot/eboot.bin
bash scripts/package.sh 00.19                 # crea el .pkg en ~/emusuni-build/out
```

## Créditos

- **[Snes9x](https://github.com/snes9xgit/snes9x)** (Super Nintendo),
  **[FCEUmm](https://github.com/libretro/libretro-fceumm)** (NES),
  **[Mupen64Plus-Next](https://github.com/libretro/mupen64plus-libretro-nx)** con ParaLLEl-RDP y
  ParaLLEl-RSP (Nintendo 64) y **[mGBA](https://mgba.io)** (Game Boy, Color y Advance) — los
  emuladores.
- **[Anime4K](https://github.com/bloc97/Anime4K)** y **AMD FidelityFX Super Resolution** — la imagen
  HD con IA y el escalado suave.
- **[DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)** — el diseño del lanzador, el runtime de
  PS4, el port de Mesa y la configuración de compilación de los que nació EmuSuNi.
- **[Dear ImGui](https://github.com/ocornut/imgui)**, **[Mesa](https://mesa3d.org)**,
  **[OpenOrbis](https://github.com/OpenOrbis) / [PacBrew](https://github.com/PacBrew)**,
  **[libretro-thumbnails](https://github.com/libretro-thumbnails)**, xBRZ, HQx, el filtro NTSC de
  blargg y los autores de las fuentes. Ver [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

EmuSuNi por [@arturofuentes1988](https://github.com/arturofuentes1988).

## Licencia y aviso legal

El código propio de EmuSuNi tiene licencia **GNU GPL v2 o posterior** ([LICENSE](LICENSE)). Snes9x
tiene su propia licencia, que solo permite el uso gratuito y **no comercial**; los paquetes de
EmuSuNi son gratuitos. Los demás componentes mantienen sus licencias
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

**No incluye juegos.** Usa juegos que te pertenezcan. EmuSuNi no está afiliado ni respaldado por
Nintendo ni por Sony. *Super Nintendo*, *Super Famicom*, *SNES*, *NES*, *Nintendo 64*, *Game Boy*,
*Game Boy Color* y *Game Boy Advance* son marcas de Nintendo; *PlayStation* y *PS4* son marcas de
Sony Interactive Entertainment.
