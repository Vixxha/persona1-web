const root = '/ms0/PSP/SAVEDATA';
const prefix = 'persona1.ULUS10432.save.v1.';
const validSlot = name => /^ULUS10432[A-Za-z0-9_-]+$/.test(name);
const validFile = name => /^[A-Za-z0-9_.-]+$/.test(name) && name !== '.' && name !== '..';

function encode(bytes) {
    let binary = '';
    for (let i = 0; i < bytes.length; i += 8192) {
        binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
    }
    return btoa(binary);
}

function decode(snapshot) {
    if (snapshot?.version !== 1 || !Number.isFinite(snapshot.savedAt) || snapshot.savedAt < 0 ||
        !Array.isArray(snapshot.files) || (snapshot.deleted !== true && snapshot.deleted !== false) ||
        (snapshot.deleted && snapshot.files.length)) {
        throw new Error('Formato de partida local no reconocido.');
    }
    const names = new Set();
    return snapshot.files.map(file => {
        if (!validFile(file.name) || names.has(file.name) || typeof file.data !== 'string' ||
            !Number.isFinite(file.modified) || file.modified < 0) {
            throw new Error('Archivo de partida local no reconocido.');
        }
        names.add(file.name);
        const binary = atob(file.data);
        return { name: file.name, modified: file.modified, bytes: Uint8Array.from(binary, char => char.charCodeAt(0)) };
    });
}

export function createSaveStore(FS, storage, report) {
    function filesAt(directory) {
        if (!FS.analyzePath(directory).exists) return [];
        return FS.readdir(directory).filter(name => name !== '.' && name !== '..').map(name => {
            const stat = FS.stat(directory + '/' + name);
            if (!validFile(name) || !FS.isFile(stat.mode)) throw new Error('Estructura de partida inesperada.');
            return { name, modified: stat.mtime.getTime() };
        });
    }

    function persist(directory, announce = true) {
        try {
            const slot = directory.slice(root.length + 1);
            if (directory !== root + '/' + slot || !validSlot(slot)) throw new Error('Ruta de partida inesperada.');
            const files = filesAt(directory);
            const snapshot = {
                version: 1,
                savedAt: Math.max(Date.now(), ...files.map(file => file.modified)),
                deleted: !FS.analyzePath(directory).exists,
                files: files.map(file => ({ ...file, data: encode(FS.readFile(directory + '/' + file.name)) })),
            };
            storage.setItem(prefix + slot, JSON.stringify(snapshot));
            if (announce) report('Partida guardada en localStorage.');
            return true;
        } catch (error) {
            console.error('No se pudo guardar la partida en localStorage.', error);
            report('No se pudo actualizar localStorage. Revisa el espacio y los permisos del navegador.');
            return false;
        }
    }

    function restore() {
        FS.mkdirTree(root);
        const slots = new Set(FS.readdir(root).filter(validSlot));
        for (let i = 0; i < storage.length; ++i) {
            const key = storage.key(i);
            if (key?.startsWith(prefix) && validSlot(key.slice(prefix.length))) slots.add(key.slice(prefix.length));
        }
        let failed = false;
        for (const slot of slots) {
            const directory = root + '/' + slot;
            try {
                const stored = storage.getItem(prefix + slot);
                if (stored === null) {
                    if (!persist(directory, false)) failed = true;
                    continue;
                }
                const snapshot = JSON.parse(stored);
                const restored = decode(snapshot);
                const existing = filesAt(directory);
                const dirTime = FS.analyzePath(directory).exists ? FS.stat(directory).mtime.getTime() : 0;
                const latest = Math.max(dirTime, ...existing.map(file => file.modified));
                if (latest > snapshot.savedAt) {
                    if (!persist(directory, false)) failed = true;
                    continue;
                }
                for (const file of existing) FS.unlink(directory + '/' + file.name);
                if (snapshot.deleted) {
                    if (FS.analyzePath(directory).exists) FS.rmdir(directory);
                } else {
                    FS.mkdirTree(directory);
                    for (const file of restored) {
                        const target = directory + '/' + file.name;
                        FS.writeFile(target, file.bytes);
                        FS.utime(target, file.modified, file.modified);
                    }
                    FS.utime(directory, snapshot.savedAt, snapshot.savedAt);
                }
            } catch (error) {
                failed = true;
                console.error('No se pudo recuperar una partida de localStorage.', error);
            }
        }
        report(failed ? 'Algunas partidas locales no pudieron recuperarse. Se conservaron las copias existentes.'
            : 'Guardado local listo. Usa el men? del juego para guardar tu partida.');
    }

    return { persist, restore };
}
