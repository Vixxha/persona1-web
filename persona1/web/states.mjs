const databaseName = 'persona1.saveStates';
const maxSize = 256 * 1024 * 1024;

function openDatabase() {
    return new Promise((resolve, reject) => {
        const request = indexedDB.open(databaseName, 1);
        request.onupgradeneeded = () => request.result.createObjectStore('slots', { keyPath: 'slot' });
        request.onsuccess = () => {
            request.result.onversionchange = () => request.result.close();
            resolve(request.result);
        };
        request.onerror = () => reject(request.error);
        request.onblocked = () => reject(new Error('Cierra las otras pestañas del juego y vuelve a intentarlo.'));
    });
}

function transaction(db, mode, perform) {
    return new Promise((resolve, reject) => {
        const tx = db.transaction('slots', mode);
        const request = perform(tx.objectStore('slots'));
        tx.oncomplete = () => resolve(request.result);
        tx.onabort = () => reject(tx.error || request.error || new Error('No se pudo guardar el estado.'));
        tx.onerror = () => {};
    });
}

async function hash(bytes) {
    const data = await crypto.subtle.digest('SHA-256', bytes);
    return Array.from(new Uint8Array(data), value => value.toString(16).padStart(2, '0')).join('');
}

async function decompress(blob) {
    const reader = blob.stream().pipeThrough(new DecompressionStream('gzip')).getReader();
    const chunks = [];
    let size = 0;
    try {
        for (;;) {
            const {done, value} = await reader.read();
            if (done) break;
            size += value.length;
            if (size > maxSize) throw new Error('El estado supera el tamaño admitido.');
            chunks.push(value);
        }
    } catch (error) {
        await reader.cancel();
        throw error;
    }
    const data = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) { data.set(chunk, offset); offset += chunk.length; }
    return data;
}

