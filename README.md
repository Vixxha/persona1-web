<p align="center">
  <img src="persona1/web/favicon.png" alt="Persona 1 Web — logo de mariposas violetas / purple butterfly logo" width="180">
</p>

<h1 align="center">Persona 1 Web</h1>

<p align="center">
  <a href="#espanol">Español</a> · <a href="#english">English</a>
</p>

---

<a id="espanol"></a>

## Español

Versión para navegador de **Shin Megami Tensei: Persona** (PSP, edición estadounidense ULUS10432), basada en **Persona: Project Philemon** y el framework [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp). Utiliza **WebAssembly** para ejecutar el motor y **WebGPU** para presentar la imagen.

### Jugar en línea

**[Abrir Persona 1 Web](https://persona1-web-vixxha.leanflour3.chatgpt.site)**

Pulsa **Iniciar juego** y espera la carga inicial, de aproximadamente **826 MB**. Necesitas un navegador con WebGPU habilitado y suficiente memoria.

La página está alojada en **Sites** y los archivos de ejecución se sirven desde **Cloudflare R2**. GitHub contiene el código fuente y la documentación; el juego no depende de GitHub Releases.

### Funciones

- Audio y reproducción de cinemáticas.
- Soporte para mandos, detección automática de iconos y asignaciones configurables.
- Controles de teclado personalizables.
- Partidas guardadas en el navegador y **tres ranuras de estados de partida**.
- Pantalla completa.
- Interfaz violeta, fondo personalizado y logo de mariposas creado por **Vixxha**.

**Problema conocido:** el desfase progresivo entre audio y vídeo de algunas cinemáticas sigue en investigación.

Las partidas se almacenan por navegador y por dominio. Las partidas de `localhost` no se trasladan automáticamente a la web pública.

### Ejecutar en tu equipo

Desde la raíz del repositorio, con tus archivos de ejecución ya compilados en `persona1/web/dist`:

```powershell
node persona1/web/serve.cjs
```

Abre **http://127.0.0.1:8613/**.

La compilación web utiliza Emscripten mediante `persona1/web/build.cjs`. La ISO, el ejecutable original, los archivos de ejecución compilados, las partidas personales y las herramientas locales quedan fuera de este repositorio.

### Documentación y desarrollo

- [Versión web: ejecución, almacenamiento y publicación](persona1/web/README.md).
- [Alojamiento independiente del juego](persona1/web/hosting/README.md).
- [Proyecto nativo de Persona 1 y preparación de tu copia local](persona1/README.md).
- [Arquitectura del framework](docs/ARCHITECTURE.md).
- [Guía para crear perfiles de otros juegos](docs/PROFILE_GUIDE.md).
- [Procedencia del código y dependencias](docs/SOURCE_PROVENANCE.md).

El proyecto original incluye una recompilación nativa para Windows. El framework analiza ejecutables Allegrex/MIPS y genera código C++ para ejecutarlos mediante su entorno nativo. El port web adapta ese motor al navegador.

Para compilar únicamente el framework, necesitas **CMake 3.20 o posterior** y un **compilador C++20**:

```bash
cmake -S . -B out/framework -DPSPRECOMP_PROFILE=""
cmake --build out/framework --config Release
ctest --test-dir out/framework -C Release --output-on-failure
```

### Créditos y licencia

- Base del proyecto: **Persona: Project Philemon** y los colaboradores de **PSPRecomp**.
- Logo de mariposas de la página: **Vixxha**.
- Iconos de mando: **WarDrimmer, ryuubu y Karasu** — [mod original](https://gamebanana.com/mods/373811).
- El código del framework se distribuye bajo la [licencia MIT](LICENSE). Las bibliotecas y los recursos de terceros conservan sus propias licencias y atribuciones.

Este proyecto no está afiliado ni respaldado por Atlus o Sega. Persona y Shin Megami Tensei son sus marcas registradas.

---

<a id="english"></a>

## English

A browser version of **Shin Megami Tensei: Persona** (PSP, US release ULUS10432), based on **Persona: Project Philemon** and the [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) framework. It uses **WebAssembly** to run the engine and **WebGPU** to display the game.

### Play online

**[Open Persona 1 Web](https://persona1-web-vixxha.leanflour3.chatgpt.site)**

Press **Iniciar juego** (“Start game”) and wait for the initial download of approximately **826 MB**. You need a browser with WebGPU enabled and sufficient memory.

The website is hosted on **Sites**, and runtime files are served from **Cloudflare R2**. GitHub contains the source code and documentation; the game does not depend on GitHub Releases.

### Features

- Audio and cinematic playback.
- Gamepad support, automatic button icon detection, and configurable bindings.
- Customizable keyboard controls.
- Browser saves and **three save-state slots**.
- Fullscreen mode.
- Purple interface, custom background, and a butterfly logo created by **Vixxha**.

**Known issue:** progressive audio/video desynchronization in some cinematics is still being investigated.

Saves are stored separately for each browser and domain. Saves from `localhost` do not automatically transfer to the public website.

### Run locally

From the repository root, with your compiled runtime files already available in `persona1/web/dist`:

```powershell
node persona1/web/serve.cjs
```

Open **http://127.0.0.1:8613/**.

The web build uses Emscripten through `persona1/web/build.cjs`. The ISO, original executable, compiled runtime files, personal saves, and local tools are excluded from this repository.

### Documentation and development

- [Web version: running, storage, and publishing](persona1/web/README.md).
- [Independent game hosting](persona1/web/hosting/README.md).
- [Native Persona 1 project and preparing your local copy](persona1/README.md).
- [Framework architecture](docs/ARCHITECTURE.md).
- [Guide to creating profiles for other games](docs/PROFILE_GUIDE.md).
- [Source provenance and dependencies](docs/SOURCE_PROVENANCE.md).

The original project includes a native Windows recompilation. The framework analyzes Allegrex/MIPS executables and generates C++ code to run them through its native runtime. The web port adapts that engine to the browser.

To build only the framework, you need **CMake 3.20 or newer** and a **C++20 compiler**:

```bash
cmake -S . -B out/framework -DPSPRECOMP_PROFILE=""
cmake --build out/framework --config Release
ctest --test-dir out/framework -C Release --output-on-failure
```

### Credits and license

- Project foundation: **Persona: Project Philemon** and the **PSPRecomp** contributors.
- Website butterfly logo: **Vixxha**.
- Gamepad icons: **WarDrimmer, ryuubu, and Karasu** — [original mod](https://gamebanana.com/mods/373811).
- Framework code is distributed under the [MIT License](LICENSE). Third-party libraries and assets retain their own licenses and attribution notices.

This project is not affiliated with or endorsed by Atlus or Sega. Persona and Shin Megami Tensei are their trademarks.
