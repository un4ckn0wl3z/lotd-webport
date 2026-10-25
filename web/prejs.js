// Before main(): mount the IndexedDB-backed /save directory (settings, profiles, saved games), and pass the page's
// debugging switches (?env=PORT_SEH=1,PORT_SHOT=100) to the port as environment variables.
Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
  if (Module.portEnv) Module.portEnv.split(',').forEach(function (kv) {
    var i = kv.indexOf('=');
    if (i > 0) ENV[kv.slice(0, i)] = kv.slice(i + 1);
  });
  FS.mkdir('/save');
  FS.mount(IDBFS, {}, '/save');
  addRunDependency('idbfs-load');
  FS.syncfs(true, function (err) {
    if (err) console.warn('could not load saves', err);
    removeRunDependency('idbfs-load');
  });
});
