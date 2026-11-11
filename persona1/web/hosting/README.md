# Alojamiento del juego

La web se publica en Sites. Cloudflare R2 almacena los archivos de ejecución y el Worker los sirve mediante streams.
GitHub se utiliza para el código fuente y la documentación.

La configuración del sitio declara `"r2": "BUCKET"` en `.openai/hosting.json`.
Para actualizar la interfaz, copia los recursos HTML, MJS, CSS, PNG, JSON y RGBA de `persona1/web/dist` a la carpeta `public` de la publicación y ejecuta `node build.mjs` y `node test.mjs`.

Los archivos de ejecución se actualizan mediante un flujo de administración temporal del almacenamiento de Sites.
El Worker público es de solo lectura: no contiene rutas de carga y admite únicamente GET y HEAD.
La ISO y los demás archivos del juego permanecen fuera de GitHub, tanto del código fuente como de Releases.
Conserva el archivo local `.openai/hosting.json` para mantener la identidad y el almacenamiento del sitio existente.

Las rutas de lectura solo permiten los cuatro archivos de ejecución definidos en el Worker.
Las partidas permanecen en el navegador de cada visitante.
