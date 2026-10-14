// See fs.h.

#include "port/fs.h"

#include <ctype.h>
#include <dirent.h>
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char *path;      // real relative path, '/' separated
  char *lower;     // lowercased key
  unsigned size;
  int fetched;
} Entry;

static Entry *g_entries;
static int g_count;
static int *g_hash;      // index+1, 0 = empty
static int g_hcap;

static unsigned hash_str(const char *s) {
  unsigned h = 2166136261u;
  while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
  return h;
}

static char *lower_dup(const char *s) {
  char *r = strdup(s);
  for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
  return r;
}

static int lookup(const char *lower) {
  if (!g_hcap) return -1;
  unsigned i = hash_str(lower) & (g_hcap - 1);
  while (g_hash[i]) {
    if (!strcmp(g_entries[g_hash[i] - 1].lower, lower)) return g_hash[i] - 1;
    i = (i + 1) & (g_hcap - 1);
  }
  return -1;
}

static void mkdirs(const char *abs) {
  char buf[512];
  snprintf(buf, sizeof buf, "%s", abs);
  for (char *p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(buf, 0777);
      *p = '/';
    }
  }
}

EM_ASYNC_JS(int, js_fetch_game_file, (const char *rel, const char *dest), {
  const r = UTF8ToString(rel);
  const d = UTF8ToString(dest);
  const url = 'game/' + r.split('/').map(encodeURIComponent).join('/');
  if (Module.fsDebug) console.log('fetch ' + url);
  try {
    const resp = await fetch(url);
    if (!resp.ok) { console.warn('fetch ' + url + ': ' + resp.status); return 0; }
    const buf = new Uint8Array(await resp.arrayBuffer());
    FS.writeFile(d, buf);
    return 1;
  } catch (e) {
    console.warn('fetch ' + url + ': ' + e);
    return 0;
  }
});

