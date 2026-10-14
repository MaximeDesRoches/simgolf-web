// Mods (docs/modding.md): installed into this browser from a .zip, managed in
// the Saves panel (Alt+S), and copied into the game's file system before it
// starts, where src/port/fs.c reads them in place of (or next to) the game's
// own files.
//
// Storage is the origin private file system, apart from the installed game:
//   simgolf-mods/<id>/mod.json      the mod's description ({} if it had none)
//   simgolf-mods/<id>/files/...     its files, laid out like the game folder
//   simgolf-mods/state.json         {"order": [ids, later wins], "off": [ids]}
// The game sees the enabled ones as /mods/<id>/files and /mods/order.txt.
// Changes apply the next time the game starts.
(function () {
  'use strict';
  var ROOT = 'simgolf-mods';
  var supported = !!(navigator.storage && navigator.storage.getDirectory);

  function top() {
    return navigator.storage.getDirectory().then(function (d) { return d.getDirectoryHandle(ROOT, { create: true }); });
  }
  async function readJson(dir, name, fallback) {
    try { return JSON.parse(await (await (await dir.getFileHandle(name)).getFile()).text()); } catch (e) { return fallback; }
  }
  async function writeFile(dir, name, data) {
    var w = await (await dir.getFileHandle(name, { create: true })).createWritable();
    await w.write(data);
    await w.close();
  }
  async function state() {
    var root = await top();
    var st = await readJson(root, 'state.json', {});
    st.order = st.order || [];
    st.off = st.off || [];
    // keep only mods that are actually there, and add any that aren't listed
    var ids = [];
    for await (var [name, h] of root.entries()) if (h.kind === 'directory') ids.push(name);
    st.order = st.order.filter(function (id) { return ids.indexOf(id) >= 0; });
    ids.sort().forEach(function (id) { if (st.order.indexOf(id) < 0) st.order.push(id); });
    st.off = st.off.filter(function (id) { return ids.indexOf(id) >= 0; });
    return st;
  }
  async function saveState(st) { await writeFile(await top(), 'state.json', JSON.stringify(st)); }

  // Every installed mod, in load order: {id, name, version, author, description, on}.
  async function list() {
    if (!supported) return [];
    var root = await top(), st = await state(), out = [];
    for (var id of st.order) {
      var info = await readJson(await root.getDirectoryHandle(id), 'mod.json', {});
      out.push({
        id: id, name: String(info.name || id), version: info.version ? String(info.version) : '',
        author: info.author ? String(info.author) : '', description: info.description ? String(info.description) : '',
        on: st.off.indexOf(id) < 0,
      });
    }
    return out;
  }

  // ---- zip (stored and deflate entries; what every zip tool writes)
  async function zipEntries(file) {
    var tailLen = Math.min(file.size, 65557);
    var tail = new DataView(await file.slice(file.size - tailLen).arrayBuffer());
    var eocd = -1;
    for (var i = tailLen - 22; i >= 0; i--) if (tail.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new Error(file.name + ' is not a zip file');
    var n = tail.getUint16(eocd + 10, true);
    var cdSize = tail.getUint32(eocd + 12, true), cdOff = tail.getUint32(eocd + 16, true);
    var cd = new DataView(await file.slice(cdOff, cdOff + cdSize).arrayBuffer());
    var out = [];
    for (var p = 0, k = 0; k < n; k++) {
      var flags = cd.getUint16(p + 8, true), method = cd.getUint16(p + 10, true), csize = cd.getUint32(p + 20, true);
      var nameLen = cd.getUint16(p + 28, true), extra = cd.getUint16(p + 30, true), comment = cd.getUint16(p + 32, true);
      var local = cd.getUint32(p + 42, true);
      var raw = new Uint8Array(cd.buffer, p + 46, nameLen);
      // names are UTF-8 when flag bit 11 is set, otherwise the DOS code page;
      // windows-1252 is close enough for the latter
      var name = new TextDecoder(flags & 0x800 ? 'utf-8' : 'windows-1252').decode(raw).replace(/\\/g, '/');
      out.push({ name: name, method: method, csize: csize, local: local });
      p += 46 + nameLen + extra + comment;
    }
    return out;
  }
  async function zipRead(file, e) {
    var h = new DataView(await file.slice(e.local, e.local + 30).arrayBuffer());
    var start = e.local + 30 + h.getUint16(26, true) + h.getUint16(28, true);
    var data = file.slice(start, start + e.csize);
    if (e.method === 0) return new Uint8Array(await data.arrayBuffer());
    if (e.method !== 8) throw new Error('unsupported compression for ' + e.name);
    return new Uint8Array(await new Response(data.stream().pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer());
  }

  function slug(s) { return String(s).replace(/\.zip$/i, '').replace(/[^A-Za-z0-9._ -]+/g, '_').replace(/^[. ]+|[. ]+$/g, '').slice(0, 64) || 'mod'; }

  // Installs a mod from a .zip: either mod.json + files/ (optionally inside one
  // top folder), or a bare tree laid out like the game folder (Themes/..., as
  // the original theme packs were). Replaces an installed mod with the same id.
  async function install(file) {
    if (!supported) throw new Error('This browser can\'t store mods.');
    var entries = (await zipEntries(file)).filter(function (e) { return !/\/$/.test(e.name) && !/(^|\/)(__MACOSX|\.DS_Store)(\/|$)/.test(e.name); });
    if (!entries.length) throw new Error(file.name + ' is empty');
    // strip a single top folder that holds everything (zipping a folder)
    var first = entries[0].name.split('/')[0], prefix = '';
    if (entries.every(function (e) { return e.name.indexOf(first + '/') === 0; }) &&
        entries.some(function (e) { return /^(mod\.json|files)$/i.test(e.name.split('/')[1]); })) prefix = first + '/';
    var manifest = entries.find(function (e) { return e.name.toLowerCase() === (prefix + 'mod.json').toLowerCase(); });
    var info = {};
    if (manifest) {
      try { info = JSON.parse(new TextDecoder().decode(await zipRead(file, manifest))); } catch (e) { throw new Error('mod.json in ' + file.name + ' isn\'t valid JSON: ' + e.message); }
    }
    var filesPrefix = manifest ? prefix + 'files/' : '';
    var files = entries.filter(function (e) { return e.name.toLowerCase().indexOf(filesPrefix.toLowerCase()) === 0 && e !== manifest; });
    if (!files.length) throw new Error(file.name + ' has no files for the game' + (manifest ? ' (expected a files/ folder next to mod.json)' : ''));
    var id = slug(info.id || file.name);
    var root = await top();
    try { await root.removeEntry(id, { recursive: true }); } catch (e) {}
    var dir = await root.getDirectoryHandle(id, { create: true });
    await writeFile(dir, 'mod.json', JSON.stringify(info.name ? info : Object.assign({ name: file.name.replace(/\.zip$/i, '') }, info)));
    var base = await dir.getDirectoryHandle('files', { create: true });
    for (var e of files) {
      var parts = e.name.slice(filesPrefix.length).split('/').filter(Boolean);
      if (!parts.length || parts.some(function (p) { return p === '..' || p === '.'; })) continue;
      var d = base;
      for (var i = 0; i < parts.length - 1; i++) d = await d.getDirectoryHandle(parts[i], { create: true });
      await writeFile(d, parts[parts.length - 1], await zipRead(file, e));
    }
    var st = await state();
    st.off = st.off.filter(function (x) { return x !== id; });
    await saveState(st);
    return { id: id, name: String(info.name || id), files: files.length };
  }
  async function remove(id) {
    await (await top()).removeEntry(id, { recursive: true });
    await saveState(await state());
  }
  async function setOn(id, on) {
    var st = await state();
    st.off = st.off.filter(function (x) { return x !== id; });
    if (!on) st.off.push(id);
    await saveState(st);
  }
  // Moves a mod one place later (dir 1, so it wins over the next) or earlier (-1).
  async function move(id, dir) {
    var st = await state(), i = st.order.indexOf(id), j = i + dir;
    if (i < 0 || j < 0 || j >= st.order.length) return;
    st.order[i] = st.order[j]; st.order[j] = id;
    await saveState(st);
  }

  // Copies the enabled mods into the game's file system (Emscripten FS).
  async function copyInto(FS) {
    var root = await top(), st = await state(), on = [], n = 0;
    async function walk(dir, path) {
      for await (var [name, h] of dir.entries()) {
        if (h.kind === 'directory') { FS.mkdirTree(path + '/' + name); await walk(h, path + '/' + name); }
        else { FS.writeFile(path + '/' + name, new Uint8Array(await (await h.getFile()).arrayBuffer())); n++; }
      }
    }
    for (var id of st.order) {
      if (st.off.indexOf(id) >= 0) continue;
      var files;
      try { files = await (await root.getDirectoryHandle(id)).getDirectoryHandle('files'); } catch (e) { continue; }
      FS.mkdirTree('/mods/' + id + '/files');
      await walk(files, '/mods/' + id + '/files');
      on.push(id);
    }
    FS.mkdirTree('/mods');
    FS.writeFile('/mods/order.txt', on.join('\n') + '\n');
    loaded = on;
    return n;
  }
  var loaded = [];

  if (supported) {
    var pre = Module.preRun ? [].concat(Module.preRun) : [];
    pre.push(function () {
      addRunDependency('simgolf-mods');
      copyInto(FS).then(function (n) {
        if (loaded.length) console.log('mods: ' + loaded.join(', ') + ' (' + n + ' files)');
      }, function (e) {
        console.warn('mods: ' + e);
      }).finally(function () { removeRunDependency('simgolf-mods'); });
    });
    Module.preRun = pre;
  }

  window.simgolfMods = {
    supported: supported, list: list, install: install, remove: remove, setOn: setOn, move: move,
    // ids the running game started with
    loaded: function () { return loaded.slice(); },
  };
})();
