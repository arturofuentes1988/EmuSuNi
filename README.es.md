<p align="center">
  <img src="docs/banner.png" alt="EmuSuNi" width="860">
</p>

<p align="center">
  <b>Veinte consolas clásicas y placas arcade en una sola app para PS4 con jailbreak.</b><br>
  Super Nintendo · NES · Nintendo 64 · Game Boy · Game Boy Color · Game Boy Advance · PlayStation ·
  Nintendo DS · Nintendo 3DS · PSP · Dreamcast · NAOMI · Atomiswave · SG-1000 · Master System ·
  Game Gear · Genesis · Saturn · PC Engine · Arcade
</p>

<p align="center">
  Un lanzador que se siente como el de una consola: tus juegos con sus carátulas, imagen HD con IA,
  trucos de internet, RetroAchievements, estados guardados, marcos y ajustes para cada juego.
</p>

<p align="center">
  <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest"><img alt="Descargar" src="https://img.shields.io/github/v/release/arturofuentes1988/EmuSuNi?label=descargar&style=for-the-badge&color=7b4dff"></a>
  <img alt="PS4 con GoldHEN" src="https://img.shields.io/badge/PS4-GoldHEN-003791?style=for-the-badge&logo=playstation&logoColor=white">
  <img alt="20 consolas" src="https://img.shields.io/badge/consolas-20-e5484d?style=for-the-badge">
  <a href="LICENSE"><img alt="Licencia GPL-2.0+" src="https://img.shields.io/badge/licencia-GPL--2.0%2B-3db46d?style=for-the-badge"></a>
</p>

<p align="center"><a href="README.md">Read in English</a> · <a href="CHANGELOG.md">Historial de cambios</a> · <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest">Descargar</a> · <a href="https://t.me/+5Y2CVb3aR500OGMx">Telegram</a></p>

---

## Guía rápida