export function createStateManager(getModule, afterRestore) {
    const dialog = document.querySelector('#states-dialog');
    const rows = document.querySelector('#state-slots');
    const status = document.querySelector('#state-notice');
    const slots = new Map();
    const buttons = [];
    const callbacks = new Map();
    let database;
    let build;
    const compatibleBuild = id => !!build && (id === build.build || build.compatibleBuilds?.includes(id));
    let busy = false;
    let sequence = 0;

    function render() {
        for (const { slot, time, save, load } of buttons) {
            const entry = slots.get(slot);
            time.textContent = entry ? new Date(entry.createdAt).toLocaleString() : 'Vacía';
            const compatible = compatibleBuild(entry?.build) && entry?.version === 1;
            load.disabled = busy || !getModule() || !database || !compatible;
            save.disabled = busy || !getModule() || !database;
            load.title = entry && !compatible ? 'Este estado pertenece a otra compilación del juego.' : '';
        }
    }

    function waitNative(module, begin) {
        const token = ++sequence;
        return new Promise((resolve, reject) => {
            callbacks.set(token, { resolve, reject });
            try {
                if (!begin(token)) throw new Error('El motor está ocupado. Vuelve a intentarlo.');
            } catch (error) {
                callbacks.delete(token);
                reject(error);
            }
        });
    }

    async function act(slot, operation) {
        if (busy || !getModule() || !database) return;
        const module = getModule();
        busy = true;
        render();
        try {
            if (operation === 'save') {
                status.textContent = 'Capturando el estado completo…';
                const bytes = await waitNative(module, token => module._persona_state_save(token));
                const checksum = await hash(bytes);
                status.textContent = 'Guardando la ranura ' + slot + '…';
                const data = await new Response(new Blob([bytes]).stream().pipeThrough(new CompressionStream('gzip'))).blob();
                const entry = { slot, version: 1, build: build.build, game: build.game, createdAt: Date.now(), checksum, data };
                await transaction(database, 'readwrite', store => store.put(entry));
                slots.set(slot, entry);
                status.textContent = 'Estado guardado en la ranura ' + slot + '.';
            } else {
                status.textContent = 'Recuperando la ranura ' + slot + '…';
                const entry = await transaction(database, 'readonly', store => store.get(slot));
                if (!entry || entry.version !== 1 || !compatibleBuild(entry.build) || entry.game !== build.game) {
                    throw new Error('La ranura está vacía o corresponde a otra versión del juego.');
                }
                const bytes = await decompress(entry.data);
                if (await hash(bytes) !== entry.checksum) throw new Error('El archivo del estado está dañado.');
                await waitNative(module, token => {
                    const pointer = module._persona_state_upload(bytes.length, token);
                    if (!pointer) return false;
                    try { module.HEAPU8.set(bytes, pointer); }
                    catch (error) { module._persona_state_cancel_upload(); throw error; }
                    module._persona_state_restore();
                    return true;
                });
                afterRestore();
                status.textContent = 'Estado de la ranura ' + slot + ' restaurado.';
                dialog.close();
            }
        } catch (error) {
            console.error(error);
            status.textContent = error?.name === 'QuotaExceededError'
                ? 'El navegador no tiene espacio suficiente. El estado anterior sigue guardado.'
                : 'No se pudo completar la operación. ' + error.message;
        } finally {
            busy = false;
            render();
        }
    }

    for (let slot = 1; slot <= 3; ++slot) {
        const row = document.createElement('tr');
        const name = document.createElement('th');
        name.scope = 'row';
        name.textContent = 'Ranura ' + slot;
        const time = document.createElement('td');
        time.textContent = 'Vacía';
        const save = document.createElement('button');
        const load = document.createElement('button');
        save.type = load.type = 'button';
        save.textContent = 'Guardar';
        load.textContent = 'Cargar';
        save.setAttribute('aria-label', 'Guardar estado en la ranura ' + slot);
        load.setAttribute('aria-label', 'Cargar estado de la ranura ' + slot);
        save.addEventListener('click', () => act(slot, 'save'));
        load.addEventListener('click', () => act(slot, 'load'));
        row.append(name, time);
        for (const button of [save, load]) {
            const cell = document.createElement('td');
            cell.append(button);
            row.append(cell);
        }
        rows.append(row);
        buttons.push({ slot, time, save, load });
    }
    document.querySelector('#states').addEventListener('click', () => { render(); dialog.showModal(); });
    document.querySelector('#close-states').addEventListener('click', () => { if (!busy) dialog.close(); });
    dialog.addEventListener('cancel', event => { if (busy) event.preventDefault(); });
    dialog.addEventListener('close', () => document.querySelector('canvas').focus({ preventScroll: true }));
    render();
    Promise.all([openDatabase(), fetch('state-build.json').then(response => {
        if (!response.ok) throw new Error('No se pudo identificar la compilación del juego.');
        return response.json();
    })]).then(async ([db, identity]) => {
        database = db;
        build = identity;
        const entries = await transaction(db, 'readonly', store => store.getAll());
        for (const entry of entries) if ([1, 2, 3].includes(entry.slot)) slots.set(entry.slot, entry);
        status.textContent = 'Inicia el juego y usa una de las tres ranuras para volver a ese punto.';
        render();
    }).catch(error => {
        status.textContent = 'No se pudo abrir el almacenamiento de estados. ' + error.message;
        console.error(error);
    });

    return {
        get busy() { return busy; },
        refresh: render,
        fail(reason) {
            for (const pending of callbacks.values()) pending.reject(new Error(reason));
            callbacks.clear();
            render();
        },
        nativeCallbacks: {
            onStateCaptured(token, bytes) { callbacks.get(token)?.resolve(bytes); callbacks.delete(token); },
            onStateRestored(token) { callbacks.get(token)?.resolve(); callbacks.delete(token); },
            onStateError(token, message) { callbacks.get(token)?.reject(new Error(message)); callbacks.delete(token); },
        },
    };
}
