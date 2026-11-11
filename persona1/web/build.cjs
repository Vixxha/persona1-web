const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');

const persona = path.resolve(__dirname, '..');
const repo = path.resolve(persona, '..');
const tools = path.resolve(repo, '..', '.tools');
const sdk = path.join(tools, 'emsdk-4.0.23');
const python = path.join(sdk, 'python/3.13.3_64bit/python.exe');
const emcc = path.join(sdk, 'upstream/emscripten/em++.py');
const env = { ...process.env, EM_CONFIG: path.join(sdk, '.emscripten'), EM_CACHE: path.join(sdk, 'cache'), EMCC_CORES: '4' };
const objects = path.join(__dirname, 'objects');
const staging = path.join(__dirname, 'dist-build');
const dist = path.join(__dirname, 'dist');
const split = path.join(persona, 'gen6/split');
const baseFlags = ['-std=c++20', '-O1', '-pthread', '-fexceptions', '-D_fseeki64=fseeko', '-D_ftelli64=ftello',
    '-Iinclude', '-Ipersona1', '-Ipersona1/gen6/split'];
const atracShim = 'persona1/web/third_party/at3_shim';
const atrac = 'persona1/web/third_party/at3_standalone';
const h264 = 'persona1/web/third_party/openh264';
const h264Includes = ['codec/api', 'codec/api/wels', 'codec/common/inc', 'codec/decoder/core/inc', 'codec/decoder/plus/inc']
    .map(dir => '-I' + h264 + '/' + dir);
const h264Sources = ['codec/common/src', 'codec/decoder/core/src', 'codec/decoder/plus/src']
    .flatMap(dir => fs.readdirSync(path.join(repo, h264, dir)).filter(name => name.endsWith('.cpp'))
        .map(name => h264 + '/' + dir + '/' + name));
const sourceNames = [
    ...h264Sources,
    ...['decoder', 'deflate', 'elf32', 'guest_memory', 'nid_registry', 'program_analysis', 'runtime', 'sha256']
        .map(name => 'src/' + name + '.cpp'),
    ...['main', 'kernel', 'io', 'ge', 'audio', 'movie', 'mods'].map(name => 'persona1/persona1_' + name + '.cpp'),
    'persona1/web/video.cpp',
    'persona1/web/states.cpp',
    ...fs.readdirSync(path.join(repo, atrac)).filter(name => name.endsWith('.cpp')).map(name => atrac + '/' + name),
    ...fs.readdirSync(split).filter(name => name.endsWith('.cpp')).map(name => 'persona1/gen6/split/' + name),
];

function invoke(args) {
    return new Promise((resolve, reject) => {
        const child = spawn(python, [emcc, ...args], { cwd: repo, env, stdio: 'inherit' });
        child.once('error', reject);
        child.once('exit', code => code === 0 ? resolve() : reject(new Error('em++ exited with ' + code)));
    });
}

function stamp(file) {
    return fs.statSync(file, { throwIfNoEntry: false })?.mtimeMs ?? 0;
}

function prepareSource(source) {
    if (!path.basename(source).startsWith('reg_')) return source;
    const input = fs.readFileSync(path.join(repo, source), 'utf8');
    const calls = /    runtime\.register_function\((0x[0-9A-Fa-f]+u), (&\w+), ("\w+")\);\r?\n/g;
    const matches = [...input.matchAll(calls)];
    const entries = matches.map(([, address, fn, name]) => '        {' + address + ', ' + fn + ', ' + name + '},');
    if (!entries.length || entries.length !== input.split('runtime.register_function(').length - 1) {
        throw new Error('Unrecognized function registrations in ' + source);
    }
    const table = [
        '    struct Entry { std::uint32_t address; Runtime::RecompiledFunction function; const char *name; };',
        '    static constexpr Entry entries[] = {',
        ...entries,
        '    };',
        '    for (const auto &entry : entries) runtime.register_function(entry.address, entry.function, entry.name);',
        '',
    ].join('\n');
    const first = matches[0].index;
    const last = matches.at(-1);
    const output = path.join(objects, path.basename(source));
    fs.writeFileSync(output, input.slice(0, first) + table + input.slice(last.index + last[0].length));
    return output;
}

