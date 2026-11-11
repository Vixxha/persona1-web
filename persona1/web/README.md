# Persona 1 Web

[Jugar en la web pública](https://persona1-web-vixxha.leanflour3.chatgpt.site)

## Código y alojamiento

GitHub contiene el código fuente, los recursos de interfaz y las instrucciones.
El juego publicado se ejecuta en Sites y sus archivos grandes se sirven desde almacenamiento Cloudflare R2 del propio sitio.
La web no utiliza Releases ni descarga archivos de GitHub para jugar.

La primera carga transfiere aproximadamente 826 MB. Requiere un navegador con WebGPU y suficiente memoria.
Las partidas se guardan por navegador y por dominio; las partidas de localhost no se trasladan automáticamente a la web pública.
El desfase progresivo de algunas cinemáticas sigue en investigación.

## Desarrollo local

Desde la raíz del repositorio, con los archivos de ejecución propios ya compilados en `persona1/web/dist`:

```powershell
node persona1/web/serve.cjs
```

Abre http://127.0.0.1:8613/. `build.cjs` compila el motor con Emscripten a partir de los archivos de tu copia local.
Los archivos ISO, ELF, WASM, los estados personales y las herramientas locales no se incluyen en Git.

## Publicación y actualizaciones

Consulta [hosting/README.md](hosting/README.md). El proyecto activo del sitio se conserva en `.publish/persona1-web`, junto a este repositorio.
Conserva su archivo `.openai/hosting.json` para actualizar el sitio existente.

Se conserva la licencia MIT y los créditos del proyecto original y las bibliotecas incluidas.
Los iconos de mando proceden del mod de WarDrimmer, ryuubu y Karasu enlazado en el panel de controles.
