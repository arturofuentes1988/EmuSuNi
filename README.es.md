<p align="center">
  <img src="docs/banner.png" alt="EmuSuNi" width="860">
</p>

<p align="center">
  <b>Veintitrés consolas clásicas y placas arcade, y los juegos de OpenBOR, en una sola app para PS4 con jailbreak.</b><br>
  Super Nintendo · NES · Nintendo 64 · Game Boy · Game Boy Color · Game Boy Advance · PlayStation ·
  Nintendo DS · Nintendo 3DS · PSP · Dreamcast · NAOMI · Atomiswave · SG-1000 · Master System ·
  Game Gear · Genesis · Saturn · PC Engine · Atari 2600 · Atari 7800 · Atari Jaguar · Arcade · OpenBOR
</p>

<p align="center">
  Un lanzador que se siente como el de una consola: tus juegos con sus carátulas, imagen HD con IA,
  juego online con tus amigos, trucos de internet, RetroAchievements, estados guardados, marcos y ajustes
  para cada juego.
</p>

<p align="center">
  <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest"><img alt="Descargar" src="https://img.shields.io/github/v/release/arturofuentes1988/EmuSuNi?label=descargar&style=for-the-badge&color=7b4dff"></a>
  <img alt="PS4 con GoldHEN" src="https://img.shields.io/badge/PS4-GoldHEN-003791?style=for-the-badge&logo=playstation&logoColor=white">
  <img alt="23 consolas" src="https://img.shields.io/badge/consolas-23-e5484d?style=for-the-badge">
  <a href="LICENSE"><img alt="Licencia GPL-2.0+" src="https://img.shields.io/badge/licencia-GPL--2.0%2B-3db46d?style=for-the-badge"></a>
</p>

