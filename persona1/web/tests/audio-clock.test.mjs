import assert from 'node:assert/strict';
import { startAudio } from '../audio.mjs';

let tick;
let clears = 0;
const originalSet = globalThis.setInterval;
const originalClear = globalThis.clearInterval;
globalThis.setInterval = callback => { tick = callback; return 7; };
globalThis.clearInterval = id => { assert.equal(id, 7); ++clears; };
try {
    let pulls = 0;
    const scheduled = [];
    const active = new Set();
    const context = {
        currentTime: 0, state: 'running', destination: {},
        createBuffer(channels, frames, rate) {
            assert.equal(channels, 2); assert.equal(rate, 44100);
            const data = [new Float32Array(frames), new Float32Array(frames)];
            return { getChannelData: i => data[i] };
        },
        createBufferSource() {
            const source = {
                connect() {}, stop() { active.delete(source); },
                start(when, offset = 0) {
                    assert.ok(when >= context.currentTime - 1e-9, 'Scheduled audio in the past');
                    assert.ok(offset >= 0 && offset < 1024 / 44100, 'Invalid PCM offset');
                    const end = when + 1024 / 44100 - offset;
                    scheduled.push({ when, offset, end, chunk: pulls - 1 });
                    source.end = end;
                    active.add(source);
                },
            };
            return source;
        },
    };
    const module = {
        HEAP16: new Int16Array(2048).fill(8192),
        _persona_pull_audio() { ++pulls; return 0; },
    };
    const stop = startAudio(context, module);
    const epoch = 1024 / 44100;
    let stalls = 0;
    // Six minutes of normal ticks mixed with 100-170 ms main-thread stalls.
    // Every audible chunk must still correspond to its original sample time.
    for (let t = .01, n = 1; t < 360; ++n) {
        const delay = n % 89 === 0 ? .1 + (n % 8) * .01 : .01;
        if (delay > .01) ++stalls;
        t += delay;
        context.currentTime = t;
        for (const source of active) {
            if (source.end <= t) { active.delete(source); source.onended?.(); }
        }
        tick();
        const latest = scheduled.at(-1);
        assert.ok(Math.abs(latest.end - (epoch + (latest.chunk + 1) * 1024 / 44100)) < 1e-8,
            'PCM timeline drifted after delayed timer');
        assert.ok(latest.end - t < .065 + 1024 / 44100 + 1e-8, 'Unbounded audio latency');
    }
    const count = pulls;
    context.state = 'suspended';
    tick();
    assert.equal(pulls, count, 'Suspended audio must not consume PCM');
    context.state = 'running';
    // A hidden/suspended page must recover in bounded work.
    context.currentTime += 120;
    tick();
    assert.ok(pulls - count < 40, 'Long stall caused unbounded catch-up');
    assert.ok(scheduled.at(-1).end - context.currentTime < .09);
    stop.reset();
    assert.equal(active.size, 0, 'Loading a state must stop old audio');
    context.currentTime += .02;
    const resetAt = context.currentTime;
    const resetIndex = scheduled.length;
    tick();
    assert.ok(Math.abs(scheduled[resetIndex].when - (resetAt + epoch)) < 1e-8);
    stop();
    assert.equal(active.size, 0);
    assert.equal(clears, 1);
    console.log('PASS: six minutes, ' + stalls + ' stalls, bounded recovery, suspend, state reset and shutdown.');
} finally {
    globalThis.setInterval = originalSet;
    globalThis.clearInterval = originalClear;
}
