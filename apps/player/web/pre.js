// Browser build: what a page can hand to px-player before it starts.
//
// Module.arguments: command-line arguments. Pages that set none may pass
// them in the address instead (used by automated tests), e.g.
//   px-player.html?arg=--frames&arg=930&arg=--input-script&arg=/examples/platformer/tests/full_run.input
// Pages that set Module.noUrlArguments (the platform's /play page) never take
// arguments from the address: a link must not choose what the player
// connects to.
//
// Module.pxPackage: a Promise of an ArrayBuffer holding a game package; it is
// written to /game.pxpkg before main() runs, so the page can download the
// game while the player itself loads.
Module['arguments'] = Module['arguments'] || [];
if (typeof location !== 'undefined' && !Module['noUrlArguments']) {
  for (const value of new URLSearchParams(location.search).getAll('arg')) {
    Module['arguments'].push(value);
  }
}
if (Module['pxPackage']) {
  const preRun = Module['preRun'];
  Module['preRun'] = preRun ? [].concat(preRun) : [];
  Module['preRun'].push(() => {
    addRunDependency('px-package');
    Promise.resolve(Module['pxPackage']).then(
      (bytes) => {
        FS.writeFile('/game.pxpkg', new Uint8Array(bytes));
        removeRunDependency('px-package');
      },
      (error) => {
        const text = 'Could not get the game: ' + (error && error.message ? error.message : error);
        if (Module['printErr']) Module['printErr'](text);
        abort(text);
      });
  });
}