// Files the game writes (saves, autosaves, the player profile) persist in
// IndexedDB: /persist mirrors the written files under /game, is loaded over the
// game files at start and is synced shortly after each write.
//
// They also follow the player between browsers: the dev server keeps a copy
// under saves/ (tools/boxedwine/serve.mjs). Each write is pushed, and at start
// files the server has newer copies of are pulled into /persist first. A file's
// mtime in /persist is the server's time of the copy it matches, so "newer"
// compares server times on both sides; a local copy overwritten by a pull goes
// into web/autosave.js's history. Without the server (another static host,
// offline) this is skipped and saves stay local.
EM_ASYNC_JS(int, js_persist_load, (void), {
  const sync = Module.saveSync = { server: false, last: 0, error: "" };
  const url = (rel) => 'saves/' + rel.split('/').map(encodeURIComponent).join('/');
  const mtime = (p) => new Date(FS.stat(p).mtime).getTime();
  const mkdirs = (p) => {
    let dir = "";
    for (const part of p.split('/').slice(1, -1)) {
      dir += '/' + part;
      try { FS.mkdir(dir); } catch (e) {}
    }
  };
  let pushes = Promise.resolve();
  const push = (rel, deleted) => {
    if (!sync.server) return;
    pushes = pushes.then(async () => {
      const p = '/persist/' + rel;
      let data = null;
      if (!deleted) { try { data = FS.readFile(p); } catch (e) { return; } }
      const r = await fetch(url(rel), { method: deleted ? 'DELETE' : 'PUT', body: data });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const t = (await r.json()).mtime;
      // stamp the copy with the server's time, unless it changed meanwhile
      if (!deleted) {
        try {
          const now = FS.readFile(p);
          if (now.length === data.length && now.every((b, i) => b === data[i])) FS.utime(p, t, t);
        } catch (e) {}
      }
      sync.last = Date.now(); sync.error = "";
      clearTimeout(Module.persistTimer);
      Module.persistTimer = setTimeout(Module.persistFlush, 500);
    }).catch((e) => { sync.error = String(e); err('fs: save sync ' + rel + ': ' + e); });
  };
  Module.persistGameFile = (path, deleted) => {
    if (!path.startsWith('/game/')) return;
    const dst = '/persist' + path.slice(5);
    let data;
    try {
      if (deleted) {
        try { FS.unlink(dst); } catch (e) {}
      } else {
        mkdirs(dst);
        data = FS.readFile(path);
        FS.writeFile(dst, data);
      }
    } catch (e) { err('fs: persist ' + path + ': ' + e); return; }
    push(path.slice(6), deleted);
    clearTimeout(Module.persistTimer);
    Module.persistTimer = setTimeout(Module.persistFlush, 500);
    // web/autosave.js keeps its own history of saves
    if (data && Module.onGameFileWritten) Module.onGameFileWritten(path.slice(6), data);
  };
  Module.persistFlush = () => {
    clearTimeout(Module.persistTimer);
    FS.syncfs(false, (e) => { if (e) err('fs: sync: ' + e); });
  };
  // For web/autosave.js: puts a .sve into the game's save folder.
  Module.saveImport = (name, bytes) => {
    const dir = '/game/' + (FS.readdir('/game').find((d) => d.toLowerCase() === 'saved games') || 'Saved Games');
    try { FS.mkdir(dir); } catch (e) {}
    FS.writeFile(dir + '/' + name, bytes);
    Module.persistGameFile(dir + '/' + name, 0);
  };
  try {
    FS.mkdir('/persist');
    FS.mount(IDBFS, {}, '/persist');
    await new Promise((res, rej) => FS.syncfs(true, (e) => e ? rej(e) : res()));
  } catch (e) { err('fs: persistent storage unavailable: ' + e); return 0; }

  const local = {};
  const walk = (dir, rel) => {
    for (const name of FS.readdir(dir)) {
      if (name === '.' || name === '..') continue;
      const p = dir + '/' + name, r = rel ? rel + '/' + name : name;
      if (FS.isDir(FS.stat(p).mode)) walk(p, r); else local[r] = mtime(p);
    }
  };
  walk('/persist', "");
  let remote = null;
  try {
    const r = await fetch('saves/', { cache: 'no-store' });
    if (r.ok && /json/.test(r.headers.get('content-type') || "")) remote = await r.json();
  } catch (e) {}
  if (remote) {
    sync.server = true;
    let pulled = 0, removed = 0;
    for (const [rel, t] of Object.entries(remote.files)) {
      if (rel in local && local[rel] >= t) continue;
      try {
        const r = await fetch(url(rel), { cache: 'no-store' });
        if (!r.ok) continue;
        const data = new Uint8Array(await r.arrayBuffer());
        const p = '/persist/' + rel;
        if (rel in local && Module.onGameFileWritten) {
          const old = FS.readFile(p);
          if (old.length !== data.length || !old.every((b, i) => b === data[i])) Module.onGameFileWritten(rel, old);
        }
        mkdirs(p);
        FS.writeFile(p, data);
        FS.utime(p, t, t);
        local[rel] = t;
        pulled++;
      } catch (e) { err('fs: save sync ' + rel + ': ' + e); }
    }
    for (const [rel, t] of Object.entries(remote.deleted || {})) {
      if (rel in local && local[rel] < t) {
        try { FS.unlink('/persist/' + rel); } catch (e) {}
        try { FS.unlink('/game/' + rel); } catch (e) {}
        delete local[rel];
        removed++;
      }
    }
    // files this browser has that the server lacks or has older copies of
    for (const rel of Object.keys(local)) {
      if (!(rel in remote.files) || local[rel] > remote.files[rel]) {
        if (!(rel in (remote.deleted || {})) || local[rel] > remote.deleted[rel]) push(rel, 0);
      }
    }
    if (pulled || removed) {
      out('fs: ' + pulled + ' saved files from the server, ' + removed + ' removed');
      sync.last = Date.now();
      Module.persistFlush();
    }
  }

  let n = 0;
  const copy = (src, dst) => {
    for (const name of FS.readdir(src)) {
      if (name === '.' || name === '..') continue;
      const s = src + '/' + name, d = dst + '/' + name;
      if (FS.isDir(FS.stat(s).mode)) {
        try { FS.mkdir(d); } catch (e) {}
        copy(s, d);
      } else {
        FS.writeFile(d, FS.readFile(s));
        n++;
      }
    }
  };
  copy('/persist', '/game');
  return n;
});