<p align="center"><a href="README.md">Read in English</a> · <a href="CHANGELOG.md">Historial de cambios</a> · <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest">Descargar</a> · <a href="https://t.me/+A53DraC3it5mZTcx">Telegram</a></p>

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
   (PC Engine), `atari2600`, `atari7800`, `atarijaguar`, `arcade` y `openbor` (los juegos `.pak` de OpenBOR). Los
   juegos de Arcade, NAOMI y Atomiswave se quedan en su `.zip` ([formatos](#las-consolas)).
3. **BIOS**: solo la necesitan los juegos de Neo Geo del Arcade (`neogeo.zip`), NAOMI (`naomi.zip`,
   `naomi2.zip`), Atomiswave (`awbios.zip`) y la Saturn (un `.bin` de 512 KB, como `saturn_bios.bin`), y
   los del Famicom Disk System (`disksys.rom`) ([BIOS](#bios)). No se incluyen juegos ni BIOS.
4. **Juega**: tus juegos aparecen solos, con su carátula si la PS4 tiene internet. En el juego,
   **L3 + R3** abre el menú: estados guardados, ajustes, controles, salir.
5. **Ayuda y errores**: el [grupo de Telegram](https://t.me/+A53DraC3it5mZTcx), o una
   [incidencia](https://github.com/arturofuentes1988/EmuSuNi/issues) con tu `/data/EmuSuNi/emusuni.log`.
   **Ajustes → Enviar registro** manda el registro desde la app y te da un código para escribir en tu reporte.

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
| **Saturn** — *experimental* | YabaSanshiro 3.4.2 | `.cue` `.chd` `.iso` `.ccd` `.mds` | **La tuya**: un `.bin` de 512 KB ([BIOS](#bios)) |
| **PC Engine** / TurboGrafx-16 | Beetle PCE Fast | `.pce` (HuCards) | No hace falta |
| **Atari 2600** — *nueva* | Stella | `.a26` `.bin` | No hace falta |
| **Atari 7800** — *nueva* | ProSystem | `.a78` | No hace falta |
| **Atari Jaguar** — *experimental, nueva* | Virtual Jaguar 3.11 | `.j64` `.jag` `.abs` `.cof` | No hace falta |
| **Arcade**: Neo Geo, Capcom CPS1/CPS2/CPS3, Sega, Konami, Taito... | FinalBurn Neo 1.0.0.03 | Un `.zip` / `.7z` por juego, sin descomprimir | Neo Geo: **la tuya**, `neogeo.zip` |
| **OpenBOR**: los beat 'em up que hacen sus fans — *nuevo* | OpenBOR v3.0 (Build 6391) y v4.0 (Build 8025) | `.pak` | No hace falta |

No se incluyen BIOS ni juegos. Solo los juegos de Neo Geo del Arcade, NAOMI, Atomiswave y la Saturn
necesitan una BIOS tuya ([BIOS](#bios)).

La **Saturn** es experimental: dibuja por software, en hilos propios, y sus juegos pueden ir más lentos que
en la consola. La **Nintendo 3DS** sigue siendo experimental: solo juegos descifrados; si un juego no carga a
la primera, **L3 + R3 → Reiniciar juego** hasta que cargue. La PC Engine juega HuCards (no juegos en CD).
La **Atari Jaguar** también es experimental: la mayoría de los juegos probados van bien, algunos (el 3D de Iron Soldier)
todavía van más lentos. **OpenBOR** juega los beat 'em up gratis que hacen sus fans, cada uno un archivo `.pak`: EmuSuNi no
trae ninguno, pon los tuyos en `Roms/openbor` ([Dónde poner los juegos](#dónde-poner-los-juegos)). No tienen estados
guardados, trucos ni carátulas.

## Novedades de la v00.23

- **Juego online con tus amigos**: juega un mismo juego con un amigo desde cualquier lugar del mundo, como si los
  dos mandos estuvieran en una consola. Abre la pestaña **En línea**, crea una sala (un código de 6 letras para tu
  amigo, o una sala pública, de 2 a 4 jugadores) o únete a una. Funciona en NES, Super Nintendo, Genesis, Master
  System, Game Gear, SG-1000, PC Engine, Arcade, Atari 2600 y Atari 7800, y la Game Boy y la Game Boy Color juegan
  con su **cable link** (**Triángulo → Cable link en línea**). También puedes **Elegir juntos**: la app muestra los
  juegos que tienen los dos. Mira [Juego online](#juego-online).
- **Chat de voz** mientras juegas online, con silencio para cada jugador (**Ajustes → Chat de voz**), y una barra
  de latencia con el ping de cada uno.
- **PSP, Dreamcast y DS online** por los servidores gratis de la comunidad: los juegos de PSP cuyos servidores
  revivieron los fans y el modo ad hoc, DCNet en la Dreamcast y Wiimmfi en la DS
  ([cómo](#online-con-los-servidores-de-la-comunidad-psp-dreamcast-ds)).
- **Consolas nuevas**: la **Atari 2600**, la **Atari 7800** y la **Atari Jaguar** (experimental), y los juegos de
  **OpenBOR** (tus propios `.pak` en `Roms/openbor`).
- **Super Smash Bros. de 3DS** se ve bien: sus luchadores ya no se rompen en la pelea. La 3DS además tiene muchos
  menos tirones la primera vez que se juega un juego.
- **3 y 4 jugadores en una PS4**: cada DualShock 4 extra con la sesión iniciada en un usuario de la PS4 es el
  siguiente jugador, y el adaptador multijugador de cada consola se conecta solo (**Adaptador multijugador**).
- **Modo portátil**: lleva tus partidas, estados, carátulas y ajustes en una memoria USB y juega con ellos en
  otra PS4 con EmuSuNi ([Modo portátil](#modo-portátil)).
- **Ultra desde el principio**: todas las consolas empiezan en Calidad Ultra (la 3DS en Media) y la DS con las
  pantallas lado a lado. Al actualizar, las consolas cuya calidad nunca elegiste pasan a los niveles nuevos, una vez.
- **Estadísticas anónimas**, **activadas por defecto**: un pequeño latido nos dice cuánta gente usa EmuSuNi,
  desde qué países y con qué versiones, nunca quién. **Ajustes → Estadísticas anónimas → No** no envía nada
  ([detalles](#estadísticas-anónimas)). También hay una [página web](https://emusuni.com)
  con los números en vivo.
- **Ajustes → Enviar registro**: el registro va al servidor de EmuSuNi y recibes un código para tu reporte en GitHub
  o Telegram. Nunca se envían tus juegos ni tus partidas; el servidor guarda solo el país, nunca tu dirección, y
  borra el registro a los 90 días.
- **Bibliotecas grandes**: la app abre al instante sea cual sea el número de juegos (no abría con más de 10 000).
- **Controles**: macros (un botón que pulsa varios), sticks asignables, y **Ajustes → Controles → Menú del juego**
  también tiene R3, L1 + R1 y L2 + R2 pulsados (sin mantener).
- **Tamaño del menú** (100 %, 90 %, 80 %, 70 %): más juegos a la vista.
- **Los estados guardados tienen su imagen** en todas las consolas (N64, PlayStation, PSP, Dreamcast, NAOMI,
  Atomiswave y 3DS no la tenían).
- **Mis estadísticas**: tu tiempo jugado por juego, solo en tu PS4.
- **Arreglos**: la Jaguar mucho más rápida con su recompilador nuevo; la PlayStation con menos pausas en las cargas,
  y Metal Gear Solid europeo, Tekken 3 y Driver 2 vuelven a funcionar; un disco de Dreamcast dañado ya no cierra la
  app y Sega Rally 2 arranca; la PSP guarda en una PS5; guardar un estado en la Saturn ya no cierra la app; un juego
  de 3DS que falla termina el juego, no EmuSuNi.

Todos los detalles: las [notas de la versión](https://github.com/arturofuentes1988/EmuSuNi/releases/latest) y el [historial de cambios](CHANGELOG.md).

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
- Todas las consolas parten con Calidad Baja, Relación de aspecto Auto y su propio marco (desde la 00.23: Ultra, la
  3DS en Media).

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

**Juntos**
- **[Juego online](#juego-online)** por internet, con rollback (como en los juegos de lucha) y chat de voz, para
  NES, Super Nintendo, Genesis, Master System, Game Gear, SG-1000, PC Engine, Arcade, Atari 2600 y Atari 7800, y el
  cable link de Game Boy; la PSP, la Dreamcast y la DS online por los servidores de la comunidad.
- **[Hasta 4 jugadores](#jugar-juntos-hasta-4-jugadores)** en una PS4, con el multitap que corresponde a cada consola.
- **[Modo portátil](#modo-portátil)**: tus datos en una memoria USB, para jugar en la PS4 de un amigo.

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
  | Atari 2600 | `.a26`, o un `.bin` de 64 KB o menos |
  | Atari 7800 | `.a78` (con su cabecera) |
  | Atari Jaguar (experimental) | `.j64` `.jag` `.abs` `.cof` |
  | Arcade | Sets de FinalBurn Neo 1.0.0.03: un `.zip` / `.7z` por juego, sin descomprimir (`mslug.zip`) |
  | OpenBOR | `.pak`, tal como lo da su autor (sin comprimir) |

  Las consolas de cartucho (de Super Nintendo a Game Boy Advance, de SG-1000 a Genesis, la PC Engine y las Atari)
  también aceptan `.zip`; las de disco, la DS, la 3DS y OpenBOR leen sus juegos tal cual. El `.zip` de un juego de
  Arcade, NAOMI o Atomiswave es el juego en sí: déjalo comprimido y con el nombre de su set.
- Internet en la PS4 para las carátulas, los trucos de internet y el juego online (opcional).

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
`atomiswave`, `sg1000`, `sms`, `gg`, `md` (Genesis), `saturn`, `pce` (PC Engine), `atari2600`, `atari7800`,
`atarijaguar`, `arcade` y `openbor`.

**Los juegos de OpenBOR** van en `openbor`: `Roms/openbor` en la memoria USB (`/mnt/usb0/Roms/openbor` por FTP) o
`/data/EmuSuNi/Roms/openbor` en la PS4. Copia ahí el `.pak` de cada juego tal cual (sin comprimir); la app elige la
versión de OpenBOR para la que se hizo. EmuSuNi no trae ninguno: son juegos gratis hechos por los fans de OpenBOR.

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

Las demás consolas funcionan sin BIOS, la PC Engine y las Atari incluidas. Si tienes las reales, se usan: PlayStation
(`scph5501.bin` u otra, más juegos funcionan), Nintendo DS (`bios7.bin`, `bios9.bin`, `firmware.bin`), Atari 7800
(`7800 BIOS (U).rom`) y Atari Jaguar (`jagboot.rom`, su animación de arranque) en `/data/EmuSuNi/system`, Dreamcast
(`dc_boot.bin`) en `/data/EmuSuNi/system/dc`.

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
Controles → Menú del juego** añade R3, L1 + R1 o L2 + R2 pulsados una vez, o el panel táctil mantenido, Options + panel táctil,
L1 + R1 o L2 + R2 mantenidos). Todos los
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

| DualShock 4 | Atari 2600 | Atari 7800 | Atari Jaguar | OpenBOR |
|---|---|---|---|---|
| Cruz | Disparo | Botón 1 | B | Saltar |
| Círculo | Gatillo del Booster Grip | Botón 2 | A | Especial |
| Cuadrado | Tipo de TV: color | — | C | Ataque |
| Triángulo | Tipo de TV: B/N | RESET | OPTION | Ataque 2 |
| L1 / R1 | Dificultad izquierda / derecha A | Dificultad izquierda / derecha B (mantenido) | Teclado numérico 1 / 2 | Ataque 3 / 4 |
| L2 / R2 | Dificultad izquierda / derecha B | — / Avance rápido | Teclado numérico 3 / 4 | — / Avance rápido |
| Options | RESET | PAUSE | PAUSE | START |
| Panel táctil | SELECT | SELECT | Las 12 teclas del teclado numérico | SELECT (volver) |
| Stick izquierdo | Joystick | Joystick | Cruceta | Cruceta |

| Menú (estilo Biblioteca) | |
|---|---|
| Cruceta izquierda / derecha | Juego anterior / siguiente (arriba / abajo: de 10 en 10) |
| L1 / R1 | Pestañas: Todos, cada consola, Favoritos, Ajustes (mantenidos: las recorren) |
| Cruz | Jugar |
| Triángulo | Opciones del juego: ajustes del juego, jugar online, trucos, controles, cambiar nombre, buscar carátula, logros, información, juego predeterminado, borrar |
| L2 / R2 | Favorito / buscar |
| Cuadrado | Buscar los juegos de nuevo |
| Panel táctil | Filtro RA: todos los juegos, los que tienen logros, los que están en curso |
| Options | Ordenar |
| Círculo | Volver |

Mantén **L1** al abrir EmuSuNi para ir al menú en vez del juego predeterminado.

### Jugar juntos (hasta 4 jugadores)

Cada jugador más necesita un DualShock 4 **con la sesión iniciada en un usuario de la PS4**: pulsa el botón PS del
mando nuevo y elige quién lo usa (vale un usuario local nuevo; no necesita cuenta de PSN). El jugador 1 es el
usuario que abrió EmuSuNi; los demás son los jugadores 2, 3 y 4 en el orden en que inician sesión, y cada uno
conserva su número mientras siga con la sesión iniciada. La PS4 admite 4 usuarios a la vez, así que 4 jugadores
como máximo. La barra de luz de cada mando muestra su jugador (azul, rojo, verde, rosa) y un mando que se conecta o
se desconecta durante el juego se avisa en pantalla. Todos los jugadores tienen los mismos botones (Controles); el
menú del juego, el avance rápido y las pantallas táctiles son del jugador 1.

| Consola | Jugadores | Cómo |
|---|---|---|
| Super Nintendo | 3-5 | Super Multitap en el puerto 2 |
| NES | 3-4 | Four Score; en los juegos de Famicom, el adaptador de 4 jugadores de Famicom |
| Genesis | 3-5 | Team Player en el puerto 1 (y un mando en el 2), el 4 Way Play de EA en los juegos de EA; los juegos J-Cart (Micro Machines 2, Pete Sampras...) traen los puertos extra en el cartucho |
| PlayStation | 3-5 | Multitap en el puerto 2 |
| Saturn | 3-6 | Adaptador de 6 jugadores (solo con **Sí**) |
| PC Engine | 3-5 | TurboTap, siempre conectado |
| N64, Dreamcast, NAOMI, Atomiswave, Arcade | 3-4 | los cuatro puertos de la propia consola |
| OpenBOR | 3-4 | los jugadores del propio juego (hasta 4) |

**Adaptador multijugador** (en los ajustes de un juego, *Sistema*) elige el adaptador: **Automático** (el valor
inicial) lo conecta solo en los juegos que se sabe que lo usan - la lista del propio emulador (la NES, el J-Cart),
la cabecera del juego (la Genesis), listas de juegos del adaptador (Super Bomberman, Secret of Mana, NHL '94, Crash
Team Racing...) -, así que los juegos de 1-2 jugadores funcionan exactamente igual que antes; **No**, nunca; **Sí**,
siempre, para un juego que Automático no conoce (un juego hecho sin el adaptador puede leer mal los mandos).

## Juego online

Dos PS4 en cualquier lugar del mundo juegan un mismo juego como si los dos mandos estuvieran en una consola.
EmuSuNi usa su propio sistema online (no el de RetroArch) con **rollback**, la técnica de los juegos de lucha: el
juego sigue sin esperar y se corrige solo si el botón del otro jugador llega tarde.

- **Consolas**: NES, Super Nintendo, Genesis, Master System, Game Gear, SG-1000, PC Engine, Arcade, Atari 2600 y
  Atari 7800 (las más ligeras). Llevan un pequeño globo en el menú.
- **La pestaña En línea** lista todas las salas con su juego, consola, anfitrión, jugadores, tipo y ping.
  **Cruz** se une, **Cuadrado** crea una sala, **Triángulo** se une con un código, **L2 / R2** filtran.
- **Salas**: **Triángulo → Jugar online** en un juego crea una **sala privada** (un código de 6 letras para tu
  amigo) o una **sala pública** (en la lista de todos), de **2, 3 o 4 jugadores** (**Jugadores**, en las consolas
  que los tienen). **Elegir juntos** crea una sala sin juego: cuando entra tu amigo, los dos ven, consola por
  consola, los juegos que tienen los dos.
- **El cable link de Game Boy**: en un juego de Game Boy o Game Boy Color, **Triángulo → Cable link en línea** crea
  una sala para dos; tu amigo elige su propio juego (**Elegir tu juego**) y pueden intercambiar o pelear como con
  un cable de verdad. Cada PS4 muestra su propia Game Boy y guarda su propia partida.
- **Las dos PS4 necesitan la misma versión de EmuSuNi y el mismo archivo del juego** (la app los compara y te dice
  si tienes otra versión).
- **Tu sala sigue abierta** mientras usas los menús (**Círculo** vuelve al menú; **Cerrar sala** la cierra).
  Cuando alguien pide entrar, un aviso te deja **aceptarlo** o **rechazarlo**.
- **Chat de voz** (**Ajustes → Chat de voz**): el micrófono del usuario que juega (unos audífonos en el
  DualShock 4, un micrófono USB o la PlayStation Camera). Micrófono abierto o pulsar para hablar, volúmenes y
  **Probar micrófono**. Empieza **apagado en las salas públicas** (cada jugador activa el suyo) y puedes silenciar a
  cualquier jugador.
- **La conexión** es directa entre las dos PS4 cuando se puede, y pasa por un servidor de relevo cuando no.
  **Ocultar mi dirección** usa siempre el relevo, así el otro jugador nunca sabe tu dirección.
- **Apodo y perfil**: tu apodo es el del usuario de la PS4 salvo que escribas otro (pestaña En línea → Ajustes
  online). El **Perfil online** es opcional y viene apagado ([detalles](#mis-estadísticas-y-el-perfil-online)).
- **Barra de latencia**: el ping de cada jugador en pantalla (verde, ámbar, rojo); se puede apagar en los ajustes
  online o en el menú del juego.
- Mientras juegas online no se pueden guardar ni cargar estados, rebobinar, usar trucos ni pausar: la partida sigue.
  En Arcade, las tablas de récords se apagan online (las tuyas quedan como estaban).

### Online con los servidores de la comunidad: PSP, Dreamcast, DS

Estas consolas juegan online como lo hacían las de verdad, por servidores gratis que mantienen los fans (no los de
EmuSuNi). Cada una es un **ajuste del juego** (**Triángulo → Ajustes del juego → Emulación**, o Ajustes en el menú
del juego), y no se envía nada hasta que el propio juego se conecta. Como con la consola real, esos servidores ven
tu dirección IP pública.

| Consola | Ajuste | Qué juega |
|---|---|---|
| **PSP** | **Juego en línea de PSP** (viene en No) y **Servidor ad hoc de PSP** | Los juegos cuyos servidores revivieron los fans (MotorStorm Arctic Edge, WipEout Pulse, FIFA, SOCOM...), desde el menú de red del propio juego, y el modo ad hoc (Monster Hunter, Tekken 6, Dissidia...) por el relevo del servidor ad hoc elegido: los que juegan juntos eligen el mismo servidor. Tu nombre en los juegos es tu apodo online |
| **Dreamcast** | **Dreamcast en línea**: No (por defecto), DCNet (módem) o DCNet (banda ancha) | DCNet, los servidores gratis de Flycast: ChuChu Rocket! y los demás juegos de módem; la banda ancha, para los juegos hechos para ella. Algunos juegos piden un nombre la primera vez y lo guardan en la VMU |
| **Nintendo DS** | **Wi-Fi de DS**: Wiimmfi (por defecto), WiiLink WFC o No | La Nintendo Wi-Fi Connection de los juegos (Mario Kart DS, Pokémon...). Los amigos registrados en un servidor no están en el otro |

## Modo portátil

Lleva las partidas, estados, trucos, carátulas, marcos, temas, ajustes, controles y la lista de juegos en una
memoria USB (pendrive) y juega con ellos en otra PS4 que tenga EmuSuNi (la de un amigo, por ejemplo).

- **Ajustes → Guardar mis datos en la memoria USB → Activar** copia tus datos a una carpeta `EmuSuNi-Portable` en
  la raíz de la memoria y reinicia la app. No se mueve ni se borra nada de tu PS4.
- **En otra PS4** con EmuSuNi, al enchufar la memoria pregunta una vez "¿Usar los datos de esta memoria?". Sus
  propios datos no se tocan y vuelven cuando quitas la memoria.
- **No se copian**: el inicio de sesión de RetroAchievements, el perfil online y el apodo, el número de las
  estadísticas, Mis estadísticas, los registros y las BIOS (si quieres una BIOS en la memoria, ponla tú en
  `EmuSuNi-Portable/system`).
- Si sacas la memoria mientras juegas, la app lo avisa y guarda lo que grabes en `/data/EmuSuNi/portable-fallback`
  (nunca encima de las partidas de la PS4); la próxima vez que esté la memoria, se copia de vuelta.
- **Desactivar** ofrece copiar los datos de la memoria de vuelta a la PS4.

## Preguntas frecuentes

**¿Pierdo mis juegos y partidas al actualizar?** No. Todo vive en `/data/EmuSuNi` (o en tu memoria en el modo
portátil) y una versión nueva no lo toca. Ni siquiera borrar y reinstalar la app lo toca.

**¿Por qué algunos juegos no tienen carátula?** La app descarga las carátulas por el nombre del juego desde las
miniaturas de libretro: la PS4 necesita internet y el archivo debe llamarse como se conoce el juego (estilo
No-Intro: `Super Mario World (USA).sfc`). Si no, pon tu propio PNG o JPEG con el nombre del juego en
`covers/<consola>`, o usa **Triángulo → Buscar carátula**. **Ajustes → Juegos sin carátula** dice por qué no se
usa una imagen. Mira [Carátulas](#carátulas).

**¿Qué juegos de 3DS funcionan?** Solo los **descifrados** (`.3ds`, `.cci`, `.cxi`, `.3dsx`): los cifrados y los
`.cia` no cargan. La 3DS es experimental y la consola más pesada: una PS4 Pro la mueve mejor.

**Mi juego de Arcade no arranca.** EmuSuNi usa sets de **FinalBurn Neo 1.0.0.03**: un `.zip` por juego, sin
descomprimir, con el nombre del set en minúsculas (`mslug.zip`). Un set de otra versión de MAME o FinalBurn puede
tener otros archivos y no arrancar. Los juegos de Neo Geo necesitan además tu `neogeo.zip` al lado. NAOMI y
Atomiswave necesitan `naomi.zip` / `awbios.zip`. Mira [BIOS](#bios).

**¿Cómo juego la Saturn?** Pon tu BIOS de Saturn (un `.bin` de 512 KB) en `/data/EmuSuNi/system/saturn` y los
juegos (`.cue` con sus pistas `.bin`, o `.chd`) en `Roms/saturn`. Es experimental: dibuja por software y algunos
juegos van más lentos que en la consola.

**¿Cómo juego los juegos de OpenBOR?** Los juegos de OpenBOR son beat 'em up gratis hechos por sus fans, cada uno un
archivo `.pak`. EmuSuNi no trae ninguno: copia el `.pak` de un juego que tengas, tal cual, a `Roms/openbor` del
pendrive o a `/data/EmuSuNi/Roms/openbor`, y aparece en la pestaña **OpenBOR**. EmuSuNi juega los juegos hechos para
OpenBOR 3.0 (Build 6391, la mayoría) y 4.0 (Build 8025).

**¿Funciona en PS5?** EmuSuNi se hace y se prueba para la PS4 con jailbreak. Algunos jugadores la usan en PS5 por
retrocompatibilidad, pero no está probado ni soportado.

**¿Cómo se instala?** Copia el `.pkg` a un pendrive (FAT32 o exFAT) e instálalo con el **Package Installer** de
GoldHEN ([Instalación](#instalación)). Luego pon tus juegos en `Roms/<consola>` del pendrive.

**¿Envía algo sobre mí?** Solo las [estadísticas anónimas](#estadísticas-anónimas), que puedes apagar en **Ajustes →
Estadísticas anónimas**, y tu registro cuando eliges **Ajustes → Enviar registro**. Nunca se envía nada de tus
archivos, nombres ni de tu PS4.

## Ajustes

**Ajustes**, en el menú, tiene las opciones de la app: Temas, Idioma, Controles, RetroAchievements,
Carpetas de juegos, Orden de las consolas, Consolas mostradas, Limpiar juegos, Descargar carátulas,
Juegos sin carátula, Marcos de los juegos, Estrellas de valoración, Efectos de sonido, Estilo del menú, **Tamaño del menú**,
Reloj, **Chat de voz**, **Juego online**, **Mis estadísticas**, **Estadísticas anónimas**, **Guardar mis datos en la memoria USB**,
Restablecer todos los ajustes, Buscar actualizaciones, **Enviar registro**, **Cómo ayudar**...

Los ajustes de un juego se cambian desde el juego: **Triángulo → Ajustes del juego** en el menú, o
**Ajustes** en el menú del juego. Cada uno puede aplicarse a **todos los juegos**, a **una
consola** o a **un juego** (*Aplicar a*, arriba), y un texto breve bajo la lista explica el ajuste y
su opción actual.

| Categoría | Ajustes |
|---|---|
| **Vídeo** | **Calidad** (Baja, Media, Alta, Ultra) · Resolución · Resolución de N64, de PS1, de PSP, de Dreamcast y de 3DS · Relación de aspecto · Disposición de pantallas, Cambiar pantallas, Tamaño y Posición de la pantalla pequeña y Separación entre pantallas de la DS y la 3DS · Pantalla panorámica PS1 · Tamaño de imagen · Zoom panorámico · **Marco** · Recortar overscan · Límite de fotogramas · V-Sync · Contador de FPS |
| **Gráficos** | Filtro de escalado · **Escalado por GPU** · Filtrado de texturas · suavizado de bordes, tramado, 2D, texturas lejanas y efectos de pantalla de la N64 · geometría (PGXP), filtrado de texturas, colores y antialiasing de la PlayStation · nitidez de texturas, escalado de texturas y antialiasing de la PSP · nitidez de texturas y transparencias de la Dreamcast · Fósforo de la 2600 · Filtro NTSC · Líneas de escaneo · **TV de tubo (CRT)** · Mezcla de alta resolución · Reducir parpadeo de sprites · Efectos de transparencia · **Colores de Game Boy** · Colores de pantalla · Estela LCD |
| **Rendimiento** | Cuadros por segundo de la PSP (God of War) · Reloj de CPU de 3DS · Reloj de CPU de Dreamcast · DSP de sonido de Dreamcast · **Recompilador Jaguar** y Velocidad GPU/DSP de Jaguar · Reloj de CPU emulado · Reloj del SuperFX · sincronía de gráficos, núcleos de CPU, momento de la imagen, ritmo de CPU y ritmo de fotogramas de la N64 · Velocidad del disco PS1 · Velocidad de emulación · Avance rápido · Salto de fotogramas · Gráficas de rendimiento |
| **Emulación** | Región · Paleta de NES · Botones turbo · CPU, RSP y Controller Pak de la N64 · Logo de inicio (BIOS) · Luz solar (Boktai) · Sensor de inclinación · Mando de PS1 · Logo de inicio PS1 · Zona muerta y sensibilidad del stick de PSP · Pantalla táctil de DS y de 3DS · Modo New 3DS · Sombreado exacto y Ojo derecho de la 3DS · Mando de PC Engine · Idioma de Dreamcast · BIOS de Jaguar · **Juego en línea de PSP**, Servidor ad hoc de PSP, **Dreamcast en línea** y **Wi-Fi de DS** ([online](#online-con-los-servidores-de-la-comunidad-psp-dreamcast-ds)) · Permitir direcciones opuestas · Memoria aleatoria y más |
| **Sistema** | Volumen del juego · Interpolación de audio · Filtro de sonido · Búfer de audio · **Adaptador multijugador** · Continuar donde lo dejaste |

Los juegos de Dreamcast, NAOMI y Atomiswave muestran solo los ajustes que les sirven, entre ellos las
cuatro calidades (hasta 3x).

### Estadísticas anónimas

Para saber cuántas personas usan EmuSuNi, desde dónde y con qué versión, la app envía un pequeño
latido anónimo al servidor en línea de EmuSuNi. Se envía al abrir la app, cada 5 minutos mientras está
abierta y después de buscar actualizaciones. También da a Juego online su número de jugadores
conectados ahora. Viene **activado por defecto**: **Ajustes → Estadísticas anónimas → No** lo apaga, y entonces no se envía nada.

- **Qué se envía**: un número aleatorio que la app crea una vez (`stats.ini` — no es el identificador
  de la PS4, de un usuario ni de PSN), la versión de la app, el idioma de la PS4, qué hace la app (los
  menús, un juego de qué consola, el juego en línea), **el juego al que se juega** — solo como suma de
  control del cartucho, nombre corto del set arcade o código del disco, nunca el nombre del archivo —,
  cuántas búsquedas de actualizaciones desde el último latido y, una vez tras actualizar, la versión
  anterior.
- **Qué no se envía**: ni nombres, ni nombres de archivo ni la lista de juegos, ni archivos ni partidas,
  ni identificadores de la PS4, de usuarios ni de PSN.
- **Qué guarda el servidor**: recuentos por país, versión, idioma y consola. El país sale de la
  dirección de la conexión, consultada al llegar el latido (la base de datos gratuita de DB-IP); la
  dirección nunca se guarda ni se registra. Los latidos de un día cuentan una vez por instalación mediante un hash del número,
  con una sal que se borra al acabar el día; los totales de semanas y meses salen de un resumen
  estadístico (HyperLogLog) del que no se puede recuperar el número de ninguna instalación.
- **Qué es público**: [la página de EmuSuNi](https://emusuni.com) muestra solo
  totales (jugadores conectados ahora, países, versiones, consolas, los juegos más jugados, las salas
  públicas sin apodos ni códigos). El título de un juego sale de listas públicas de juegos (No-Intro,
  Redump, FinalBurn Neo), nunca de la app, y solo se muestra cuando 3 o más instalaciones lo han jugado.

### Mis estadísticas y el perfil online

- **Mis estadísticas** (Ajustes → Mis estadísticas, también en la pestaña En línea): tu tiempo jugado
  por juego y consola, sesiones, último juego, partidas online y datos de RetroAchievements. Se
  guardan solo en tu PS4 (`mystats.ini`), nunca se envían.
- **Perfil online** (la pestaña **En línea**; **desactivado** salvo que lo actives): el servidor de
  EmuSuNi guarda el historial de esta PS4 — minutos, sesiones y partidas online por juego y consola,
  su país — bajo un hash de su número aleatorio, y te da un **código de perfil** que lo abre en
  **Estadísticas → Mi perfil** de la página. Sin el código nadie puede encontrarlo ni leerlo. **Perfil
  público** (desactivado salvo que lo actives) además pone tu apodo, país y horas en las
  clasificaciones de la página. **Borrar mi perfil** lo borra todo en el servidor (incluso con las
  estadísticas desactivadas); un perfil sin usar 400 días se borra solo. Sin perfil online, no se
  guarda nada por PS4.

## Archivos en la PS4

Todo está en `/data/EmuSuNi` (accesible por FTP con GoldHEN):

| Ruta | Contenido |
|---|---|
| `games/`, `nes/` | Las carpetas de juegos predeterminadas de Super Nintendo y NES |
| `covers/` | Carátulas: una carpeta por consola, con los nombres de las carpetas de juegos (`covers/snes`, `covers/nes`, `covers/md`...); PNG o JPEG |
| `saves/` | Partidas de batería y el reloj de los cartuchos (`.rtc`), las tarjetas de memoria de PlayStation, la Memory Stick de la PSP (`saves/psp`), las VMU de la Dreamcast (`saves/dc`), la memoria interna de la Saturn (`saves/saturn`), las partidas y récords de los juegos de OpenBOR (`saves/openbor`) |
| `states/` | Estados guardados |
| `cheats/` | Trucos: `<nombre del archivo>.cht` (formato RetroArch), en la subcarpeta de su consola (`cheats/nes`, `cheats/ps1`...; los de Super Nintendo directamente en `cheats/`). Si un juego aún no tiene trucos, **Triángulo → Trucos** muestra la carpeta y el nombre exactos |
| `system/` | Archivos de BIOS (ver [BIOS](#bios)) |
| `cache/` | La colección de trucos de internet (se guarda un mes), las listas de los juegos de varios discos, las listas, imágenes y traducciones de RetroAchievements, las estrellas de valoración, las listas de The Bezel Project |
| `bezels/` | Tus marcos: PNG de 1920×1080 transparente donde va el juego, en `bezels/<consola>/`; los que descarga **Ajustes → Marcos de los juegos** también van ahí |
| `themes/` | Fondos (PNG, 1920×1080) y sonidos que reemplazan a los incluidos |
| `settings.ini`, `controls.ini`, `xmb.ini`, `favorites.ini` | Tus ajustes |
| `retroachievements.ini` | Tu sesión de RetroAchievements (su token, nunca la contraseña) |
| `stats.ini` | Las [estadísticas anónimas](#estadísticas-anónimas): activadas o no, el número aleatorio de esta app, el perfil online (activado o no, público o no, su código) |
| `mystats.ini` | [Mis estadísticas](#mis-estadísticas-y-el-perfil-online): tu tiempo jugado, sesiones y partidas online por juego (nunca se envía) |
| `netplay.ini` | [Juego online](#juego-online): tu apodo, la barra de latencia y, si quieres, otro servidor |
| `psp-mac.txt` | La dirección de red propia de la PSP para el [Juego en línea de PSP](#online-con-los-servidores-de-la-comunidad-psp-dreamcast-ds), creada al azar una vez |
| `portable-drives.ini`, `portable-fallback/` | [Modo portátil](#modo-portátil): las memorias que esta PS4 ya respondió; lo guardado mientras la memoria no estaba |
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
EMUSUNI_VERSION=00.23 bash scripts/build.sh with-saturn with-gblink with-openbor   # compila ~/emusuni-build/emusuni_eboot/eboot.bin
bash scripts/package.sh 00.23                                                     # crea el .pkg en ~/emusuni-build/out
```

`scripts/build.sh` compila primero cada núcleo (`scripts/build-*-core.sh`, de nuevo solo lo que
cambió); `no-n64`, `no-ps1`, `no-nds`, `no-psp`, `no-dc`, `no-sega`, `no-arcade`, `no-3ds`, `no-pce`, `no-atari` o
`no-jaguar` dejan fuera una consola, y `with-saturn`, `with-gblink` (el cable link de Game Boy online) y
`with-openbor` incluyen la Saturn, el cable link y OpenBOR (las versiones publicadas los llevan).
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
  [Mednafen](https://mednafen.github.io) (PC Engine), **[Stella](https://stella-emu.github.io)** (Atari 2600),
  **[ProSystem](https://github.com/libretro/prosystem-libretro)** (Atari 7800),
  **[Virtual Jaguar](https://github.com/libretro/virtualjaguar-libretro)** (Atari Jaguar),
  **[FinalBurn Neo](https://github.com/finalburnneo/FBNeo)** (Arcade),
  **[SameBoy](https://sameboy.github.io)** (el cable link de Game Boy online) y
  **[OpenBOR](https://github.com/DCurrent/openbor)**, en [el núcleo libretro de zanac](https://github.com/zanac/openbor-6391-libretro).
- **[RetroAchievements](https://retroachievements.org)** y su biblioteca rcheevos, las valoraciones de la
  **[LaunchBox Games Database](https://gamesdb.launchbox-app.com)** y los marcos de
  **[The Bezel Project](https://github.com/thebezelproject)**, que descarga la app.
- **[Anime4K](https://github.com/bloc97/Anime4K)**, **AMD FidelityFX Super Resolution**,
  **[SMAA](https://github.com/iryoku/smaa)** y **MMPX** — la imagen HD con IA, el escalado suave y
  los bordes limpios.
- **[DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)** — el diseño del lanzador, el runtime de
  PS4, el port de Mesa y la configuración de compilación de los que nació EmuSuNi.
- **[Opus](https://opus-codec.org)** para el chat de voz, y los servidores de la comunidad que mantienen vivos los
  juegos online de la PSP, la Dreamcast (DCNet) y la DS (Wiimmfi, WiiLink WFC).
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

**No incluye juegos, BIOS ni juegos de OpenBOR.** Usa juegos que te pertenezcan. EmuSuNi no está afiliado ni
respaldado por Nintendo, Sony, Sega, Atari ni ningún otro fabricante de estas consolas y juegos, ni por los servicios
a los que se conecta su juego online. *Super Nintendo*, *Super Famicom*, *SNES*, *NES*, *Nintendo 64*, *Game Boy*,
*Game Boy Color*, *Game Boy Advance*, *Nintendo DS* y *Nintendo 3DS* son marcas de Nintendo; *PlayStation*, *PSP* y
*PS4* son marcas de Sony Interactive Entertainment; *Dreamcast*, *Saturn*, *Genesis*, *Mega Drive*, *Master System*,
*Game Gear* y *NAOMI* son marcas de Sega; *Atari 2600*, *Atari 7800* y *Jaguar* son marcas de Atari; los demás
nombres son marcas de sus dueños.