1. **Instala**: descarga `EmuSuNi-vXX.YY.pkg` de la [última versión](https://github.com/arturofuentes1988/EmuSuNi/releases/latest),
   cópialo a un pendrive e instálalo con el **Package Installer** de GoldHEN ([Instalación](#instalación)).
   **Para actualizar**: desde la v00.22, la app avisa cuando sale una versión nueva y **Ajustes → Buscar
   actualizaciones** la instala; o instala el paquete nuevo encima del anterior (tus juegos, partidas y
   ajustes se quedan). Desde la v00.21, instala la v00.22 a mano esta única vez.
2. **Pon tus juegos** en `Roms/<consola>` del pendrive o en `/data/EmuSuNi/Roms/<consola>` de la PS4
   (por FTP). La app crea las carpetas sola: `snes`, `nes`, `n64`, `gb`, `gbc`, `gba`, `ps1`, `nds`,
   `3ds`, `psp`, `dc`, `naomi`, `atomiswave`, `sg1000`, `sms`, `gg`, `md` (Genesis), `saturn`, `pce`
   (PC Engine) y `arcade`. Los juegos de Arcade, NAOMI y Atomiswave se quedan en su `.zip`
   ([formatos](#las-consolas)).
3. **BIOS**: solo la necesitan los juegos de Neo Geo del Arcade (`neogeo.zip`), NAOMI (`naomi.zip`,
   `naomi2.zip`), Atomiswave (`awbios.zip`) y la Saturn (un `.bin` de 512 KB, como `saturn_bios.bin`), y
   los del Famicom Disk System (`disksys.rom`) ([BIOS](#bios)). No se incluyen juegos ni BIOS.
4. **Juega**: tus juegos aparecen solos, con su carátula si la PS4 tiene internet. En el juego,
   **L3 + R3** abre el menú: estados guardados, ajustes, controles, salir.
5. **Ayuda y errores**: el [grupo de Telegram](https://t.me/+5Y2CVb3aR500OGMx), o una
   [incidencia](https://github.com/arturofuentes1988/EmuSuNi/issues) con tu `/data/EmuSuNi/emusuni.log`.

## Las consolas

| Consola | Emulador | Formatos | BIOS |
|---|---|---|---|
| **Super Nintendo** | Snes9x 1.63 | `.sfc` `.smc` `.swc` `.fig` `.bs` | No hace falta |
| **NES** y Famicom Disk System | FCEUmm | `.nes` `.fds` `.unf` `.unif` | No hace falta |
| **Nintendo 64** | Mupen64Plus-Next | `.z64` `.n64` `.v64` | No hace falta |
| **Game Boy** y **Game Boy Color** | mGBA | `.gb` `.sgb` `.dmg` `.gbc` `.cgb` | No hace falta |
| **Game Boy Advance** | mGBA | `.gba` `.agb` | No hace falta |
| **PlayStation** | SwanStation | `.cue` `.bin` `.chd` `.pbp` `.m3u` `.iso` `.img` `.ecm` `.mds` | No hace falta |
| **Nintendo DS** | melonDS DS 1.4.0 | `.nds` `.ids` | No hace falta |
| **Nintendo 3DS** — *experimental* | Azahar 2126.2 | `.3ds` `.cci` `.cxi` `.3dsx` descifrados | No hace falta |
| **PSP** | PPSSPP 1.20.4 | `.iso` `.cso` `.chd` `.pbp` `.elf` | No hace falta |
| **Dreamcast** | Flycast 2.7 | `.gdi` `.cdi` `.chd` `.cue` `.m3u` | No hace falta |
| **NAOMI** y NAOMI 2 | Flycast 2.7 | Sets de MAME `.zip` `.7z`, imágenes `.dat` `.bin` | **La tuya**: `naomi.zip`, `naomi2.zip` |
| **Atomiswave** | Flycast 2.7 | Sets de MAME `.zip` `.7z` | **La tuya**: `awbios.zip` |
| **SG-1000** y SC-3000 | Genesis Plus GX | `.sg` `.sc` | No hace falta |
| **Master System** | Genesis Plus GX | `.sms` `.bms` | No hace falta |
| **Game Gear** | Genesis Plus GX | `.gg` | No hace falta |
| **Genesis** / Mega Drive | Genesis Plus GX | `.md` `.gen` `.smd` `.bin` `.mdx` `.68k` `.sgd` | No hace falta |
| **Saturn** — *experimental, nueva* | YabaSanshiro 3.4.2 | `.cue` `.chd` `.iso` `.ccd` `.mds` | **La tuya**: un `.bin` de 512 KB ([BIOS](#bios)) |
| **PC Engine** / TurboGrafx-16 — *nueva* | Beetle PCE Fast | `.pce` (HuCards) | No hace falta |
| **Arcade**: Neo Geo, Capcom CPS1/CPS2/CPS3, Sega, Konami, Taito... | FinalBurn Neo 1.0.0.03 | Un `.zip` / `.7z` por juego, sin descomprimir | Neo Geo: **la tuya**, `neogeo.zip` |

No se incluyen BIOS ni juegos. Solo los juegos de Neo Geo del Arcade, NAOMI, Atomiswave y la Saturn
necesitan una BIOS tuya ([BIOS](#bios)).

La **Saturn** es experimental: dibuja por software, en hilos propios, y sus juegos pueden ir más lentos que
en la consola. La **Nintendo 3DS** sigue siendo experimental: solo juegos descifrados; si un juego no carga a
la primera, **L3 + R3 → Reiniciar juego** hasta que cargue. La PC Engine juega HuCards (no juegos en CD).

## Novedades de la 00.22

- **Dos consolas nuevas**: la **Sega Saturn** (experimental) y la **PC Engine / TurboGrafx-16**.
- **RetroAchievements** (Casual) en todas las consolas menos la 3DS: inicia sesión en **Ajustes →
  RetroAchievements**; **Triángulo → Logros** muestra los logros de un juego; una marca "RA" en las
  carátulas de los juegos que los tienen, y el **panel táctil** muestra solo esos; **Mis logros**; sus
  textos traducidos al idioma de la app.
- **Actualizaciones desde la app**: **Ajustes → Buscar actualizaciones** avisa cuando sale una versión
  nueva, la descarga y se la pasa al instalador de la PS4. Desde la v00.21, instala la v00.22 a mano esta
  única vez.
- **Estrellas de valoración** bajo las carátulas (los jugadores de la LaunchBox Games Database) y **un marco
  para cada juego** de The Bezel Project (**Ajustes → Marcos de los juegos** descarga los de tus juegos).
- **Las pantallas de la DS y la 3DS con los mismos ajustes**: Disposición de pantallas (una sobre otra, lado
  a lado, pantalla grande + pequeña, una sola pantalla), Cambiar pantallas, el tamaño y la posición de la
  pantalla pequeña, la separación entre ellas.
- **Calidades** en la Dreamcast, NAOMI y Atomiswave (hasta 3x) y en la 3DS (hasta 4x); en todas las consolas
  cada calidad es ahora claramente más nítida que la anterior.
- **Más rápidas**: la Dreamcast, NAOMI y Atomiswave, la Saturn, la 3DS (menos paradas para compilar sus
  shaders y su código) y la N64. El sonido mantiene su tono cuando un juego va por debajo de su velocidad.
- **Arreglos**: la PSP ya no va demasiado rápida con el salto de fotogramas (God of War); arrancan los `.bin`
  de PlayStation sin su `.cue`, las ISO de sectores de 2048 bytes y los discos con LibCrypt; Cruis'n USA,
  Resident Evil 2 y World Driver Championship de N64 ya no se quedan en negro, y Ultra ya no dibuja líneas
  sobre su 2D; ajustes automáticos para los errores gráficos de Pokémon X e Y, Majora's Mask 3D, Metroid:
  Samus Returns y Castlevania: Mirror of Fate en la 3DS.
- **Carátulas**: las de Super Nintendo en `covers/snes`, JPEG y de cualquier tamaño; los archivos numerados
  (`01 Comix Zone`) tienen la suya; **Juegos sin carátula** dice por qué no se usa una imagen.
- **Se aplican las listas de los propios emuladores con ajustes especiales para algunos juegos** (el registro
  dice cuáles).
- **Un juego que no abre a la primera se vuelve a abrir solo.**
- **Un solo teclado para todos los textos** (cambiar nombre, buscar, iniciar sesión) con los botones del menú;
  todos los menús dan la vuelta; **L1 / R1 mantenidos** recorren las consolas; el menú del juego también puede
  abrirse manteniendo R3 o el panel táctil, con Options + panel táctil, o manteniendo L1 + R1 o L2 + R2
  (**Ajustes → Controles → Menú del juego**); zona muerta y sensibilidad del stick de PSP.
- Todas las consolas parten con Calidad Baja, Relación de aspecto Auto y su propio marco.

Todos los detalles: las [notas de la versión](https://github.com/arturofuentes1988/EmuSuNi/releases/latest) y el [historial de cambios](CHANGELOG.md).

## Capturas

Novedades de la 00.22:

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/v00.22/1-saturn-library.png" alt="Página de la Sega Saturn"></td>
    <td width="50%"><img src="docs/screenshots/v00.22/2-saturn-daytona-usa.png" alt="Daytona USA en la Saturn"></td>
  </tr>
  <tr>
    <td align="center"><b>Sega Saturn</b> (experimental) — carátulas, estrellas y la marca RA</td>
    <td align="center"><b>Daytona USA</b> — con su propio marco de The Bezel Project</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/v00.22/3-pc-engine-library.png" alt="Página de la PC Engine"></td>
    <td><img src="docs/screenshots/v00.22/4-pc-engine-new-adventure-island.png" alt="New Adventure Island en la PC Engine"></td>
  </tr>
  <tr>
    <td align="center"><b>PC Engine / TurboGrafx-16</b> — HuCards</td>
    <td align="center"><b>New Adventure Island</b> — con su propio marco</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/v00.22/5-achievements-in-game.png" alt="Logros en el menú del juego"></td>
    <td><img src="docs/screenshots/v00.22/6-my-achievements.png" alt="Mis logros"></td>
  </tr>
  <tr>
    <td align="center"><b>RetroAchievements</b> en el menú del juego (L3 + R3) — traducidos al español</td>
    <td align="center"><b>Mis logros</b> — lo último que desbloqueaste y tus juegos</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/v00.22/7-check-for-updates.png" alt="Buscar actualizaciones"></td>
    <td></td>
  </tr>
  <tr>
    <td align="center"><b>Buscar actualizaciones</b> — la app te avisa de cada versión nueva</td>
    <td></td>
  </tr>
</table>

Versiones anteriores:

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

**Imagen**
- **Calidad** en un solo ajuste: Baja, Media, Alta o **Ultra** (lo máximo de cada consola), cada una
  claramente más nítida que la anterior. Baja es la imagen de la propia consola; en las consolas 2D
  Media, Alta y Ultra son Suavizado de bordes, Pixel Art HD (xBRZ) e IA (xBRZ + Anime4K), en las 3D una
  imagen más grande con bordes suavizados, más nítida en Ultra.
- **Escalado por GPU**: Suavizado de bordes (MMPX y SMAA), HD natural (AMD FSR), IA natural (la red
  neuronal Anime4K, sin aspecto de cera), Pixel Art HD (xBRZ), IA (xBRZ + Anime4K), HD suave y
  Nitidez HD, a la resolución de la TV.
- **3D en HD**: la Nintendo 64, la PlayStation y la PSP hasta 4x (la PSP con texturas mejoradas), la
  Dreamcast, NAOMI y Atomiswave hasta 3x, la 3DS hasta 4x.
- **Resolución** 2x–6x con xBRZ, HQx o Píxeles nítidos; relación de aspecto, escala entera, zoom,
  líneas de escaneo (en las Game Boy, la rejilla de su pantalla), filtro NTSC, V-Sync.
- **Marcos para todas las consolas**: el de su propia consola, una TV retro y seis marcos comunes
  (mueble arcade, minimalista oscuro, neón, madera, carbono, estrellas), los tuyos, o **un marco para
  cada juego** de [The Bezel Project](https://github.com/thebezelproject) (**Ajustes → Marcos de los
  juegos** descarga los de tus juegos). **TV de tubo (CRT)**: cristal curvo, líneas y máscara de
  fósforo, en cuatro estilos.
- **Pantallas de la DS y la 3DS**: una sobre otra, lado a lado, una grande y otra pequeña, o una sola;
  cambiadas de lugar, con separación, la pequeña donde quieras.

**Para jugar**
- Partidas guardadas solas, **8 ranuras de estado** con captura, *continuar donde lo dejaste* y
  reiniciar.
- **[RetroAchievements](https://retroachievements.org)** (Casual) en todas las consolas menos la 3DS:
  inicia sesión en **Ajustes → RetroAchievements**, desbloquea logros mientras juegas, mira los de un
  juego con **Triángulo → Logros** y los tuyos en **Mis logros**; una marca "RA" en las carátulas de los
  juegos que los tienen; los textos de los logros traducidos al idioma de la app.
- **Trucos**: archivos `.cht` de RetroArch, o **Buscar trucos en internet** (no en Dreamcast, NAOMI,
  Atomiswave, Arcade ni 3DS, cuyos emuladores aquí no aceptan trucos).
- **Juegos de varios discos** agrupados solos, con **Disco** en el menú del juego.
- **El menú del juego** (L3 + R3, u otros botones que elijas): guardar y cargar estados, cambiar el
  disco, ajustes, trucos, logros, reiniciar, salir.
- Hasta 5 mandos DualShock 4, vibración, avance rápido.
- Un juego que no abre a la primera se vuelve a abrir solo.

**Un lanzador como el de una consola**
- Dos estilos de menú: **Biblioteca** (carátulas en fila, una pestaña por consola con L1 / R1, que
  da la vuelta) y **XMB** (las columnas de la PSP), 12 temas de color o tus fondos, sonidos de 8 bits.
- **Carátulas automáticas** desde el servidor de libretro, y **estrellas de valoración** bajo ellas (los
  jugadores de la [LaunchBox Games Database](https://gamesdb.launchbox-app.com)).
- **Actualizaciones desde la app**: **Ajustes → Buscar actualizaciones** avisa cuando sale una versión
  nueva, la descarga y se la pasa al instalador de la PS4.
- **Orden de las consolas**: en el orden que quieras, o Por empresa, Por potencia, Por año o Por
  abecedario.
- Favoritos, buscador, orden (A–Z, recientes, región, consola), juego predeterminado, explorador de
  carpetas, *Limpiar juegos* y **13 idiomas** (completos en inglés y español; los otros 11, solo en
  parte).
- **Cada ajuste para todos los juegos, para una consola o para un juego**, con una breve
  explicación de cada opción, y **botones configurables**.

## Requisitos

- Una PS4 con **[GoldHEN](https://github.com/GoldHEN/GoldHEN)** (cualquier modelo).
- Tus propios juegos:

  | Consola | Archivos |
  |---|---|
  | Super Nintendo | `.sfc` `.smc` `.swc` `.fig` `.bs` |
  | NES | `.nes` `.fds` `.unf` `.unif` |
  | Nintendo 64 | `.z64` `.n64` `.v64` |
  | Game Boy | `.gb` `.sgb` `.dmg` |
  | Game Boy Color | `.gbc` `.cgb` (y los `.gb` que pongas en su carpeta, como Pokémon Amarillo) |
  | Game Boy Advance | `.gba` `.agb` |
  | PlayStation | `.cue` (con sus pistas `.bin`) o un `.bin` solo, `.chd` `.pbp` `.m3u` `.iso` `.img` `.ecm` `.mds` |
  | Nintendo DS | `.nds` `.ids` |
  | Nintendo 3DS (experimental) | `.3ds` `.cci` `.cxi` `.3dsx` descifrados (no `.cia`) |
  | PSP | `.iso` `.cso` `.chd` `.pbp` `.elf` |
  | Dreamcast | `.gdi` (con sus pistas) `.chd` `.cdi` `.cue` `.m3u` |
  | NAOMI / NAOMI 2 | Sets de MAME `.zip` `.7z` sin descomprimir, o imágenes `.dat` `.bin` |
  | Atomiswave | Sets de MAME `.zip` `.7z` sin descomprimir |
  | SG-1000 / SC-3000 | `.sg` `.sc` |
  | Master System | `.sms` `.bms` |
  | Game Gear | `.gg` |
  | Genesis / Mega Drive | `.md` `.gen` `.smd` `.bin` `.mdx` `.68k` `.sgd` |
  | Saturn (experimental) | `.cue` (con sus pistas `.bin`) `.chd` `.iso` `.ccd` `.mds` |
  | PC Engine / TurboGrafx-16 | `.pce` (HuCards) |
  | Arcade | Sets de FinalBurn Neo 1.0.0.03: un `.zip` / `.7z` por juego, sin descomprimir (`mslug.zip`) |

  Las consolas de cartucho (de Super Nintendo a Game Boy Advance, de SG-1000 a Genesis, y la PC Engine)
  también aceptan `.zip`; las de disco, la DS y la 3DS leen sus juegos tal cual. El `.zip` de un juego de
  Arcade, NAOMI o Atomiswave es el juego en sí: déjalo comprimido y con el nombre de su set.
- Internet en la PS4 para las carátulas y los trucos de internet (opcional).

## Instalación

1. Descarga **`EmuSuNi-vXX.YY.pkg`** desde la [última versión](https://github.com/arturofuentes1988/EmuSuNi/releases/latest).
2. Cópialo a un pendrive (FAT32 o exFAT), por ejemplo en una carpeta `PKG`.
3. En la PS4, instálalo con el **Package Installer** de GoldHEN (o *Ajustes → Debug Settings →
   Game → Package Installer*).
4. Abre **EmuSuNi** desde la pantalla de inicio.

**Para actualizar**: desde la v00.22, la app mira una vez al día y avisa cuando sale una versión nueva;
**Ajustes → Buscar actualizaciones** mira ahora, y **Actualizar ahora** descarga el paquete (a la carpeta
`PKG` del USB, o a la PS4) y se lo pasa al instalador de la PS4 (la instalación se ve en *Notificaciones →
Descargas*; cierra EmuSuNi para que la PS4 la termine). Desde la v00.21, instala la v00.22 a mano esta
única vez.

A mano, instala el paquete nuevo encima del anterior: tus ajustes, partidas, estados y
carátulas se quedan en `/data/EmuSuNi`. Si el Package Installer no deja instalarlo encima, borra EmuSuNi
desde la pantalla de inicio (**OPTIONS → Eliminar**) e instala el paquete nuevo: `/data/EmuSuNi` no se
toca, así que no pierdes nada.

## Dónde poner los juegos

La app crea sola una carpeta para cada consola, en la PS4 y en el USB que uses para juegos:

| Dónde | Ruta | Por FTP |
|---|---|---|
| Memoria USB | `Roms/<consola>` en la raíz del USB | `/mnt/usb0/Roms/<consola>` |
| La PS4 | `/data/EmuSuNi/Roms/<consola>` | `/data/EmuSuNi/Roms/<consola>` |

Las carpetas: `snes`, `nes`, `n64`, `gb`, `gbc`, `gba`, `ps1`, `nds`, `3ds`, `psp`, `dc`, `naomi`,
`atomiswave`, `sg1000`, `sms`, `gg`, `md` (Genesis), `saturn`, `pce` (PC Engine) y `arcade`.

- Copia ahí tus juegos (por FTP o con el USB en un PC): aparecen solos. Pulsa **Cuadrado** en la Biblioteca
  para buscar otra vez enseguida.
- **Cualquier carpeta** sirve también: **Ajustes → Carpetas de juegos** elige otra para una consola.
- En la memoria de la PS4 importan las mayúsculas: un zip de arcade, NAOMI o Atomiswave debe llevar el
  nombre de su set en minúsculas (`mslug.zip`, `mvsc2.zip`).
- Borrar o actualizar la app no borra `/data/EmuSuNi`: tus juegos, partidas, carátulas y ajustes se quedan.

**Juegos de varios discos** (PlayStation, Dreamcast): se juntan solos en un juego cuando:
- sus archivos están juntos con el disco en el nombre: `Juego (Disc 1).chd`, `Juego (Disc 2).chd`
  (también `(CD2)`, `(Disk 2)`, `[Disc 2]`);
- cada disco tiene su carpeta: `Juego (Disc 1)/disc.gdi`, o `Juego/Disc 1/disc.gdi`;
- o un archivo `.m3u` los enumera.

En el juego, **L3 + R3 → Disco** cambia el disco (Izquierda / Derecha para elegir, Cruz para meterlo). La
próxima vez el juego arranca con el último disco que usaste.

## BIOS

No se incluye ninguna. Solo estas necesitan la tuya:

| Consola | Archivos | Carpeta |
|---|---|---|
| Arcade: juegos de Neo Geo | `neogeo.zip` | Junto a los juegos (`Roms/arcade`) |
| NAOMI / NAOMI 2 | `naomi.zip`, `naomi2.zip` | Junto a los juegos (`Roms/naomi`) o `/data/EmuSuNi/system/dc` |
| Atomiswave | `awbios.zip` | Junto a los juegos (`Roms/atomiswave`) o `/data/EmuSuNi/system/dc` |
| Saturn | Un `.bin` de 512 KB: `saturn_bios.bin` o `mpr-17933.bin` (EE. UU. / Europa), `sega_101.bin` (Japón), o cualquier `.bin` que lleve "bios" en el nombre (se prefiere una de EE. UU. o Europa) | `/data/EmuSuNi/system/saturn`, `/data/EmuSuNi/system` o junto al juego (`Roms/saturn`) |
| Famicom Disk System | `disksys.rom` | `/data/EmuSuNi/system` |

Las demás consolas funcionan sin BIOS, la PC Engine incluida. Si tienes las reales, se usan: PlayStation (`scph5501.bin` u otra,
más juegos funcionan) y Nintendo DS (`bios7.bin`, `bios9.bin`, `firmware.bin`) en `/data/EmuSuNi/system`,
Dreamcast (`dc_boot.bin`) en `/data/EmuSuNi/system/dc`.

## Carátulas

- **Se descargan solas** (la PS4 necesita internet) desde el servidor de libretro; el avance se ve
  bajo *Tus juegos*. *Ajustes → Descargar carátulas* las busca de nuevo.
- **Las tuyas también sirven**: un PNG o JPEG con el nombre del archivo del juego o con su título en
  `/data/EmuSuNi/covers/<consola>`, con los mismos nombres de carpeta que los juegos (`covers/snes`,
  `covers/nes`, `covers/md`...), o en `EmuSuNi/covers/<consola>` en el USB. De cualquier tamaño: una imagen
  grande se reduce al leerla. **Ajustes → Juegos sin carátula** dice por qué no se usa una imagen tuya.
- Si a un juego le falta, **Triángulo → Buscar carátula** muestra las más parecidas para elegir.

## Controles

**L3 + R3** (los dos sticks presionados) abre el menú del juego en todas las consolas (**Ajustes →
Controles → Menú del juego** añade R3 o el panel táctil mantenidos, Options + panel táctil, L1 + R1 o
L2 + R2 mantenidos). Todos los
botones se pueden cambiar desde un juego: **Triángulo → Controles** (*Aplicar a*: Este juego, o
Todos los juegos, que son todos los de su consola).

| DualShock 4 | Super Nintendo | NES | Nintendo 64 | Game Boy / Color | Game Boy Advance |
|---|---|---|---|---|---|
| Cruz | B | B | A | B | B |
| Círculo | A | A | C abajo | A | A |
| Cuadrado | Y | Turbo B | B | Turbo B | Turbo B |
| Triángulo | X | Turbo A | C arriba | Turbo A | Turbo A |
| L1 / R1 | L / R | Lado del disco / insertar o expulsar (FDS) | L / R | — | L / R |
| L2 | — | — | Z | — | — |
| R2 | Avance rápido | Avance rápido | Z | Avance rápido | Avance rápido |
| Options | Start | Start | Start | Start | Start |
| Panel táctil | Select | Select | Avance rápido | Select | Select |
| Stick izquierdo | Cruceta | Cruceta | Stick | Cruceta | Cruceta |
| Stick derecho | — | — | Botones C | Sensor de inclinación | Sensor de inclinación |

| DualShock 4 | PlayStation | Nintendo DS | PSP | Dreamcast |
|---|---|---|---|---|
| Cruz | Cruz | B | Cruz | A |
| Círculo | Círculo | A | Círculo | B |
| Cuadrado | Cuadrado | Y | Cuadrado | X |
| Triángulo | Triángulo | X | Triángulo | Y |
| L1 / R1 | L1 / R1 | L / R | L / R | — |
| L2 | L2 | Select | — | Gatillo L (analógico) |
| R2 | R2 | Avance rápido | Avance rápido | Gatillo R (analógico) |
| L3 / R3 | L3 / R3 | Micrófono (soplar) / cambiar pantallas | — | — |
| Options | Start | Start | Start | Start |
| Panel táctil | Select | Pantalla táctil | Select | — |
| Stick izquierdo | Stick izquierdo | Cruceta | Stick | Stick |
| Stick derecho | Stick derecho | — | — | — |

| DualShock 4 | Saturn | PC Engine |
|---|---|---|
| Cruz | B | II |
| Círculo | C | I |
| Cuadrado | A | III (mando de 6 botones) |
| Triángulo | Y | IV (mando de 6 botones) |
| L1 / R1 | X / Z | V / VI (mando de 6 botones) |
| L2 | L | — |
| R2 | R | Avance rápido |
| Options | Start | Run |
| Panel táctil | Avance rápido | Select |
| Stick izquierdo | Cruceta | Cruceta |

| Menú (estilo Biblioteca) | |
|---|---|
| Cruceta izquierda / derecha | Juego anterior / siguiente (arriba / abajo: de 10 en 10) |
| L1 / R1 | Pestañas: Todos, cada consola, Favoritos, Ajustes (mantenidos: las recorren) |
| Cruz | Jugar |
| Triángulo | Opciones del juego: favorito, ajustes del juego, trucos, controles, cambiar nombre, buscar carátula, logros, información, juego predeterminado, borrar |
| L2 / R2 | Favorito / buscar |
| Cuadrado | Buscar los juegos de nuevo |
| Panel táctil | Filtro RA: todos los juegos, los que tienen logros, los que están en curso |
| Options | Ordenar |
| Círculo | Volver |

Mantén **L1** al abrir EmuSuNi para ir al menú en vez del juego predeterminado.

## Ajustes

**Ajustes**, en el menú, tiene las opciones de la app: Temas, Idioma, Controles, RetroAchievements,
Carpetas de juegos, Orden de las consolas, Consolas mostradas, Limpiar juegos, Descargar carátulas,
Juegos sin carátula, Marcos de los juegos, Estrellas de valoración, Efectos de sonido, Estilo del menú,
Reloj, Restablecer todos los ajustes, Buscar actualizaciones...

Los ajustes de un juego se cambian desde el juego: **Triángulo → Ajustes del juego** en el menú, o
**Ajustes** en el menú del juego. Cada uno puede aplicarse a **todos los juegos**, a **una
consola** o a **un juego** (*Aplicar a*, arriba), y un texto breve bajo la lista explica el ajuste y
su opción actual.

| Categoría | Ajustes |
|---|---|
| **Vídeo** | **Calidad** (Baja, Media, Alta, Ultra) · Resolución · Resolución de N64, de PS1, de PSP, de Dreamcast y de 3DS · Relación de aspecto · Disposición de pantallas, Cambiar pantallas, Tamaño y Posición de la pantalla pequeña y Separación entre pantallas de la DS y la 3DS · Pantalla panorámica PS1 · Tamaño de imagen · Zoom panorámico · **Marco** · Recortar overscan · Límite de fotogramas · V-Sync · Contador de FPS |
| **Gráficos** | Filtro de escalado · **Escalado por GPU** · Filtrado de texturas · suavizado de bordes, tramado, 2D, texturas lejanas y efectos de pantalla de la N64 · geometría (PGXP), filtrado de texturas, colores y antialiasing de la PlayStation · nitidez de texturas, escalado de texturas y antialiasing de la PSP · nitidez de texturas y transparencias de la Dreamcast · Filtro NTSC · Líneas de escaneo · **TV de tubo (CRT)** · Mezcla de alta resolución · Reducir parpadeo de sprites · Efectos de transparencia · **Colores de Game Boy** · Colores de pantalla · Estela LCD |
| **Rendimiento** | Cuadros por segundo de la PSP (God of War) · Reloj de CPU de 3DS · Reloj de CPU de Dreamcast · DSP de sonido de Dreamcast · Reloj de CPU emulado · Reloj del SuperFX · sincronía de gráficos, núcleos de CPU, momento de la imagen, ritmo de CPU y ritmo de fotogramas de la N64 · Velocidad del disco PS1 · Velocidad de emulación · Avance rápido · Salto de fotogramas · Gráficas de rendimiento |
| **Emulación** | Región · Paleta de NES · Botones turbo · CPU, RSP y Controller Pak de la N64 · Logo de inicio (BIOS) · Luz solar (Boktai) · Sensor de inclinación · Mando de PS1 · Logo de inicio PS1 · Zona muerta y sensibilidad del stick de PSP · Pantalla táctil de DS y de 3DS · Modo New 3DS · Sombreado exacto y Ojo derecho de la 3DS · Mando de PC Engine · Permitir direcciones opuestas · Memoria aleatoria y más |
| **Sistema** | Volumen del juego · Interpolación de audio · Filtro de sonido · Búfer de audio · Multitap (3-5 jugadores) · Continuar donde lo dejaste |

Los juegos de Dreamcast, NAOMI y Atomiswave muestran solo los ajustes que les sirven, entre ellos las
cuatro calidades (hasta 3x).

## Archivos en la PS4

Todo está en `/data/EmuSuNi` (accesible por FTP con GoldHEN):

| Ruta | Contenido |
|---|---|
| `games/`, `nes/` | Las carpetas de juegos predeterminadas de Super Nintendo y NES |
| `covers/` | Carátulas: una carpeta por consola, con los nombres de las carpetas de juegos (`covers/snes`, `covers/nes`, `covers/md`...); PNG o JPEG |
| `saves/` | Partidas de batería y el reloj de los cartuchos (`.rtc`), las tarjetas de memoria de PlayStation, la Memory Stick de la PSP (`saves/psp`), las VMU de la Dreamcast (`saves/dc`), la memoria interna de la Saturn (`saves/saturn`) |
| `states/` | Estados guardados |
| `cheats/` | Trucos: `<nombre del archivo>.cht` (formato RetroArch), en la subcarpeta de su consola (`cheats/nes`, `cheats/ps1`...; los de Super Nintendo directamente en `cheats/`). Si un juego aún no tiene trucos, **Triángulo → Trucos** muestra la carpeta y el nombre exactos |
| `system/` | Archivos de BIOS (ver [BIOS](#bios)) |
| `cache/` | La colección de trucos de internet (se guarda un mes), las listas de los juegos de varios discos, las listas, imágenes y traducciones de RetroAchievements, las estrellas de valoración, las listas de The Bezel Project |
| `bezels/` | Tus marcos: PNG de 1920×1080 transparente donde va el juego, en `bezels/<consola>/`; los que descarga **Ajustes → Marcos de los juegos** también van ahí |
| `themes/` | Fondos (PNG, 1920×1080) y sonidos que reemplazan a los incluidos |
| `settings.ini`, `controls.ini`, `xmb.ini`, `favorites.ini` | Tus ajustes |
| `retroachievements.ini` | Tu sesión de RetroAchievements (su token, nunca la contraseña) |
| `emusuni.log` | El registro, para informar de errores (`emusuni-1.log` a `emusuni-4.log`: las sesiones anteriores) |

## Compilar

Se compila en **Ubuntu 24.04** (WSL2 sirve) con clang 21, el OpenOrbis de PacBrew y el runtime de
PS4 de DolphinPS4 (libc++ 21 y el controlador Vulkan RADV de Mesa).

```sh
# una vez: paquetes, toolchain, libc++, Mesa, los núcleos de todas las consolas y Dear ImGui (como root)
sudo -i bash /ruta/a/EmuSuNi/scripts/setup.sh

# luego, también como root (setup.sh deja las fuentes y el runtime de PS4 en la carpeta personal de root)
sudo -i
cd /ruta/a/EmuSuNi
EMUSUNI_VERSION=00.22 bash scripts/build.sh with-saturn   # compila ~/emusuni-build/emusuni_eboot/eboot.bin
bash scripts/package.sh 00.22                             # crea el .pkg en ~/emusuni-build/out
```

`scripts/build.sh` compila primero cada núcleo (`scripts/build-*-core.sh`, de nuevo solo lo que
cambió); `no-n64`, `no-ps1`, `no-nds`, `no-psp`, `no-dc`, `no-sega`, `no-arcade`, `no-3ds` o `no-pce`
dejan fuera una consola, y `with-saturn` incluye la Saturn (las versiones publicadas la llevan).
`scripts/make-assets.py` vuelve a crear el icono, las imágenes de inicio, el logo y los sonidos de 8 bits
(Python + Pillow); `scripts/make-bezels.py`, los marcos.

## Créditos

- Los emuladores: **[Snes9x](https://github.com/snes9xgit/snes9x)** (Super Nintendo),
  **[FCEUmm](https://github.com/libretro/libretro-fceumm)** (NES),
  **[Mupen64Plus-Next](https://github.com/libretro/mupen64plus-libretro-nx)** con ParaLLEl-RDP y
  ParaLLEl-RSP (Nintendo 64), **[mGBA](https://mgba.io)** (Game Boy, Color y Advance),
  **[SwanStation](https://github.com/libretro/swanstation)**, basado en
  [DuckStation](https://github.com/stenzek/duckstation) (PlayStation),
  **[melonDS DS](https://github.com/JesseTG/melonds-ds)**, basado en
  [melonDS](https://melonds.kuribo64.net) (Nintendo DS), **[Azahar](https://azahar-emu.org)**
  (Nintendo 3DS), **[PPSSPP](https://www.ppsspp.org)** (PSP),
  **[Flycast](https://github.com/flyinghead/flycast)** (Dreamcast, NAOMI y Atomiswave; con partes de la
  memoria rápida de [Resonance](https://github.com/iHaiDeeZ/Resonance)),
  **[Genesis Plus GX](https://github.com/libretro/Genesis-Plus-GX)** (SG-1000, Master System, Game Gear
  y Genesis), **[YabaSanshiro](https://github.com/libretro/yabause/tree/yabasanshiro)** (Saturn),
  **[Beetle PCE Fast](https://github.com/libretro/beetle-pce-fast-libretro)**, basado en
  [Mednafen](https://mednafen.github.io) (PC Engine) y **[FinalBurn Neo](https://github.com/finalburnneo/FBNeo)**
  (Arcade).
- **[RetroAchievements](https://retroachievements.org)** y su biblioteca rcheevos, las valoraciones de la
  **[LaunchBox Games Database](https://gamesdb.launchbox-app.com)** y los marcos de
  **[The Bezel Project](https://github.com/thebezelproject)**, que descarga la app.
- **[Anime4K](https://github.com/bloc97/Anime4K)**, **AMD FidelityFX Super Resolution**,
  **[SMAA](https://github.com/iryoku/smaa)** y **MMPX** — la imagen HD con IA, el escalado suave y
  los bordes limpios.
- **[DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)** — el diseño del lanzador, el runtime de
  PS4, el port de Mesa y la configuración de compilación de los que nació EmuSuNi.
- **[Dear ImGui](https://github.com/ocornut/imgui)**, **[Mesa](https://mesa3d.org)**,
  **[OpenOrbis](https://github.com/OpenOrbis) / [PacBrew](https://github.com/PacBrew)**,
  **[libretro-thumbnails](https://github.com/libretro-thumbnails)**, la colección de trucos de
  libretro, xBRZ, HQx, el filtro NTSC de blargg y los autores de las fuentes. Ver
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

EmuSuNi por [@arturofuentes1988](https://github.com/arturofuentes1988).

## Licencia y aviso legal

El código propio de EmuSuNi tiene licencia **GNU GPL v2 o posterior** ([LICENSE](LICENSE)). Snes9x,
Genesis Plus GX y FinalBurn Neo tienen sus propias licencias, que solo permiten el uso gratuito y **no
comercial**; los paquetes de EmuSuNi son gratuitos, y EmuSuNi no pide donaciones. Los demás emuladores y
componentes mantienen sus licencias ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

**No incluye juegos ni BIOS.** Usa juegos que te pertenezcan. EmuSuNi no está afiliado ni respaldado
por Nintendo, Sony, Sega ni ningún otro fabricante de estas consolas y juegos. *Super Nintendo*,
*Super Famicom*, *SNES*, *NES*, *Nintendo 64*, *Game Boy*, *Game Boy Color*, *Game Boy Advance*,
*Nintendo DS* y *Nintendo 3DS* son marcas de Nintendo; *PlayStation*, *PSP* y *PS4* son marcas de Sony
Interactive Entertainment; *Dreamcast*, *Saturn*, *Genesis*, *Mega Drive*, *Master System*, *Game Gear*
y *NAOMI* son marcas de Sega; los demás nombres son marcas de sus dueños.