EM_JS(void, js_persist_file, (const char *real, int deleted), {
  if (Module.persistGameFile) Module.persistGameFile(UTF8ToString(real), deleted);
});

void fs_written(const char *real) {
  // the debug log is rewritten at every start; not worth keeping
  const char *base = strrchr(real, '/');
  if (base && !strcasecmp(base + 1, "logfile.txt")) return;
  js_persist_file(real, 0);
}

void fs_deleted(const char *real) { js_persist_file(real, 1); }

// Mods (docs/modding.md). Each enabled mod is a folder /mods/<id>/files laid
// out like the game folder; its files are read in place of the game's (or
// added next to them) and show up in directory listings. /mods/order.txt lists
// the enabled ids, one per line, later ones winning; without it every folder
// in /mods is loaded in name order. The web page copies the enabled mods there
// from its own storage; the desktop app maps /mods to its mods folder.
// Writes always go to the game folder.
typedef struct {
  char *rel;    // path relative to the mod's files folder, '/' separated
  char *lower;  // lowercased rel: the lookup key
  char *real;   // absolute path
  unsigned size;
} ModFile;

static ModFile *g_mods;
static int g_mod_count, g_mod_cap;
static int *g_mod_hash;  // index+1, 0 = empty
static int g_mod_hcap;

static int mod_lookup(const char *lower) {
  if (!g_mod_hcap) return -1;
  unsigned i = hash_str(lower) & (g_mod_hcap - 1);
  while (g_mod_hash[i]) {
    if (!strcmp(g_mods[g_mod_hash[i] - 1].lower, lower)) return g_mod_hash[i] - 1;
    i = (i + 1) & (g_mod_hcap - 1);
  }
  return -1;
}

static void mod_rehash(void) {
  free(g_mod_hash);
  g_mod_hcap = g_mod_hcap ? g_mod_hcap * 2 : 128;
  g_mod_hash = calloc(g_mod_hcap, sizeof(int));
  for (int i = 0; i < g_mod_count; i++) {
    unsigned h = hash_str(g_mods[i].lower) & (g_mod_hcap - 1);
    while (g_mod_hash[h]) h = (h + 1) & (g_mod_hcap - 1);
    g_mod_hash[h] = i + 1;
  }
}

static void mod_add(const char *rel, const char *real, unsigned size) {
  char *low = lower_dup(rel);
  int i = mod_lookup(low);
  if (i >= 0) {  // a later mod replaces an earlier one's file
    free(low);
    free(g_mods[i].real);
    g_mods[i].real = strdup(real);
    g_mods[i].size = size;
    return;
  }
  if (g_mod_count == g_mod_cap) {
    g_mod_cap = g_mod_cap ? g_mod_cap * 2 : 64;
    g_mods = realloc(g_mods, g_mod_cap * sizeof(ModFile));
  }
  ModFile *m = &g_mods[g_mod_count++];
  m->rel = strdup(rel);
  m->lower = low;
  m->real = strdup(real);
  m->size = size;
  if (g_mod_count * 2 > g_mod_hcap) {
    mod_rehash();
  } else {
    unsigned h = hash_str(low) & (g_mod_hcap - 1);
    while (g_mod_hash[h]) h = (h + 1) & (g_mod_hcap - 1);
    g_mod_hash[h] = g_mod_count;
  }
}

