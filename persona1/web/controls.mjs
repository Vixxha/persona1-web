export const actions = [
    { id: 'up', label: 'Arriba', bit: 0x10, key: 'ArrowUp', pad: 'button:12' },
    { id: 'down', label: 'Abajo', bit: 0x40, key: 'ArrowDown', pad: 'button:13' },
    { id: 'left', label: 'Izquierda', bit: 0x80, key: 'ArrowLeft', pad: 'button:14' },
    { id: 'right', label: 'Derecha', bit: 0x20, key: 'ArrowRight', pad: 'button:15' },
    { id: 'cross', label: 'Cruz / confirmar', bit: 0x4000, key: 'KeyZ', pad: 'button:0' },
    { id: 'circle', label: 'C?rculo / cancelar', bit: 0x2000, key: 'KeyX', pad: 'button:1' },
    { id: 'square', label: 'Cuadrado', bit: 0x8000, key: 'KeyA', pad: 'button:2' },
    { id: 'triangle', label: 'Tri?ngulo', bit: 0x1000, key: 'KeyS', pad: 'button:3' },
    { id: 'l', label: 'L', bit: 0x100, key: 'KeyQ', pad: 'button:4' },
    { id: 'r', label: 'R', bit: 0x200, key: 'KeyW', pad: 'button:5' },
    { id: 'start', label: 'Start', bit: 8, key: 'Enter', pad: 'button:9' },
    { id: 'select', label: 'Select', bit: 1, key: 'Space', pad: 'button:8' },
    { id: 'analogUp', label: 'Stick arriba', bit: 0, key: 'KeyI', pad: 'axis:1:-1' },
    { id: 'analogDown', label: 'Stick abajo', bit: 0, key: 'KeyK', pad: 'axis:1:1' },
    { id: 'analogLeft', label: 'Stick izquierda', bit: 0, key: 'KeyJ', pad: 'axis:0:-1' },
    { id: 'analogRight', label: 'Stick derecha', bit: 0, key: 'KeyL', pad: 'axis:0:1' },
];
export const families = { playstation: 'PlayStation', xbox: 'Xbox', switch: 'Switch' };
export const neutralInput = 0x80800000 >>> 0;
export const defaultKeyboard = () => Object.fromEntries(actions.map(action => [action.id, action.key]));
export const defaultGamepad = () => Object.fromEntries(actions.map(action => [action.id, action.pad]));

export function detectFamily(id) {
    if (/054c|sony|playstation|dualshock|dualsense/i.test(id)) return 'playstation';
    if (/057e|nintendo|switch|joy-con/i.test(id)) return 'switch';
    return 'xbox';
}

export function bindingValue(pad, binding) {
    if (!pad || pad.connected === false || !binding) return 0;
    const parts = binding.split(':');
    const index = Number(parts[1]);
    if (parts[0] === 'button') {
        const button = pad.buttons[index];
        return Math.min(1, Math.max(0, button?.value || (button?.pressed ? 1 : 0)));
    }
    const value = Math.max(0, (pad.axes[index] || 0) * Number(parts[2]));
    return value <= 0.2 ? 0 : Math.min(1, (value - 0.2) / 0.8);
}

export function readMappedInput(pressed, pad, keyboard, gamepad) {
    const values = Object.fromEntries(actions.map(action => [action.id,
        Math.max(pressed.has(keyboard[action.id]) ? 1 : 0, bindingValue(pad, gamepad[action.id]))]));
    let buttons = 0;
    for (const action of actions) if (action.bit && values[action.id] >= 0.5) buttons |= action.bit;
    const byte = value => Math.round(128 + Math.max(-1, Math.min(1, value)) * (value < 0 ? 128 : 127));
    const x = byte(values.analogRight - values.analogLeft);
    const y = byte(values.analogDown - values.analogUp);
    return (buttons | (x << 16) | (y << 24)) >>> 0;
}

