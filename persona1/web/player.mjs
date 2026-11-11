import { startAudio } from './audio.mjs';
﻿import { createStateManager } from './states.mjs';
import { createSaveStore } from './save-store.mjs';
import { createControls } from './controls.mjs';
import { loadPromptTextures } from './prompts.mjs';

const canvas = document.querySelector('canvas');
const saveStatus = document.querySelector('#save-status');
const status = document.querySelector('#status');
const start = document.querySelector('#start');
let gameModule;
let promptTextures;
let promptStyle;
const controls = createControls(canvas, style => {
    promptStyle = style;
    if (!gameModule || !promptTextures) return;
    try { promptTextures.apply(gameModule, style); }
    catch (error) {
        document.querySelector('#controls-notice').textContent = 'No se pudieron actualizar los iconos del juego.';
        console.error(error);
    }
});
let resetStateAudio = () => {};
const stateManager = createStateManager(() => gameModule, () => {
    controls.clear();
    resetStateAudio();
});

const player = document.querySelector('#player');
const fullscreen = document.querySelector('#fullscreen');
fullscreen.disabled = !document.fullscreenEnabled || !player.requestFullscreen;
if (fullscreen.disabled) fullscreen.title = 'Este navegador no permite activar la pantalla completa.';

fullscreen.addEventListener('click', async () => {
    try {
        if (document.fullscreenElement === player) await document.exitFullscreen();
        else await player.requestFullscreen();
    } catch (error) {
        status.textContent = 'No se pudo activar la pantalla completa. Vuelve a pulsar el bot?n para reintentar.';
        console.error(error);
    }
});

document.addEventListener('fullscreenchange', () => {
    const active = document.fullscreenElement === player;
    fullscreen.textContent = active ? 'Salir de pantalla completa' : 'Pantalla completa';
    fullscreen.setAttribute('aria-pressed', String(active));
    controls.clear();
    canvas.focus({ preventScroll: true });
});

const width = 480;
const height = 272;
const stride = width * 4;
const shaderSource = `
@group(0) @binding(0) var pixels: texture_2d<f32>;

struct Vertex {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex fn vertex(@builtin(vertex_index) index: u32) -> Vertex {
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    return Vertex(vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0), uv);
}

@fragment fn fragment(input: Vertex) -> @location(0) vec4f {
    let size = vec2f(textureDimensions(pixels));
    let point = clamp(vec2i(input.uv * size), vec2i(0), vec2i(size) - vec2i(1));
    return vec4f(textureLoad(pixels, point, 0).rgb, 1.0);
}
`;

async function createDisplay() {
    const adapter = await navigator.gpu?.requestAdapter();
    if (!adapter) throw new Error('Este navegador no tiene un adaptador WebGPU disponible.');
    const device = await adapter.requestDevice();
    try {
        const context = canvas.getContext('webgpu');
        if (!context) throw new Error('No se pudo crear el contexto WebGPU.');
        const format = navigator.gpu.getPreferredCanvasFormat();
        const shader = device.createShaderModule({ code: shaderSource });
        const compilation = await shader.getCompilationInfo();
        const errors = compilation.messages.filter(message => message.type === 'error');
        if (errors.length) throw new Error(errors.map(error => error.message).join('\n'));
        const pipeline = await device.createRenderPipelineAsync({
            layout: 'auto',
            vertex: { module: shader, entryPoint: 'vertex' },
            fragment: { module: shader, entryPoint: 'fragment', targets: [{ format }] },
        });
        const texture = device.createTexture({
            size: [width, height],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.TEXTURE_BINDING,
        });
        const bindings = device.createBindGroup({
            layout: pipeline.getBindGroupLayout(0),
            entries: [{ binding: 0, resource: texture.createView() }],
        });
        context.configure({ device, format, alphaMode: 'opaque' });
        const frame = new Uint8Array(stride * height);
        let lost = false;
        device.lost.then(info => {
            lost = true;
            if (info.reason !== 'destroyed') status.textContent = 'Se perdi? el dispositivo WebGPU. Recarga la p?gina.';
        });
        device.addEventListener('uncapturederror', event => { status.textContent = event.error.message; });
        return {
            present(heap, pointer) {
                if (lost) return false;
                frame.set(heap.subarray(pointer, pointer + frame.length));
                device.queue.writeTexture({ texture }, frame, { bytesPerRow: stride }, [width, height]);
                const encoder = device.createCommandEncoder();
                const pass = encoder.beginRenderPass({
                    colorAttachments: [{ view: context.getCurrentTexture().createView(), loadOp: 'clear', storeOp: 'store' }],
                });
                pass.setPipeline(pipeline);
                pass.setBindGroup(0, bindings);
                pass.draw(3);
                pass.end();
                device.queue.submit([encoder.finish()]);
                return true;
            },
            destroy() {
                context.unconfigure();
                device.destroy();
            },
        };
    } catch (error) {
        device.destroy();
        throw error;
    }
}

