// Schedule PCM against one sample clock; timer delays must not move that clock.
const frames = 1024;
const rate = 44100;
const lookahead = 0.065;
const maxCatchup = Math.ceil(0.75 * rate / frames);

export function startAudio(context, module) {
    let epoch = null;
    let chunk = 0;
    const sources = new Set();
    const reset = () => {
        for (const source of sources) { try { source.stop(); } catch {} }
        sources.clear();
        epoch = null;
        chunk = 0;
    };
    const pump = () => {
        if (context.state !== 'running') return;
        const now = context.currentTime;
        if (epoch === null) epoch = now + frames / rate;
        // The native FIFO holds at most 750 ms. Drain stale PCM after a long
        // interruption without creating thousands of nodes or blocking the UI.
        const expired = Math.max(0, Math.floor((now - epoch) * rate / frames) - chunk);
        if (expired > maxCatchup) {
            for (let i = 0; i < maxCatchup; ++i) module._persona_pull_audio();
            chunk += expired;
        }
        while (epoch + chunk * frames / rate < now + lookahead) {
            const when = epoch + chunk * frames / rate;
            const end = epoch + (++chunk) * frames / rate;
            const pointer = module._persona_pull_audio() >>> 1;
            // Expired chunks belong to time already elapsed, rather than to a
            // new future start. Replaying them would accumulate A/V delay.
            if (end <= now) continue;
            const offset = Math.max(0, now - when);
            const samples = module.HEAP16;
            const buffer = context.createBuffer(2, frames, rate);
            for (let channel = 0; channel < 2; ++channel) {
                const output = buffer.getChannelData(channel);
                for (let i = 0; i < frames; ++i) output[i] = samples[pointer + i * 2 + channel] / 32768;
            }
            const source = context.createBufferSource();
            source.buffer = buffer;
            source.connect(context.destination);
            sources.add(source);
            source.onended = () => sources.delete(source);
            source.start(Math.max(when, now), offset);
        }
    };
    const timer = setInterval(pump, 10);
    pump();
    const stop = () => { clearInterval(timer); reset(); };
    stop.reset = reset;
    return stop;
}