static void mod_walk(const char *abs, const char *rel) {
  DIR *d = opendir(abs);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.') continue;  // ., .., .DS_Store
    char a[600], r[512];
    snprintf(a, sizeof a, "%s/%s", abs, de->d_name);
    snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
    struct stat st;
    if (stat(a, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) mod_walk(a, r);
    else if (*rel || (strcasecmp(de->d_name, "mod.json") && strcasecmp(de->d_name, "order.txt"))) mod_add(r, a, (unsigned)st.st_size);
  }
  closedir(d);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void mod_load(const char *id) {
  if (!*id || strchr(id, '/') || !strcmp(id, "..")) return;
  // A mod's files sit in its files/ folder next to mod.json. A folder without
  // files/ is used as is, so an old theme pack unzipped into mods/ (Themes/...)
  // works too.
  char abs[600];
  struct stat st;
  snprintf(abs, sizeof abs, FS_MODS_ROOT "/%s/files", id);
  if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) snprintf(abs, sizeof abs, FS_MODS_ROOT "/%s", id);
  int before = g_mod_count;
  mod_walk(abs, "");
  fprintf(stderr, "fs: mod %s: %d files\n", id, g_mod_count - before);
}

static void mods_init(void) {
  FILE *f = fopen(FS_MODS_ROOT "/order.txt", "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof line, f)) {
      line[strcspn(line, "\r\n")] = 0;
      if (line[0] != '#') mod_load(line);
    }
    fclose(f);
  } else {
    DIR *d = opendir(FS_MODS_ROOT);
    if (!d) return;
    char **ids = NULL;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
      if (de->d_name[0] == '.') continue;
      char p[600];
      struct stat st;
      snprintf(p, sizeof p, FS_MODS_ROOT "/%s", de->d_name);
      if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
      ids = realloc(ids, (n + 1) * sizeof(char *));
      ids[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(ids, n, sizeof(char *), cmp_str);
    for (int i = 0; i < n; i++) {
      mod_load(ids[i]);
      free(ids[i]);
    }
    free(ids);
  }
}

void fs_init(void) {
  mods_init();
  mkdir(FS_GAME_ROOT, 0777);
  // No manifest in the native desktop build: the game files are on disk.
  FILE *f = fopen("/game.manifest", "r");
  if (!f) fprintf(stderr, "fs: no /game.manifest, using the files in " FS_GAME_ROOT "\n");
  char line[600];
  int cap = 0;
  while (f && fgets(line, sizeof line, f)) {
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    if (g_count == cap) {
      cap = cap ? cap * 2 : 1024;
      g_entries = realloc(g_entries, cap * sizeof(Entry));
    }
    Entry *e = &g_entries[g_count++];
    e->path = strdup(line);
    e->lower = lower_dup(line);
    e->size = (unsigned)strtoul(tab + 1, NULL, 10);
    e->fetched = 0;
  }
  if (f) fclose(f);
  g_hcap = 1;
  while (g_hcap < g_count * 2) g_hcap <<= 1;
  g_hash = calloc(g_hcap, sizeof(int));
  for (int i = 0; i < g_count; i++) {
    unsigned h = hash_str(g_entries[i].lower) & (g_hcap - 1);
    while (g_hash[h]) h = (h + 1) & (g_hcap - 1);
    g_hash[h] = i + 1;
    char abs[600];
    snprintf(abs, sizeof abs, FS_GAME_ROOT "/%s", g_entries[i].path);
    mkdirs(abs);
  }
  // The game writes into these but never creates them; the retail installer
  // does. Without "Saved Games" every save and autosave fails, which is the
  // case for the Pages build (its install holds files only).
  mkdir(FS_GAME_ROOT "/Saved Games", 0777);
  mkdir(FS_GAME_ROOT "/Snapshots", 0777);
  chdir(FS_GAME_ROOT);
  fprintf(stderr, "fs: %d game files in manifest\n", g_count);
  int saved = js_persist_load();
  if (saved) fprintf(stderr, "fs: %d saved files restored\n", saved);
}

