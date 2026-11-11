# Publicacion en Sites

Este Worker sirve los recursos de la pagina y transfiere los archivos grandes desde la release web-v1 de GitHub.

La carpeta de publicacion activa esta en .publish/persona1-web, junto al repositorio principal. Conserva su archivo .openai/hosting.json: contiene la identidad del sitio, sin claves secretas.

Para actualizar: copia los cambios de estos scripts a la carpeta de publicacion, actualiza public con los recursos HTML, MJS, CSS, PNG, JSON y RGBA de persona1/web/dist, ejecuta node build.mjs y node test.mjs, y publica con el flujo de Sites.

Los archivos ISO, ELF, WASM y CSV se sirven desde la release. Si cambia el motor, publica una nueva version de esos archivos y actualiza runtimeOrigin en worker.mjs antes de publicar la web.