export function swapBinding(bindings, action, next) {
    const other = actions.find(item => item.id !== action && bindings[item.id] === next && next !== null);
    if (other) bindings[other.id] = bindings[action];
    bindings[action] = next;
}

export function keyLabel(code) {
    if (!code) return 'Sin asignar';
    const names = { ArrowUp: '?', ArrowDown: '?', ArrowLeft: '?', ArrowRight: '?', Space: 'Espacio',
        Enter: 'Enter', ShiftLeft: 'Shift izq.', ShiftRight: 'Shift der.', ControlLeft: 'Ctrl izq.',
        ControlRight: 'Ctrl der.', AltLeft: 'Alt izq.', AltRight: 'Alt der.', Backspace: 'Retroceso' };
    return names[code] || code.replace(/^(Key|Digit|Numpad)/, '');
}

export function padLabel(binding, family) {
    if (!binding) return 'Sin asignar';
    const [kind, index, sign] = binding.split(':');
    if (kind === 'axis') return 'Eje ' + (Number(index) + 1) + (sign === '-1' ? ' ?' : ' +');
    const face = { playstation: ['?', '?', '?', '?'], xbox: ['A', 'B', 'X', 'Y'], switch: ['B', 'A', 'Y', 'X'] };
    const shoulder = { playstation: ['L1', 'R1', 'L2', 'R2', 'Share', 'Options'],
        xbox: ['LB', 'RB', 'LT', 'RT', 'View', 'Menu'], switch: ['L', 'R', 'ZL', 'ZR', '?', '+'] };
    const labels = [...face[family], ...shoulder[family], 'L3', 'R3', '?', '?', '?', '?', 'Home'];
    return labels[Number(index)] || 'Bot?n ' + (Number(index) + 1);
}

const validKey = value => value === null || (typeof value === 'string' && /^[A-Za-z0-9]+$/.test(value) && value.length < 64);
const validPad = value => value === null || (typeof value === 'string' && /^(button:\d+|axis:\d+:(-1|1))$/.test(value));
function sanitize(bindings, defaults, valid) {
    return Object.fromEntries(actions.map(action => [action.id,
        bindings && Object.hasOwn(bindings, action.id) && valid(bindings[action.id]) ? bindings[action.id] : defaults[action.id]]));
}

export function loadConfig(storage) {
    const defaults = { version: 1, keyboard: defaultKeyboard(), gamepads: {}, icons: 'auto' };
    const text = storage.getItem('persona1.controls.v1');
    if (!text) return defaults;
    const saved = JSON.parse(text);
    if (saved?.version !== 1) throw new Error('Versi?n de controles desconocida.');
    defaults.keyboard = sanitize(saved.keyboard, defaults.keyboard, validKey);
    if (saved.gamepads && typeof saved.gamepads === 'object') {
        for (const [id, bindings] of Object.entries(saved.gamepads)) {
            if (id.startsWith('gamepad:')) defaults.gamepads[id] = sanitize(bindings, defaultGamepad(), validPad);
        }
    }
    if (saved.icons === 'auto' || Object.hasOwn(families, saved.icons)) defaults.icons = saved.icons;
    return defaults;
}