async function build() {
    for (const dir of [objects, staging, dist]) fs.mkdirSync(dir, { recursive: true });
    let completed = 0;
    const targets = sourceNames.map(source => ({
        source, output: path.join(objects, (source.startsWith(h264) ? 'openh264_' : '') + path.basename(source, '.cpp') + '.o'),
    }));
    const headers = ['include', 'persona1', 'src'].flatMap(dir =>
        fs.readdirSync(path.join(repo, dir), { recursive: true })
            .filter(file => /\.(h|hpp|inl)$/.test(file) && !file.startsWith('web' + path.sep))
            .map(file => stamp(path.join(repo, dir, file))));
    headers.push(stamp(path.join(split, 'decls.hpp')), stamp(path.join(persona, 'persona1_display.hpp')));
    const latestHeader = Math.max(...headers);
    const atracHeader = Math.max(...fs.readdirSync(path.join(repo, atrac)).filter(name => name.endsWith('.h'))
        .map(name => stamp(path.join(repo, atrac, name))));
    const h264Header = Math.max(...fs.readdirSync(path.join(repo, h264), { recursive: true })
        .filter(file => /\.(h|hpp)$/.test(file)).map(file => stamp(path.join(repo, h264, file))));
    let next = 0;
    let failure;
    await Promise.all(Array.from({ length: 5 }, async () => {
        while (!failure && next < targets.length) {
            const { source, output } = targets[next++];
            try {
                const usesAtrac = source.startsWith(atrac) || source === "persona1/persona1_audio.cpp" || source === "persona1/persona1_movie.cpp";
                const usesH264 = source.startsWith(h264) || source === "persona1/persona1_movie.cpp";
                const usesStates = /^persona1\/persona1_(main|kernel|io|ge|audio|movie)\.cpp$/.test(source) || source === "persona1/web/states.cpp";
                const stateHeader = usesStates ? stamp(path.join(__dirname,"state-codec.hpp")) : 0;
                const sourceTime = Math.max(stateHeader,latestHeader, stamp(path.join(repo, source)), usesAtrac ? Math.max(atracHeader, stamp(path.join(repo, atracShim, 'ppsspp_config.h')),
                    stamp(path.join(repo, atracShim, 'Common/Log.h'))) : 0, usesH264 ? h264Header : 0);
                const isRegistration = path.basename(source).startsWith('reg_');
                const tableSource = path.join(objects, path.basename(source));
                const tableMissing = isRegistration && !stamp(tableSource);
                if (stamp(output) < sourceTime || tableMissing) {
                    const temp = output + '.partial';
                    await invoke([...baseFlags, ...(usesH264 ? h264Includes : []), ...(usesAtrac ? ['-I' + atracShim] : []), ...(isRegistration ? ['-O0'] : []),
                        '-c', prepareSource(source), '-o', temp]);
                    fs.renameSync(temp, output);
                }
                ++completed;
                if (completed % 10 === 0 || completed === targets.length) console.log('Compiled:', completed, '/', targets.length);
            } catch (error) {
                failure = error;
            }
        }
    }));
    if (failure) throw failure;

    const disc = path.join(repo, '..', "Shin Megami Tensei - Persona (USA).iso");
    fs.accessSync(disc, fs.constants.R_OK);
    const output = path.join(staging, 'persona.mjs');
    await invoke(['-O1', '-pthread', '-fexceptions', '-sPROXY_TO_PTHREAD=1', '-sPTHREAD_POOL_SIZE=4',
        '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=134217728', '-sSTACK_SIZE=5242880', '-sEXIT_RUNTIME=1',
        '-sFORCE_FILESYSTEM=1', '-lidbfs.js', '-sEXPORTED_RUNTIME_METHODS=FS,ENV,UTF8ToString,HEAPU8,HEAP16,addRunDependency,removeRunDependency', '-sEXPORT_NAME=createPersona',
        '-o', output, ...targets.map(target => target.output)]);
    const buildId = require('node:crypto').createHash('sha256').update(fs.readFileSync(path.join(staging,'persona.wasm'))).digest('hex');
    fs.writeFileSync(path.join(dist,'state-build.json'),JSON.stringify({version:1,game:'ULUS10432',build:buildId,compatibleBuilds:JSON.parse(fs.readFileSync(path.join(__dirname,'state-compatible-builds.json'),'utf8'))}));
    for (const file of fs.readdirSync(path.join(__dirname, 'controller-assets'))) {
        fs.copyFileSync(path.join(__dirname, 'controller-assets', file), path.join(dist, file));
    }
    const bundledDisc = path.join(dist, 'Persona.iso');
    fs.copyFileSync(disc, bundledDisc + '.tmp');
    fs.renameSync(bundledDisc + '.tmp', bundledDisc);
    for (const file of fs.readdirSync(staging)) fs.copyFileSync(path.join(staging, file), path.join(dist, file));
    for (const [dir, names] of [[__dirname, ['index.html', 'favicon.png', 'theme.css', 'persona-background.png', 'player.mjs', 'audio.mjs', 'save-store.mjs', 'controls.mjs', 'prompts.mjs', 'states.mjs']], [persona, ['EBOOT_DECRYPTED.ELF', 'nids_persona1.csv']]]) {
        for (const name of names) fs.copyFileSync(path.join(dir, name), path.join(dist, name));
    }
}

build().catch(error => { console.error(error); process.exitCode = 1; });



