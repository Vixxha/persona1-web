# Persona 1 Web

Versión para navegador de Persona: Project Philemon, con WebAssembly y WebGPU.

## Ejecutar en tu equipo

Desde la raiz del repositorio:

```powershell
node persona1/web/serve.cjs
```

Abre http://127.0.0.1:8613/. El servidor sirve la carpeta `dist` que genera `build.cjs`. Para usar la version ya compilada, descarga los archivos de la release `web-v1` en `persona1/web/dist` y extrae `web-assets.zip` en esa misma carpeta.
Se necesita un navegador con WebGPU, audio habilitado y suficiente memoria para cargar el disco completo.

## Funciones

- Sonido y cinemáticas.
- Mando, detección automática de iconos y controles configurables.
- Guardado de partidas en el navegador y tres ranuras de estados.
- Pantalla completa y diseño violeta con fondo y favicon personalizados.

El desfase progresivo de algunas cinemáticas sigue en investigación.

## Publicación

El código fuente se mantiene separado de los archivos de ejecución. El repositorio ignora ISO, ELF, código generado del juego y herramientas locales.
La web utiliza un Worker con las cabeceras COOP/COEP necesarias para WebAssembly con hilos.
Los archivos grandes se transfieren como streams desde una versión de ejecución; el Worker no carga la ISO completa en memoria.
La primera carga descarga aproximadamente 826 MB. Las partidas se guardan por navegador y por dominio; las partidas de localhost no se trasladan automáticamente a la web pública.

El proyecto de publicación y su identidad están en la carpeta local `.publish/persona1-web`, junto al repositorio.
Para actualizarlo, copia los recursos de `persona1/web/dist` a su carpeta `public`, excluyendo ISO, ELF, WASM, CSV y archivos de compilación, ejecuta `node build.mjs` y `node test.mjs`, y publica una versión de Sites.
Si se recompila el motor, publica también sus archivos de ejecución y actualiza la versión de descarga en el Worker.

Se conserva la licencia MIT y los créditos del proyecto original y de las bibliotecas incluidas. Los iconos de mando proceden del mod de WarDrimmer, ryuubu y Karasu enlazado desde el panel de controles.