export function createControls(canvas, onStyle) {
    const dialog = document.querySelector('#controls-dialog');
    const table = document.querySelector('#bindings');
    const notice = document.querySelector('#controls-notice');
    const deviceSelect = document.querySelector('#controller-select');
    const iconSelect = document.querySelector('#icon-style');
    const deviceStatus = document.querySelector('#controller-status');
    const keyboard = new Set();
    let config;
    try { config = loadConfig(window.localStorage); }
    catch (error) {
        config = { version: 1, keyboard: defaultKeyboard(), gamepads: {}, icons: 'auto' };
        notice.textContent = 'No se pudieron recuperar los controles. Se usar? la configuraci?n inicial.';
        console.error(error);
    }
    let current = null;
    let selected = 'auto';
    let lastDevice = 'keyboard';
    let capture = null;
    let armed = false;
    let inventory = '';
    let previousStyle = '';
    const signatures = new Map();
    const rowButtons = new Map();
    const activeProfile = () => {
        if (!current) return defaultGamepad();
        const key = 'gamepad:' + current.id;
        config.gamepads[key] ||= defaultGamepad();
        return config.gamepads[key];
    };
    function save() {
        try {
            window.localStorage.setItem('persona1.controls.v1', JSON.stringify(config));
            notice.textContent = 'Controles guardados en este navegador.';
        } catch (error) {
            notice.textContent = 'Los controles cambiaron, pero el navegador no permiti? guardarlos.';
            console.error(error);
        }
    }
    function render() {
        const family = config.icons === 'auto' ? detectFamily(current?.id || '') : config.icons;
        deviceStatus.textContent = current ? 'Mando conectado: ' + current.id : 'Conecta un mando y pulsa uno de sus botones.';
        iconSelect.value = config.icons;
        const profile = activeProfile();
        for (const action of actions) {
            const row = rowButtons.get(action.id);
            row.key.textContent = keyLabel(config.keyboard[action.id]);
            row.pad.textContent = current ? padLabel(profile[action.id], family) : 'Sin mando';
            row.pad.disabled = !current;
        }
    }
    function finishCapture(binding) {
        const bindings = capture.kind === 'key' ? config.keyboard : activeProfile();
        swapBinding(bindings, capture.action, binding);
        capture = null;
        save();
        render();
    }
    function beginCapture(kind, action) {
        keyboard.clear();
        capture = { kind, action };
        armed = false;
        notice.textContent = kind === 'key' ? 'Pulsa la tecla deseada. Esc cancela.'
            : 'Suelta los controles y pulsa un bot?n o mueve un eje del mando. Esc cancela.';
    }
    for (const action of actions) {
        const row = document.createElement('tr');
        const label = document.createElement('th');
        label.scope = 'row';
        label.textContent = action.label;
        const key = document.createElement('button');
        const pad = document.createElement('button');
        key.type = pad.type = 'button';
        key.setAttribute('aria-label', 'Cambiar teclado para ' + action.label);
        pad.setAttribute('aria-label', 'Cambiar mando para ' + action.label);
        key.addEventListener('click', () => beginCapture('key', action.id));
        pad.addEventListener('click', () => beginCapture('pad', action.id));
        for (const button of [key, pad]) {
            const cell = document.createElement('td');
            cell.append(button);
            row.append(cell);
        }
        row.prepend(label);
        table.append(row);
        rowButtons.set(action.id, { key, pad });
    }
    document.querySelector('#controls').addEventListener('click', () => {
        keyboard.clear();
        render();
        dialog.showModal();
    });
    function cancelCapture() {
        if (!capture) return false;
        capture = null;
        notice.textContent = 'Cambio cancelado.';
        render();
        return true;
    }
    dialog.addEventListener('cancel', event => { if (cancelCapture()) event.preventDefault(); });
    dialog.addEventListener('close', () => { capture = null; keyboard.clear(); canvas.focus({ preventScroll: true }); });
    document.querySelector('#close-controls').addEventListener('click', () => dialog.close());
    document.querySelector('#clear-binding').addEventListener('click', () => {
        if (capture) finishCapture(null);
        else notice.textContent = 'Elige primero el control que deseas dejar sin asignar.';
    });
    document.querySelector('#reset-controls').addEventListener('click', () => {
        config.keyboard = defaultKeyboard();
        if (current) config.gamepads['gamepad:' + current.id] = defaultGamepad();
        capture = null;
        save();
        render();
    });
    deviceSelect.addEventListener('change', () => {
        selected = deviceSelect.value;
        capture = null;
        keyboard.clear();
    });
    iconSelect.addEventListener('change', () => { config.icons = iconSelect.value; save(); render(); });
    window.addEventListener('keydown', event => {
        if (dialog.open && capture?.kind === 'key') {
            event.preventDefault();
            event.stopPropagation();
            if (event.code === 'Escape') cancelCapture();
            else if (!event.repeat && validKey(event.code)) finishCapture(event.code);
            return;
        }
        if (dialog.open || event.target !== canvas) return;
        if (actions.some(action => config.keyboard[action.id] === event.code)) {
            event.preventDefault();
            keyboard.add(event.code);
            lastDevice = 'keyboard';
        }
    }, true);
    window.addEventListener('keyup', event => {
        if (keyboard.delete(event.code)) event.preventDefault();
    }, true);
    canvas.addEventListener('blur', () => keyboard.clear());
    window.addEventListener('blur', () => keyboard.clear());
    document.addEventListener('visibilitychange', () => keyboard.clear());

    function poll() {
        let pads = [];
        try { pads = Array.from(navigator.getGamepads?.() || []).filter(pad => pad?.connected); }
        catch (error) { deviceStatus.textContent = 'El navegador no permite acceder a los mandos.'; }
        const nextInventory = pads.map(pad => pad.index + ':' + pad.id).join('|');
        if (inventory !== nextInventory) {
            inventory = nextInventory;
            deviceSelect.replaceChildren(new Option('Autom?tico', 'auto'));
            for (const pad of pads) deviceSelect.add(new Option(pad.id, String(pad.index)));
            if (selected !== 'auto' && !pads.some(pad => String(pad.index) === selected)) selected = 'auto';
            deviceSelect.value = selected;
        }
        let nextPad = pads.find(pad => pad.index === current?.index && pad.id === current?.id) || pads[0] || null;
        const focused = document.hasFocus() && !document.hidden;
        for (const pad of pads) {
            const activity = pad.buttons.map(button => button.pressed || button.value > 0.5 ? 1 : 0).join('') + ':' +
                pad.axes.map(value => Math.abs(value) < 0.25 ? 0 : Math.round(value * 8)).join(',');
            const previous = signatures.get(pad.index + ':' + pad.id);
            const active = pad.buttons.some(button => button.pressed || button.value > 0.5) || pad.axes.some(value => Math.abs(value) > 0.25);
            if (focused && active && activity !== previous) {
                if (selected === 'auto') nextPad = pad;
                if (selected === 'auto' || String(pad.index) === selected) lastDevice = 'gamepad';
            }
            signatures.set(pad.index + ':' + pad.id, activity);
        }
        if (selected !== 'auto') nextPad = pads.find(pad => String(pad.index) === selected) || null;
        if (current?.id !== nextPad?.id || current?.index !== nextPad?.index || inventory !== nextInventory) {
            capture = null;
            current = nextPad;
            render();
        } else current = nextPad;
        if (dialog.open && capture?.kind === 'pad' && current) {
            const activeButton = current.buttons.findIndex(button => button.pressed || button.value > 0.65);
            const activeAxis = current.axes.findIndex(value => Math.abs(value) > 0.65);
            if (activeButton < 0 && activeAxis < 0) armed = true;
            else if (armed) finishCapture(activeButton >= 0 ? 'button:' + activeButton
                : 'axis:' + activeAxis + ':' + (current.axes[activeAxis] < 0 ? -1 : 1));
        }
        const family = config.icons === 'auto' ? detectFamily(current?.id || '') : config.icons;
        const mode = lastDevice === 'gamepad' && current ? 'gamepad' : 'keyboard';
        const style = { family, mode, bindings: mode === 'gamepad' ? activeProfile() : config.keyboard };
        const signature = JSON.stringify(style);
        if (signature !== previousStyle) {
            previousStyle = signature;
            onStyle(style);
        }
        requestAnimationFrame(poll);
    }
    render();
    poll();
    return {
        clear() { keyboard.clear(); },
        read() {
            if (document.querySelector("dialog[open]") || document.hidden || !document.hasFocus()) return neutralInput;
            return readMappedInput(keyboard, current, config.keyboard, activeProfile());
        },
    };
}