// DOS path -> normalized relative path (no leading slash, '/' separated).
static void normalize(const char *dos, char *out, int outlen) {
  char tmp[512];
  int j = 0;
  for (const char *p = dos; *p && j < (int)sizeof tmp - 1; p++) tmp[j++] = *p == '\\' ? '/' : *p;
  tmp[j] = 0;
  const char *s = tmp;
  if (isalpha((unsigned char)s[0]) && s[1] == ':') s += 2;
  // The port reports C:\SimGolf as the install folder (GetModuleFileName).
  if (!strncasecmp(s, "/simgolf/", 9)) s += 9;
  if (!strncasecmp(s, FS_GAME_ROOT "/", strlen(FS_GAME_ROOT) + 1)) s += strlen(FS_GAME_ROOT) + 1;
  while (*s == '/') s++;
  int k = 0;
  while (*s && k < outlen - 1) {
    if (s[0] == '.' && s[1] == '/') { s += 2; continue; }
    if (s[0] == '/' && k > 0 && out[k - 1] == '/') { s++; continue; }
    out[k++] = *s++;
  }
  out[k] = 0;
}

// Case-insensitive match of each path component against what's in the FS.
static int walk_fs(const char *rel, char *out, int outlen, int allow_missing_leaf) {
  char work[512];
  snprintf(work, sizeof work, "%s", rel);
  snprintf(out, outlen, "%s", FS_GAME_ROOT);
  char *save = NULL;
  char *comp = strtok_r(work, "/", &save);
  while (comp) {
    char *next = strtok_r(NULL, "/", &save);
    DIR *d = opendir(out);
    int found = 0;
    if (d) {
      struct dirent *de;
      while ((de = readdir(d))) {
        if (!strcasecmp(de->d_name, comp)) {
          size_t l = strlen(out);
          snprintf(out + l, outlen - l, "/%s", de->d_name);
          found = 1;
          break;
        }
      }
      closedir(d);
    }
    if (!found) {
      if (next || !allow_missing_leaf) return 0;
      size_t l = strlen(out);
      snprintf(out + l, outlen - l, "/%s", comp);
    }
    comp = next;
  }
  return 1;
}

int fs_resolve(const char *dospath, char *out, int outlen, int for_write) {
  if (!dospath) return 0;
  char rel[512];
  normalize(dospath, rel, sizeof rel);
  char *low = lower_dup(rel);
  if (!for_write) {
    int m = mod_lookup(low);
    if (m >= 0) {
      free(low);
      snprintf(out, outlen, "%s", g_mods[m].real);
      return 1;
    }
  }
  int i = lookup(low);
  free(low);
  if (i >= 0) {
    Entry *e = &g_entries[i];
    snprintf(out, outlen, FS_GAME_ROOT "/%s", e->path);
    if (!e->fetched) {
      struct stat st;
      if (stat(out, &st) != 0) {
        if (!js_fetch_game_file(e->path, out)) {
          if (!for_write) return 0;
        }
      }
      e->fetched = 1;
    }
    return 1;
  }
  return walk_fs(rel, out, outlen, for_write);
}

static int wild_match(const char *pat, const char *s) {
  if (!*pat) return !*s;
  if (*pat == '*') {
    if (!strcmp(pat, "*.*")) return 1;
    for (;; s++) {
      if (wild_match(pat + 1, s)) return 1;
      if (!*s) return 0;
    }
  }
  if (!*s) return 0;
  if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s))
    return wild_match(pat + 1, s + 1);
  return 0;
}

// Names already reported by fs_list (case-insensitive, as on Windows).
typedef struct {
  char **names;
  int n, cap;
} Seen;

static int seen_add(Seen *s, const char *name) {
  for (int k = 0; k < s->n; k++)
    if (!strcasecmp(s->names[k], name)) return 0;
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 256;
    s->names = realloc(s->names, s->cap * sizeof(char *));
  }
  s->names[s->n++] = strdup(name);
  return 1;
}