async function asset(name) {
    const response = await fetch(name);
    if (!response.ok) throw new Error(`No se pudo cargar ${name} (HTTP ${response.status}).`);
    return new Uint8Array(await response.arrayBuffer());
}

start.addEventListener('click', async () => {


    start.disabled = true;

    let display;
    let audio;
    let stopAudio;
    let saveStore;
    const closeAudio = () => {
        gameModule = null;
        stateManager.fail("La sesi?n del juego termin?.");
        stopAudio?.();
        if (audio && audio.state !== "closed") void audio.close();
    };
    try {
        if (!crossOriginIsolated) throw new Error('Abre el juego a trav?s del servidor local incluido.');
        audio = new AudioContext({ latencyHint: "interactive" });
        await audio.resume();
        status.textContent = 'Preparando WebGPU y cargando Persona?';
        display = await createDisplay();
        const [{ default: createPersona }, elf, names, image, textures] = await Promise.all([
            import('./persona.mjs'), asset('EBOOT_DECRYPTED.ELF'), asset('nids_persona1.csv'), asset('Persona.iso'), loadPromptTextures(),
        ]);
        promptTextures = textures;
        let firstFrame = true;
        status.textContent = 'Arrancando Persona?';
        const module = await createPersona({
            arguments: ['/EBOOT_DECRYPTED.ELF', '/', '0', '/Persona.iso'],
            preRun: [module => {
                module.FS.writeFile('/EBOOT_DECRYPTED.ELF', elf, { canOwn: true });
                module.FS.writeFile('/nids_persona1.csv', names, { canOwn: true });
                module.FS.writeFile('/Persona.iso', image, { canOwn: true });
                promptTextures.apply(module, promptStyle);
                module.FS.mkdir('/ms0');
                module.FS.mount(module.FS.filesystems.IDBFS, { autoPersist: true }, '/ms0');
                module.addRunDependency('persona-saves');
                module.FS.syncfs(true, error => {
                    if (error) console.warn('No se pudo abrir la copia de seguridad en IndexedDB.', error);
                    try {
                        saveStore = createSaveStore(module.FS, window.localStorage, message => { saveStatus.textContent = message; });
                        saveStore.restore();
                    } catch (failure) {
                        saveStatus.textContent = 'localStorage est? bloqueado. Habilita el almacenamiento para conservar la partida.';
                        console.error(failure);
                    } finally {
                        module.removeRunDependency('persona-saves');
                    }
                });
            }],
            persistSaveSlot(directory) {
                if (saveStore) saveStore.persist(directory);
                else saveStatus.textContent = 'No se pudo guardar en localStorage. Revisa los permisos del navegador.';
            },
            presentFrame(pointer) {
                if (display.present(this.HEAPU8, pointer) && firstFrame) {
                    firstFrame = false;
                    status.textContent = 'Persona est? mostrando imagen.';
                }
            },
            readInput: () => stateManager.busy ? 0x80800000 : controls.read(),
            ...stateManager.nativeCallbacks,
            getPromptRevision: () => promptTextures?.revision || 0,
            print: console.log,
            printErr: console.error,
            onAbort: reason => { closeAudio(); status.textContent = `Error de ejecuci?n: ${reason}`; },
            onExit: code => { closeAudio(); status.textContent = `Persona termin? (c?digo ${code}).`; display.destroy(); },
        });
        gameModule = module;
        promptTextures.apply(module, promptStyle);
        if (audio.state !== "closed") stopAudio = startAudio(audio, module);
        resetStateAudio = () => stopAudio?.reset();
        stateManager.refresh();
        canvas.focus();
    } catch (error) {
        closeAudio();
        display?.destroy();
        status.textContent = error.message;
        start.disabled = false;

    }
});

