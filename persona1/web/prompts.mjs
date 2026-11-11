import { actions, keyLabel, padLabel } from './controls.mjs';

async function fetchAsset(name) {
    const response = await fetch(name);
    if (!response.ok) throw new Error('No se pudieron cargar los iconos del mando (' + response.status + ').');
    return new Uint8Array(await response.arrayBuffer());
}

const faceCells = { circle: [160, 0, 80, 80], cross: [240, 0, 80, 80],
    square: [320, 0, 80, 80], triangle: [400, 0, 80, 80] };
const buttonCells = [faceCells.cross, faceCells.circle, faceCells.square, faceCells.triangle];
const defaultBindings = Object.fromEntries(actions.map(action => [action.id, action.pad]));

function imageCanvas(bytes) {
    const header = new DataView(bytes.buffer, bytes.byteOffset, 8);
    const w = header.getUint32(0, true), h = header.getUint32(4, true);
    if (bytes.length !== 8 + w * h * 4) throw new Error('Textura de mando incompleta.');
    const canvas = new OffscreenCanvas(w, h);
    const context = canvas.getContext('2d');
    context.putImageData(new ImageData(new Uint8ClampedArray(bytes.slice(8)), w, h), 0, 0);
    return canvas;
}

function paintLabel(context, rectangle, text) {
    const [x, y, w, h] = rectangle;
    context.clearRect(x, y, w, h);
    context.fillStyle = '#101014';
    context.fillRect(x + 2, y + 2, w - 4, h - 4);
    context.strokeStyle = '#e9e9ef';
    context.lineWidth = 2;
    context.strokeRect(x + 3, y + 3, w - 6, h - 6);
    let size = Math.min(h * 0.55, 40);
    context.font = '600 ' + size + 'px system-ui';
    while (context.measureText(text).width > w - 12 && size > 9) {
        --size;
        context.font = '600 ' + size + 'px system-ui';
    }
    context.textAlign = 'center';
    context.textBaseline = 'middle';
    context.fillStyle = '#ffffff';
    context.fillText(text, x + w / 2, y + h / 2);
}

function drawDirections(context, x, y, style) {
    context.clearRect(x, y, 80, 80);
    const cells = { up: [x + 26, y, 28, 27], down: [x + 26, y + 53, 28, 27],
        left: [x, y + 26, 29, 28], right: [x + 51, y + 26, 29, 28] };
    for (const [action, rectangle] of Object.entries(cells)) {
        const binding = style.bindings[action];
        paintLabel(context, rectangle, style.mode === 'keyboard' ? keyLabel(binding) : padLabel(binding, style.family));
    }
}

function customize(file, bytes, style, sourceControls) {
    const relevant = file === 'UI/Controls.png' ? ['cross', 'circle', 'square', 'triangle', 'l', 'r', 'up', 'down', 'left', 'right']
        : file === 'UI/Controls2.png' ? ['start', 'up', 'down', 'left', 'right'] : ['l', 'r'];
    if (style.mode === 'gamepad' && relevant.every(action => style.bindings[action] === defaultBindings[action])) return bytes;
    const canvas = imageCanvas(bytes);
    const context = canvas.getContext('2d');
    const label = action => style.mode === 'keyboard' ? keyLabel(style.bindings[action]) : padLabel(style.bindings[action], style.family);
    if (file === 'UI/Controls.png') {
        for (const [action, rectangle] of Object.entries(faceCells)) {
            const binding = style.bindings[action];
            if (style.mode === 'gamepad' && /^button:[0-3]$/.test(binding || '')) {
                context.clearRect(...rectangle);
                context.drawImage(sourceControls, ...buttonCells[Number(binding.split(':')[1])], ...rectangle);
            } else paintLabel(context, rectangle, label(action));
        }
        for (const [action, rectangle] of [['l', [480, 0, 120, 40]], ['r', [480, 40, 120, 40]]]) {
            if (style.mode === 'keyboard' || style.bindings[action] !== defaultBindings[action]) paintLabel(context, rectangle, label(action));
        }
        if (style.mode === 'keyboard' || ['up', 'down', 'left', 'right'].some(action => style.bindings[action] !== defaultBindings[action])) {
            drawDirections(context, 0, 0, style);
            drawDirections(context, 80, 0, style);
        }
    } else if (file === 'UI/Controls2.png') {
        if (style.mode === 'keyboard' || style.bindings.start !== defaultBindings.start) paintLabel(context, [0, 0, 256, 80], label('start'));
        if (style.mode === 'keyboard' || ['up', 'down', 'left', 'right'].some(action => style.bindings[action] !== defaultBindings[action])) {
            drawDirections(context, 280, 0, style);
        }
    } else {
        if (style.mode === 'keyboard' || style.bindings.l !== defaultBindings.l) paintLabel(context, [0, 0, 85, 60], label('l'));
        if (style.mode === 'keyboard' || style.bindings.r !== defaultBindings.r) paintLabel(context, [120, 0, 85, 60], label('r'));
    }
    const output = new Uint8Array(8 + canvas.width * canvas.height * 4);
    output.set(bytes.subarray(0, 8));
    output.set(context.getImageData(0, 0, canvas.width, canvas.height).data, 8);
    return output;
}

export async function loadPromptTextures() {
    const manifest = JSON.parse(new TextDecoder().decode(await fetchAsset('controller-prompts.json')));
    const textures = {};
    await Promise.all(Object.entries(manifest.families).map(async ([family, files]) => {
        textures[family] = {};
        await Promise.all(Object.entries(files).map(async ([file, entry]) => {
            textures[family][file] = await fetchAsset(entry.asset);
        }));
    }));
    let signature = '';
    let revision = 0;
    return {
        get revision() { return revision; },
        apply(module, style) {
            const next = JSON.stringify(style);
            if (next === signature) return;
            const family = style.mode === 'keyboard' ? 'playstation' : style.family;
            const source = textures[family];
            const sourceControls = imageCanvas(source['UI/Controls.png']);
            const prepared = Object.entries(source).map(([file, bytes]) => [file, customize(file, bytes, style, sourceControls)]);
            module.FS.mkdirTree('/mods/UI');
            module.FS.mkdirTree('/mods/Casino');
            module.FS.writeFile('/mods/textures.ini', manifest.ini);
            for (const [file, bytes] of prepared) module.FS.writeFile('/mods/' + file + '.rgba', bytes);
            signature = next;
            ++revision;
        },
    };
}