// Whether some mod has files under `dir` (game-relative, '/' separated).
static int mod_has_dir(const char *dir) {
  char *ldir = lower_dup(dir);
  size_t dl = strlen(ldir);
  int found = 0;
  for (int i = 0; i < g_mod_count && !found; i++)
    found = !strncmp(g_mods[i].lower, ldir, dl) && g_mods[i].lower[dl] == '/';
  free(ldir);
  return found;
}

int fs_list(const char *dospattern, fs_list_fn fn, void *ctx) {
  char rel[512];
  normalize(dospattern, rel, sizeof rel);
  char *slash = strrchr(rel, '/');
  const char *pat = slash ? slash + 1 : rel;
  char dir[512] = "";
  if (slash) {
    memcpy(dir, rel, slash - rel);
    dir[slash - rel] = 0;
  }
  int n = 0;
  Seen seen = {0};
  char absdir[600];
  int have_dir = 1;
  if (dir[0]) have_dir = walk_fs(dir, absdir, sizeof absdir, 0) || mod_has_dir(dir);
  else snprintf(absdir, sizeof absdir, "%s", FS_GAME_ROOT);
  // Windows lists "." and ".." first in a subfolder, and the theme screen
  // (FUN_004725b0) skips the first entry of Themes\*.* on that basis.
  if (dir[0] && have_dir && wild_match(pat, ".")) {
    seen_add(&seen, ".");
    seen_add(&seen, "..");
    fn(ctx, ".", 1, 0);
    fn(ctx, "..", 1, 0);
    n += 2;
  }
  if (have_dir && (!dir[0] || walk_fs(dir, absdir, sizeof absdir, 0))) {
    // Everything in the FS directory: subdirectories, fetched and written
    // files. Unfetched manifest files exist as names only, so list those
    // separately.
    DIR *d = opendir(absdir);
    if (d) {
      struct dirent *de;
      while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!wild_match(pat, de->d_name) || !seen_add(&seen, de->d_name)) continue;
        char p[800];
        snprintf(p, sizeof p, "%s/%s", absdir, de->d_name);
        struct stat st;
        stat(p, &st);
        fn(ctx, de->d_name, S_ISDIR(st.st_mode), (unsigned)st.st_size);
        n++;
      }
      closedir(d);
    }
    const char *reldir = absdir + strlen(FS_GAME_ROOT);
    if (*reldir == '/') reldir++;
    size_t dl = strlen(reldir);
    for (int i = 0; i < g_count; i++) {
      const char *p = g_entries[i].path;
      if (dl && (strncmp(p, reldir, dl) || p[dl] != '/')) continue;
      const char *name = dl ? p + dl + 1 : p;
      if (strchr(name, '/')) continue;
      if (!wild_match(pat, name) || !seen_add(&seen, name)) continue;
      fn(ctx, name, 0, g_entries[i].size);
      n++;
    }
  }
  // Files and folders only mods have.
  if (g_mod_count) {
    char *ldir = lower_dup(dir);
    size_t dl = strlen(ldir);
    for (int i = 0; i < g_mod_count; i++) {
      const ModFile *m = &g_mods[i];
      if (dl && (strncmp(m->lower, ldir, dl) || m->lower[dl] != '/')) continue;
      const char *name = m->rel + (dl ? dl + 1 : 0);
      const char *sub = strchr(name, '/');
      char first[256];
      snprintf(first, sizeof first, "%.*s", sub ? (int)(sub - name) : (int)strlen(name), name);
      if (!wild_match(pat, first) || !seen_add(&seen, first)) continue;
      fn(ctx, first, sub != NULL, sub ? 0 : m->size);
      n++;
    }
    free(ldir);
  }
  for (int k = 0; k < seen.n; k++) free(seen.names[k]);
  free(seen.names);
  return n;
}
